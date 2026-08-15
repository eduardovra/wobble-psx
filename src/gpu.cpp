#include "gpu.h"

#include <cstddef>

namespace {

// The word that ends a polyline. Software terminates the vertex list
// with it rather than giving a count up front.
constexpr u32 POLYLINE_END = 0x50005000;
constexpr u32 POLYLINE_END_MASK = 0xF000F000;

// A transfer's width and height are encoded so that zero means the
// maximum: 0 pixels would be a pointless transfer, 1024 (or 512) does
// not fit the field, so the value wraps into it.
u32 transfer_extent(u32 value, u32 limit)
{
    return ((value - 1) & (limit - 1)) + 1;
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

void Gpu::reset()
{
    // GP1(00h)'s effect, and the power-on state: blanked, no transfer
    // in progress, DMA off, every drawing register cleared.
    mode = Gp0Mode::Command;
    transfer = Transfer{};
    command_words = 0;
    draw_mode = 0;
    texture_window = 0;
    draw_area_top = 0;
    draw_area_bottom = 0;
    draw_offset = 0;
    mask_setting = 0;
    display_mode = 0;
    display_start = 0;
    display_range_x = 0;
    display_range_y = 0;
    dma_direction = DmaDirection::Off;
    display_disabled = true;
    irq = false;
    odd_field = false;
    gpuread_latch = 0;
    // VRAM survives a reset on hardware, so it is not cleared here.
}

u32 Gpu::status() const
{
    u32 value = 0;

    // Bits 0..10 are the low bits of the GP0(E1h) draw mode word,
    // unchanged. Its texture-disable bit is the one that moves.
    value |= draw_mode & 0x7FF;
    value |= (mask_setting & 0x3) << 11;

    // Bits 13 and 31 both describe the interlace field. A progressive
    // display has no fields, so 13 reads as one and 31 as zero.
    const bool interlaced = ((display_mode >> 5) & 1) != 0;
    const bool field = interlaced ? odd_field : true;
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
    const bool ready_for_command = mode != Gp0Mode::ImageLoad;
    const bool ready_to_send = mode == Gp0Mode::ImageStore;
    value |= static_cast<u32>(ready_for_command) << 26;
    value |= static_cast<u32>(ready_to_send) << 27;
    value |= static_cast<u32>(ready_for_command) << 28;

    // Bit 25 answers the DMA controller's request line, and what it
    // means depends on which way the transfer is going.
    bool dma_request = false;
    switch (dma_direction) {
    case DmaDirection::Off:
        dma_request = false;
        break;
    case DmaDirection::Fifo:
        dma_request = true;  // the command FIFO is never full here
        break;
    case DmaDirection::CpuToGp0:
        dma_request = ready_for_command;
        break;
    case DmaDirection::VramToCpu:
        dma_request = ready_to_send;
        break;
    }
    value |= static_cast<u32>(dma_request) << 25;
    value |= static_cast<u32>(dma_direction) << 29;
    value |= static_cast<u32>(interlaced && odd_field) << 31;

    return value;
}

u32 Gpu::read()
{
    if (mode != Gp0Mode::ImageStore) {
        return gpuread_latch;
    }

    // Two pixels to a word, the lower-addressed one in the low half.
    const u32 low = load_pixel();
    const u32 high = load_pixel();
    if (transfer.done()) {
        mode = Gp0Mode::Command;
    }
    return low | (high << 16);
}

void Gpu::write_gp0(u32 word)
{
    switch (mode) {
    case Gp0Mode::ImageLoad:
        store_pixel(static_cast<u16>(word));
        store_pixel(static_cast<u16>(word >> 16));
        if (transfer.done()) {
            mode = Gp0Mode::Command;
        }
        return;

    case Gp0Mode::PolyLine:
        if ((word & POLYLINE_END_MASK) == POLYLINE_END) {
            mode = Gp0Mode::Command;
        }
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

    switch (op) {
    case 0x00:  // NOP
    case 0x01:  // clear texture cache
        return;
    case 0x1F:  // raise the GPU's interrupt
        irq = true;
        return;
    case 0xE1:
        draw_mode = command[0];
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

    if (op >= 0xA0 && op <= 0xBF) {
        begin_transfer();
        mode = Gp0Mode::ImageLoad;
    } else if (op >= 0xC0 && op <= 0xDF) {
        begin_transfer();
        mode = Gp0Mode::ImageStore;
    } else if (op >= 0x48 && op <= 0x5F) {
        // A line command with bit 3 set is a polyline: the vertices
        // already collected are the first two, and the rest follow
        // until the terminator.
        mode = Gp0Mode::PolyLine;
    }
    // Anything else draws, which this emulator does not do yet. Its
    // words have been counted off, so the stream stays in step.
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
    vram[std::size_t{y} * VRAM_WIDTH + x] = pixel;
    transfer.pixels_done++;
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
