#include "gpu.h"

#include <algorithm>
#include <cstddef>

#include "savestate.h"

namespace {

// The word that ends a polyline. Software terminates the vertex list
// with it rather than giving a count up front.
constexpr u32 POLYLINE_END = 0x50005000;
constexpr u32 POLYLINE_END_MASK = 0xF000F000;

// The parts of GP0(E1h) a textured primitive carries with it: where its
// texture lives, and whether it has one at all.
constexpr u32 DRAW_MODE_TEXTURE_PAGE = 0x1FF;
constexpr u32 DRAW_MODE_TEXTURE_DISABLE = 1u << 11;

// GP0(E6h)'s two bits, and the pixel bit they are about.
constexpr u32 MASK_SET_WHEN_DRAWING = 1u << 0;
constexpr u32 MASK_CHECK_BEFORE_DRAW = 1u << 1;
constexpr u16 MASK_BIT = 0x8000;

// A transfer's width and height are encoded so that zero means the
// maximum: 0 pixels would be a pointless transfer, 1024 (or 512) does
// not fit the field, so the value wraps into it.
u32 transfer_extent(u32 value, u32 limit)
{
    return ((value - 1) & (limit - 1)) + 1;
}

// Coordinates in drawing commands are eleven-bit signed fields packed
// into a word, so the sign has to be put back by hand.
s32 sign_extend_11(u32 value)
{
    const u32 low = value & 0x7FF;
    return static_cast<s32>(low < 0x400 ? low : low - 0x800);
}

// What the console spends on one pixel, in CPU cycles scaled by
// PIXEL_COST_SCALE so that fractions of a cycle can be written as
// whole numbers. Every figure is gpu/bandwidth's log divided by the
// pixels that test covers: the flat opaque primitive is the anchor and
// the rest are the ratios the log measures, which is why they are here
// as measurements rather than as a formula.
constexpr u32 PIXEL_COST_SCALE = 1024;

constexpr u32 COST_FILL = 85;   // GP0(02h), which skips every stage
constexpr u32 COST_FLAT = 552;  // one colour, straight into VRAM
constexpr u32 COST_SEMI = 833;  // ... read back and blended first

// A textured rectangle and a textured polygon are nowhere near each
// other — twice a flat pixel against five times — and the difference
// is the texture cache. A sprite walks its texture one texel to the
// pixel, along the row, so almost every read is already in the cache;
// a polygon interpolates in two dimensions and misses it constantly.
// Modelling the cache itself is a long way beyond this, so the two
// paths carry the cost the log gives them.
constexpr u32 COST_TEXTURED_RECT = 1093;
constexpr u32 COST_TEXTURED_POLY = 2889;

// The three ways a pixel moves rather than being drawn. Only two of
// them cost anything here, and that is a measurement rather than an
// omission: a pixel coming in from the CPU arrives no faster than
// whatever is sending it, and both dma/chopping and gpu/bandwidth put
// a console's way in at the speed of the DMA controller alone. The
// controller is already charged for that, so charging the GPU again
// counts it twice — dma/chopping takes half as long again as a console
// the moment it does.
//
// The way out is not the same. A console reads VRAM back close to a
// third slower than it writes it, on the same channel at the same
// block size, and that difference is the GPU's: the pixels have to be
// out of VRAM before the controller has anything to move.
constexpr u32 COST_VRAM_LOAD = 0;     // arriving from the CPU
constexpr u32 COST_VRAM_STORE = 242;  // going back to it
constexpr u32 COST_VRAM_COPY = 1383;  // VRAM to VRAM

// The flag bits every drawing command shares, in the low five bits of
// its command byte. Not all of them apply to every family — a
// rectangle is never shaded across, and an untextured primitive has no
// palette — but reading them all here costs nothing.
Shading shading_of(u32 op)
{
    Shading how;
    how.raw = (op & 0x01) != 0;
    how.translucent = (op & 0x02) != 0;
    how.textured = (op & 0x04) != 0;
    how.gouraud = (op & 0x10) != 0 && op < 0x60;
    return how;
}

// What a pixel of this primitive costs. Semi-transparency is only ever
// charged for on an untextured one: a textured pixel already goes
// through the blender to get there, and the log times the two rows to
// the hblank.
u32 pixel_cost(const Shading& how, bool polygon)
{
    if (how.textured) {
        return polygon ? COST_TEXTURED_POLY : COST_TEXTURED_RECT;
    }
    return how.translucent ? COST_SEMI : COST_FLAT;
}

}  // namespace

u32 gp0_length(u32 command)
{
    const u32 op = command >> 24;

    // The command byte's top three bits pick the family, and the rest
    // of it modifies the shape within that family.
    switch (op >> 5) {
    case 0:  // 00h..1Fh, miscellaneous
        // Only the rectangle fill carries parameters; the rest, NOP
        // and cache clear and the interrupt request, are bare.
        return op == 0x02 ? 3 : 1;

    case 1: {  // 20h..3Fh, polygons
        const u32 vertices = (op & 0x08) ? 4 : 3;
        const u32 words_per_vertex = (op & 0x04) ? 2 : 1;  // + texcoord
        // Gouraud shading gives every vertex after the first its own
        // colour word; the first one shares the command word.
        const u32 extra_colours = (op & 0x10) ? vertices - 1 : 0;
        return 1 + vertices * words_per_vertex + extra_colours;
    }

    case 2: {  // 40h..5Fh, lines
        // A polyline's length is not known here — it runs until the
        // terminator — so this is the two-vertex minimum and the rest
        // is consumed in PolyLine mode.
        const u32 extra_colour = (op & 0x10) ? 1 : 0;
        return 3 + extra_colour;
    }

    case 3: {  // 60h..7Fh, rectangles
        // Bits 4..3 give a fixed size of 1x1, 8x8 or 16x16; zero means
        // the size comes as a further word.
        const u32 sized_by_word = ((op >> 3) & 3) == 0 ? 1 : 0;
        const u32 texcoord = (op & 0x04) ? 1 : 0;
        return 2 + sized_by_word + texcoord;
    }

    case 4:  // 80h..9Fh, VRAM to VRAM: source, destination, size
        return 4;

    case 5:  // A0h..BFh, CPU to VRAM: destination, size, then pixels
    case 6:  // C0h..DFh, VRAM to CPU: source, size, then GPUREAD
        return 3;

    default:  // E0h..FFh, drawing settings, one word each
        return 1;
    }
}

s32 Gpu::offset_x() const { return sign_extend_11(draw_offset); }

s32 Gpu::offset_y() const { return sign_extend_11(draw_offset >> 11); }

u32 Gpu::display_width() const
{
    // Bit 6 asks for 368 pixels and overrules the two-bit field below
    // it, which is the odd one out because it was added late.
    if (((display_mode >> 6) & 1) != 0) {
        return 368;
    }
    constexpr std::array<u32, 4> WIDTHS = {256, 320, 512, 640};
    return WIDTHS[display_mode & 3];
}

u32 Gpu::dot_cycles() const
{
    if (((display_mode >> 6) & 1) != 0) {
        return 7;  // the 368-pixel mode, again out on its own
    }
    constexpr std::array<u32, 4> CYCLES = {10, 8, 5, 4};
    return CYCLES[display_mode & 3];
}

u64 Gpu::hblank_cycles() const
{
    // GP1(06h) carries the two edges of the window as positions along
    // the scanline, measured in the GPU's own cycles. Everything the
    // scanline has outside them is blanking.
    const u32 left = display_range_x & 0xFFF;
    const u32 right = (display_range_x >> 12) & 0xFFF;
    const u32 window = right > left ? right - left : 0;
    const u64 active = std::min<u64>(window, GPU_CYCLES_PER_SCANLINE);
    return (GPU_CYCLES_PER_SCANLINE - active) * CYCLES_PER_SCANLINE /
        GPU_CYCLES_PER_SCANLINE;
}

u32 Gpu::display_height() const
{
    // The taller mode only exists interlaced: it is the two fields
    // together, so without interlacing there is only ever one of them.
    const bool tall = ((display_mode >> 2) & 1) != 0;
    return tall && interlaced() ? 480 : 240;
}

// Scanlines are counted from vsync, and the middle of the frame — the
// scanline the picture is centred on — is 88h on NTSC and A3h on PAL.
// A picture of the full height reaches half of itself either side of
// it, which is where GP1(07h) is left by a reset.
u32 Gpu::first_scanline() const
{
    constexpr u32 NTSC_MIDDLE = 0x88;
    constexpr u32 PAL_MIDDLE = 0xA3;
    constexpr u32 NTSC_LINES = 240;
    constexpr u32 PAL_LINES = 288;
    if (pal()) {
        return PAL_MIDDLE - PAL_LINES / 2;
    }
    return NTSC_MIDDLE - NTSC_LINES / 2;
}

Gpu::Colour Gpu::display_pixel(u32 x, u32 y) const
{
    if (x >= display_width() || y >= display_height()) {
        return {};
    }

    // GP1(07h) says which scanlines of the frame the picture is put
    // on, and it is as tall as they are many. Asking for fewer than
    // the frame holds leaves the rest of it black rather than reading
    // further down VRAM: the BIOS asks for 239 of them, which is 478
    // of the 480 lines its logo screen occupies, and taking the other
    // two out of VRAM found the font stored under the picture and drew
    // it along the bottom as a row of coloured dashes. A game shaking
    // the screen moves this range rather than what it drew, so where
    // the picture starts matters as much as how tall it is.
    const s32 first = static_cast<s32>(first_scanline());
    const s32 range_start = static_cast<s32>(display_range_y & 0x3FF);
    const s32 range_end = static_cast<s32>((display_range_y >> 10) & 0x3FF);

    // A scanline is one line of the picture, or two of them where the
    // two fields are shown at once.
    const s32 lines_each = display_height() > 240 ? 2 : 1;
    const s32 top = (range_start - first) * lines_each;
    const s32 height = (range_end - range_start) * lines_each;

    // Which line of the picture this line of the frame shows. Above
    // the picture there is nothing to show, and a picture starting
    // above the frame has its first lines cut off rather than moved.
    const s32 line_of_picture = static_cast<s32>(y) - top;
    if (line_of_picture < 0 || line_of_picture >= height) {
        return {};
    }

    const u32 start_x = display_start & 0x3FF;
    const u32 start_y = (display_start >> 10) & 0x1FF;
    const u32 vram_line = start_y + static_cast<u32>(line_of_picture);
    const std::size_t line = std::size_t{vram_line % VRAM_HEIGHT} * VRAM_WIDTH;

    if (colour_24bit()) {
        // Three bytes to the pixel, laid end to end across the
        // halfwords rather than packed inside them, so the line is
        // addressed in bytes and every third pixel straddles a
        // halfword boundary.
        const auto byte_at = [&](u32 offset) {
            const u32 within = (start_x * 2 + offset) % (VRAM_WIDTH * 2);
            const u16 pair = vram[line + within / 2];
            return static_cast<u8>(pair >> ((within & 1) * 8));
        };
        return {byte_at(x * 3), byte_at(x * 3 + 1), byte_at(x * 3 + 2)};
    }

    const u16 pixel = vram[line + (start_x + x) % VRAM_WIDTH];

    // Five bits to eight, with the top bits repeated into the bottom
    // ones so that a full-brightness channel comes out full rather
    // than a little short of it.
    const auto stretch = [](u32 value) {
        return static_cast<u8>((value << 3) | (value >> 2));
    };
    return {stretch(pixel & 0x1F),
            stretch((pixel >> 5) & 0x1F),
            stretch((pixel >> 10) & 0x1F)};
}

void Gpu::visit_state(State& state)
{
    state(vram);
    state(mode);
    state(transfer.x);
    state(transfer.y);
    state(transfer.width);
    state(transfer.height);
    state(transfer.pixels_done);
    state(draw_mode);
    state(texture_window);
    state(draw_area_top);
    state(draw_area_bottom);
    state(draw_offset);
    state(mask_setting);
    state(display_mode);
    state(allow_texture_disable);
    state(display_start);
    state(display_range_x);
    state(display_range_y);
    state(dma_direction);
    state(display_disabled);
    state(irq);
    state(scanline);
    state(odd_field);
    state(gpuread_latch);
    state(command);
    state(command_words);
    state(polyline_x);
    state(polyline_y);
    state(polyline_colour);
    state(polyline_next_colour);
    state(polyline_has_colour);
    state(busy_until);
    state(fifo_words);
    state(cost_owed);
}

void Gpu::reset()
{
    // GP1(00h)'s effect, and the power-on state: blanked, no transfer
    // in progress, DMA off, every drawing register cleared.
    mode = Gp0Mode::Command;
    transfer = Transfer{};
    command_words = 0;
    busy_until = 0;
    fifo_words = 0;
    cost_owed = 0;
    pixels_drawn = 0;
    draw_mode = 0;
    texture_window = 0;
    draw_area_top = 0;
    draw_area_bottom = 0;
    draw_offset = 0;
    mask_setting = 0;
    display_mode = 0;
    allow_texture_disable = false;
    display_start = 0;
    // A reset leaves the ranges where a full-height picture on an NTSC
    // set wants them, which is the one part of the display it does not
    // simply zero.
    display_range_x = 0x00C00200;  // 200h to 200h+256*10
    display_range_y = 0x00040010;  // 010h to 010h+240
    dma_direction = DmaDirection::Off;
    display_disabled = true;
    irq = false;
    scanline = 0;
    odd_field = false;
    gpuread_latch = 0;
    polyline_x = 0;
    polyline_y = 0;
    polyline_colour = 0;
    polyline_next_colour = 0;
    polyline_has_colour = false;
    // VRAM survives a reset on hardware, so it is not cleared here.
}

bool Gpu::next_scanline()
{
    scanline++;

    if (scanline == VISIBLE_SCANLINES) {
        return true;
    }
    if (scanline >= SCANLINES_PER_FRAME) {
        scanline = 0;
        // An interlaced display shows alternate halves of the picture
        // on alternate frames; a progressive one has only the one
        // field and nothing to alternate.
        if (interlaced()) {
            odd_field = !odd_field;
        }
    }
    return false;
}

// The draw mode as it is allowed to be stored. Texture disable is the
// one bit software cannot simply have: it needs GP1(09h) first, and
// without it the bit never arrives rather than being ignored later.
u32 Gpu::gated_draw_mode(u32 value) const
{
    if (allow_texture_disable) {
        return value;
    }
    return value & ~DRAW_MODE_TEXTURE_DISABLE;
}

// A textured primitive carries its own texture page, and that is not a
// separate register from GP0(E1h) — it is the same one. Drawing leaves
// the page behind, where GPUSTAT and the next untextured primitive both
// find it. Only the page and the disable bit come across; the dither
// and draw-to-display bits beside them are the draw mode's own.
void Gpu::apply_texpage(u32 attribute)
{
    constexpr u32 FROM_PRIMITIVE =
        DRAW_MODE_TEXTURE_PAGE | DRAW_MODE_TEXTURE_DISABLE;
    draw_mode &= ~FROM_PRIMITIVE;
    draw_mode |= gated_draw_mode(attribute) & FROM_PRIMITIVE;
}

// How much of the FIFO is occupied. Nothing waits behind a GPU that
// has caught up, so a backlog only exists while it is still drawing.
u32 Gpu::queued_words(u64 now) const { return drawing(now) ? fifo_words : 0; }

// GPUSTAT bit 28. A block is sixteen words, so what it asks is not
// whether there is room for a word but whether there is room for all
// of them: the GPU has to have caught up.
//
// It answers for the way in, and nothing else. A transfer out of VRAM
// occupies the way out: the GPU is holding pixels for the CPU to
// collect and will still take a command or a block while it waits, so
// this stays set throughout one. Saying otherwise hangs anything that
// waits for the GPU after starting a read — PSn00bSDK ends every
// DrawPrim with a DrawSync, which with DMA off is a spin on this bit
// alone, and gpu/mask-bit never returns from its first readback.
bool Gpu::ready_for_block(u64 now) const
{
    return mode == Gp0Mode::ImageStore || !drawing(now);
}

// GPUSTAT bit 27, the way out: pixels are only there to be collected
// once the GPU has read them out of VRAM.
bool Gpu::ready_to_send(u64 now) const
{
    return mode == Gp0Mode::ImageStore && !drawing(now);
}

// Bit 25, which is also the wire the DMA controller watches. Which of
// the ready bits it repeats depends on which way GP1(04h) pointed the
// channel.
bool Gpu::dma_ready(u64 now) const
{
    switch (dma_direction) {
    case DmaDirection::Off:
        return false;
    case DmaDirection::Fifo:
        return queued_words(now) < FIFO_WORDS;
    case DmaDirection::CpuToGp0:
        return ready_for_block(now);
    case DmaDirection::VramToCpu:
        return ready_to_send(now);
    }
    return false;
}

u32 Gpu::status(u64 now) const
{
    u32 value = 0;

    // Bits 0..10 are the low bits of the GP0(E1h) draw mode word,
    // unchanged. Its texture-disable bit is the one that moves.
    value |= draw_mode & 0x7FF;
    value |= (mask_setting & 0x3) << 11;

    // Bit 13 is the field being shown. A progressive display has none,
    // and reads as one.
    const bool interlace = interlaced();
    const bool field = interlace ? odd_field : true;
    value |= static_cast<u32>(field) << 13;

    value |= ((display_mode >> 7) & 1) << 14;  // reverse flag
    value |= ((draw_mode >> 11) & 1) << 15;    // texture disable
    value |= ((display_mode >> 6) & 1) << 16;  // horizontal res 2
    value |= ((display_mode >> 0) & 3) << 17;  // horizontal res 1
    value |= ((display_mode >> 2) & 1) << 19;  // vertical resolution
    value |= ((display_mode >> 3) & 1) << 20;  // NTSC or PAL
    value |= ((display_mode >> 4) & 1) << 21;  // display colour depth
    value |= ((display_mode >> 5) & 1) << 22;  // vertical interlace
    value |= static_cast<u32>(display_disabled) << 23;
    value |= static_cast<u32>(irq) << 24;

    // The three ready bits are what software polls before handing over
    // work, and the reason this has to be derived rather than fixed:
    // during a transfer into VRAM the GPU genuinely is not ready for a
    // command, and says so.
    //
    // What they mostly answer for is how far behind the GPU has fallen
    // — see the three functions above. The command bit is the one the
    // FIFO makes worth polling: software can hand over a whole command
    // and only then be made to wait, rather than being stopped between
    // one word and the next.
    const bool ready_for_command =
        mode != Gp0Mode::ImageLoad && queued_words(now) < FIFO_WORDS;
    value |= static_cast<u32>(ready_for_command) << 26;
    value |= static_cast<u32>(ready_to_send(now)) << 27;
    value |= static_cast<u32>(ready_for_block(now)) << 28;

    value |= static_cast<u32>(dma_ready(now)) << 25;
    value |= static_cast<u32>(dma_direction) << 29;

    // Bit 31 is which lines are being drawn at this instant: none
    // during vertical blank, then the current field when interlaced,
    // and otherwise simply whether the current scanline is odd. It is
    // the finest-grained thing software can see of the video signal,
    // and it is what a display-synchronised loop watches.
    bool drawing_odd = false;
    if (!in_vblank()) {
        drawing_odd = interlace ? odd_field : ((scanline & 1) != 0);
    }
    value |= static_cast<u32>(drawing_odd) << 31;

    return value;
}

u32 Gpu::read(u64 now)
{
    if (mode != Gp0Mode::ImageStore) {
        return gpuread_latch;
    }

    // Two pixels to a word, the lower-addressed one in the low half.
    catch_up(now);
    const u32 low = load_pixel();
    const u32 high = load_pixel();
    charge(2, COST_VRAM_STORE);
    if (transfer.done()) {
        mode = Gp0Mode::Command;
    }
    return low | (high << 16);
}

// Brings the clock the GPU is charged against up to the present. Work
// only ever piles up on top of work still outstanding, so this is what
// keeps an idle GPU from being charged from where it left off — and it
// is where the FIFO empties, since a GPU that has caught up has
// nothing waiting behind it.
void Gpu::catch_up(u64 now)
{
    if (now < busy_until) {
        return;
    }
    busy_until = now;
    fifo_words = 0;
}

// A pixel costs a fraction of a cycle, and a transfer charges for two
// of them at a time — so the part of a cycle left over is kept rather
// than rounded away, and kept across an idle GPU too. Rounding it down
// costs a VRAM readback a fifth of its time; throwing it away whenever
// the GPU has caught up costs the readback all of it, because two
// pixels never come to a whole cycle in the first place.
void Gpu::charge(u32 pixels, u32 cost_per_pixel)
{
    const u64 owed = cost_owed + u64{pixels} * cost_per_pixel;
    busy_until += owed / PIXEL_COST_SCALE;
    cost_owed = static_cast<u32>(owed % PIXEL_COST_SCALE);
}

void Gpu::write_gp0(u32 word, u64 now)
{
    catch_up(now);

    // A word arriving at a GPU that has caught up is taken straight in
    // hand; one arriving at a GPU still drawing has to wait behind it,
    // and that waiting is the FIFO. Software that hands over a word
    // with it already full has ignored what GPUSTAT told it, and the
    // console has nowhere to put that word either — so it is counted
    // no further rather than dropped, which would desynchronise every
    // command after it.
    if (drawing(now)) {
        fifo_words = std::min(fifo_words + 1, FIFO_WORDS);
    }

    switch (mode) {
    case Gp0Mode::ImageLoad:
        store_pixel(static_cast<u16>(word));
        store_pixel(static_cast<u16>(word >> 16));
        charge(2, COST_VRAM_LOAD);
        if (transfer.done()) {
            mode = Gp0Mode::Command;
        }
        return;

    case Gp0Mode::PolyLine:
        if ((word & POLYLINE_END_MASK) == POLYLINE_END) {
            mode = Gp0Mode::Command;
            return;
        }
        pixels_drawn = 0;
        extend_polyline(word);
        charge(pixels_drawn, pixel_cost(shading_of(command[0] >> 24), false));
        return;

    case Gp0Mode::ImageStore:
    case Gp0Mode::Command:
        break;
    }

    command[command_words] = word;
    command_words++;
    if (command_words < gp0_length(command[0])) {
        return;  // still collecting
    }

    execute_gp0();
    command_words = 0;
}

void Gpu::execute_gp0()
{
    const u32 op = command[0] >> 24;
    pixels_drawn = 0;

    switch (op) {
    case 0x00:  // NOP
    case 0x01:
        // Clear cache. The palette the GPU is holding goes with it, so
        // the next textured primitive reads its own out of VRAM again.
        // This is the only thing that lets go of it: a fill or a line
        // drawn over the palette leaves the held copy alone.
        clut_entries = 0;
        return;
    case 0x1F:  // raise the GPU's interrupt
        irq = true;
        return;
    case 0xE1:
        draw_mode = gated_draw_mode(command[0]);
        return;
    case 0xE2:
        texture_window = command[0];
        return;
    case 0xE3:
        draw_area_top = command[0];
        return;
    case 0xE4:
        draw_area_bottom = command[0];
        return;
    case 0xE5:
        draw_offset = command[0];
        return;
    case 0xE6:
        mask_setting = command[0] & 0x3;
        return;
    default:
        break;
    }

    if (op == 0x02) {
        const u32 width = ((command[2] & 0x3FF) + 0xF) & ~0xFu;
        const u32 height = (command[2] >> 16) & 0x1FF;
        fill_vram(*this,
                  command[1] & 0x3F0,
                  (command[1] >> 16) & 0x1FF,
                  width,
                  height,
                  command[0] & 0xFFFFFF);
        charge(width * height, COST_FILL);
    } else if (op >= 0x20 && op <= 0x3F) {
        draw_polygon();
        charge(pixels_drawn, pixel_cost(shading_of(op), true));
    } else if (op >= 0x60 && op <= 0x7F) {
        draw_sprite();
        charge(pixels_drawn, pixel_cost(shading_of(op), false));
    } else if (op >= 0x80 && op <= 0x9F) {
        const u32 width = transfer_extent(command[3] & 0xFFFF, VRAM_WIDTH);
        const u32 height =
            transfer_extent((command[3] >> 16) & 0xFFFF, VRAM_HEIGHT);
        copy_vram(*this,
                  command[1] & 0x3FF,
                  (command[1] >> 16) & 0x1FF,
                  command[2] & 0x3FF,
                  (command[2] >> 16) & 0x1FF,
                  width,
                  height);
        charge(width * height, COST_VRAM_COPY);
    } else if (op >= 0xA0 && op <= 0xBF) {
        begin_transfer();
        mode = Gp0Mode::ImageLoad;
    } else if (op >= 0xC0 && op <= 0xDF) {
        begin_transfer();
        mode = Gp0Mode::ImageStore;
    } else if (op >= 0x40 && op <= 0x5F) {
        draw_line_command();
        charge(pixels_drawn, pixel_cost(shading_of(op), false));
        // Bit 3 makes it a polyline: the two vertices collected are the
        // first segment, and the rest follow until the terminator.
        if ((op & 0x08) != 0) {
            mode = Gp0Mode::PolyLine;
        }
    }
}

// A vertex word is a signed pair packed into a word, eleven bits each
// with the rest ignored, and everything is drawn relative to the offset
// GP0(E5h) set.
Vertex Gpu::vertex_at(u32 word) const
{
    Vertex corner;
    corner.x = sign_extend_11(word) + offset_x();
    corner.y = sign_extend_11(word >> 16) + offset_y();
    return corner;
}

void Gpu::draw_polygon()
{
    const u32 op = command[0] >> 24;
    Shading how = shading_of(op);

    const u32 corners = (op & 0x08) != 0 ? 4 : 3;

    // The words are walked with a cursor rather than a stride, because
    // the corners are not all the same length: under Gouraud shading
    // the first one takes its colour from the command word while every
    // other one carries its own.
    std::array<Vertex, 4> shape;
    u32 word = 1;
    for (u32 i = 0; i < corners; i++) {
        u32 colour = command[0] & 0xFFFFFF;
        if (how.gouraud && i > 0) {
            colour = command[word] & 0xFFFFFF;
            word++;
        }

        shape[i] = vertex_at(command[word]);
        shape[i].colour = colour;
        word++;

        if (how.textured) {
            const u32 texture = command[word];
            shape[i].u = texture & 0xFF;
            shape[i].v = (texture >> 8) & 0xFF;
            // The palette rides along with the first corner and the
            // texture page with the second; the rest carry nothing but
            // their own coordinates.
            if (i == 0) {
                how.clut = texture >> 16;
            }
            if (i == 1) {
                apply_texpage(texture >> 16);
                how.texpage = draw_mode & DRAW_MODE_TEXTURE_PAGE;
            }
            word++;
        }
    }

    draw_triangle(*this, {shape[0], shape[1], shape[2]}, how);
    if (corners == 4) {
        // A quad is two triangles sharing the diagonal between the
        // second and third corners, which is the order the GPU takes
        // them in — hence the fan rather than a loop.
        draw_triangle(*this, {shape[1], shape[2], shape[3]}, how);
    }
}

// A line command carries both its ends. Only the shaded form gives the
// second end a colour of its own; the flat form draws the whole line in
// the colour that came with the command byte.
void Gpu::draw_line_command()
{
    const u32 op = command[0] >> 24;
    Shading how = shading_of(op);
    how.textured = false;  // no line command has a texture

    Vertex from = vertex_at(command[1]);
    from.colour = command[0] & 0xFFFFFF;

    Vertex to;
    if (how.gouraud) {
        to = vertex_at(command[3]);
        to.colour = command[2] & 0xFFFFFF;
    } else {
        to = vertex_at(command[2]);
        to.colour = from.colour;
    }

    draw_line(*this, from, to, how);

    polyline_x = to.x;
    polyline_y = to.y;
    polyline_colour = to.colour;
    polyline_has_colour = false;
}

// One more vertex of a polyline, drawn as a segment from wherever the
// last one left off. A shaded polyline sends a colour ahead of each
// position, so a word is only a vertex once a colour is waiting.
void Gpu::extend_polyline(u32 word)
{
    const u32 op = command[0] >> 24;
    Shading how = shading_of(op);
    how.textured = false;

    if (how.gouraud && !polyline_has_colour) {
        polyline_next_colour = word & 0xFFFFFF;
        polyline_has_colour = true;
        return;
    }

    Vertex from;
    from.x = polyline_x;
    from.y = polyline_y;
    from.colour = polyline_colour;

    Vertex to = vertex_at(word);
    to.colour = how.gouraud ? polyline_next_colour : polyline_colour;
    polyline_has_colour = false;

    draw_line(*this, from, to, how);

    polyline_x = to.x;
    polyline_y = to.y;
    polyline_colour = to.colour;
}

void Gpu::draw_sprite()
{
    const u32 op = command[0] >> 24;
    Shading how = shading_of(op);
    how.texpage = draw_mode & 0x1FF;  // a sprite has no page of its own

    Vertex corner = vertex_at(command[1]);
    corner.colour = command[0] & 0xFFFFFF;

    u32 word = 2;
    if (how.textured) {
        corner.u = command[word] & 0xFF;
        corner.v = (command[word] >> 8) & 0xFF;
        how.clut = command[word] >> 16;
        word++;
    }

    // Bits 4..3 give a fixed 1x1, 8x8 or 16x16; zero means the size
    // arrives as a further word.
    u32 width = 1;
    u32 height = 1;
    switch ((op >> 3) & 3) {
    case 1:
        break;
    case 2:
        width = height = 8;
        break;
    case 3:
        width = height = 16;
        break;
    default:
        width = command[word] & 0x3FF;
        height = (command[word] >> 16) & 0x1FF;
        break;
    }

    draw_rectangle(*this, corner, width, height, how);
}

void Gpu::begin_transfer()
{
    // Word 1 is the corner in VRAM, word 2 the size, both packed as
    // y in the high half and x in the low.
    transfer.x = command[1] & 0x3FF;
    transfer.y = (command[1] >> 16) & 0x1FF;
    transfer.width = transfer_extent(command[2] & 0xFFFF, VRAM_WIDTH);
    transfer.height = transfer_extent((command[2] >> 16) & 0xFFFF, VRAM_HEIGHT);
    transfer.pixels_done = 0;
}

void Gpu::store_pixel(u16 pixel)
{
    if (transfer.done()) {
        // An odd pixel count leaves the last word half unused.
        return;
    }
    // A transfer that runs off an edge wraps around VRAM rather than
    // spilling into the next row, since VRAM has no rows to spill to.
    const u32 x =
        (transfer.x + transfer.pixels_done % transfer.width) % VRAM_WIDTH;
    const u32 y =
        (transfer.y + transfer.pixels_done / transfer.width) % VRAM_HEIGHT;
    const std::size_t at = std::size_t{y} * VRAM_WIDTH + x;

    // GP0(E6h) governs a transfer as much as it governs drawing: the
    // mask bit is not a property of the pixels being sent but of the
    // port they arrive through. A pixel already marked is protected
    // from one, and one written through it is marked in turn.
    transfer.pixels_done++;
    const bool protect = (mask_setting & MASK_CHECK_BEFORE_DRAW) != 0;
    if (protect && (vram[at] & MASK_BIT) != 0) {
        return;
    }
    if ((mask_setting & MASK_SET_WHEN_DRAWING) != 0) {
        vram[at] = pixel | MASK_BIT;
        return;
    }
    vram[at] = pixel;
}

u16 Gpu::load_pixel()
{
    if (transfer.done()) {
        return 0;
    }
    const u32 x =
        (transfer.x + transfer.pixels_done % transfer.width) % VRAM_WIDTH;
    const u32 y =
        (transfer.y + transfer.pixels_done / transfer.width) % VRAM_HEIGHT;
    transfer.pixels_done++;
    return vram[std::size_t{y} * VRAM_WIDTH + x];
}

void Gpu::write_gp1(u32 word)
{
    const u32 op = (word >> 24) & 0xFF;
    const u32 parameter = word & 0xFFFFFF;

    switch (op) {
    case 0x00:  // reset the GPU
        reset();
        break;
    case 0x01:  // abandon whatever GP0 was collecting
        mode = Gp0Mode::Command;
        command_words = 0;
        transfer = Transfer{};
        // What software resets the FIFO for is to get out of a stall,
        // so the words waiting in it go as well. The work already
        // handed to the drawing engine does not: the console cannot
        // un-draw it, and busy_until is when it will be finished.
        fifo_words = 0;
        break;
    case 0x02:
        irq = false;
        break;
    case 0x03:
        display_disabled = (parameter & 1) != 0;
        break;
    case 0x04:
        dma_direction = static_cast<DmaDirection>(parameter & 0x3);
        break;
    case 0x05:
        display_start = parameter;
        break;
    case 0x06:
        display_range_x = parameter;
        break;
    case 0x07:
        display_range_y = parameter;
        break;
    case 0x08:
        display_mode = parameter;
        break;
    case 0x09:
        allow_texture_disable = (parameter & 1) != 0;
        break;
    case 0x10:
        // Reads back a drawing register through GPUREAD. The register
        // is chosen by the low bits of the parameter.
        switch (parameter & 0x7) {
        case 0x2:
            gpuread_latch = texture_window & 0xFFFFF;
            break;
        case 0x3:
            gpuread_latch = draw_area_top & 0xFFFFF;
            break;
        case 0x4:
            gpuread_latch = draw_area_bottom & 0xFFFFF;
            break;
        case 0x5:
            gpuread_latch = draw_offset & 0x3FFFFF;
            break;
        case 0x7:
            gpuread_latch = 2;  // GPU version
            break;
        default:
            break;  // leaves the latch as it was, as hardware does
        }
        break;
    default:
        break;  // the remaining commands do nothing this emulator sees
    }
}
