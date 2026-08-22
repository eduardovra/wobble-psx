#pragma once

#include <array>
#include <cstddef>

#include "raster.h"
#include "types.h"

struct State;

// The GPU, as far as the CPU can see it: two ports, and the state
// behind them.
//
//   0x1F801810  write GP0   drawing commands and pixel data
//               read  GPUREAD  data coming back the other way
//   0x1F801814  write GP1   display control, taking effect at once
//               read  GPUSTAT what the GPU is currently doing
//
// The two write ports differ in more than what they do. GP1 commands
// are one word and act immediately, which is why a stuck GPU can still
// be reset. GP0 commands are a stream: the command byte says how many
// words follow, and the GPU counts them off. Nothing frames a command
// but that count, so a single wrong length desynchronises every
// command after it — which is why gp0_length() is the heart of this
// file and is tested command by command.
//
// Drawing happens straight into VRAM as each command arrives, with no
// display list and no deferral, because that is what the hardware does
// and there is nothing here a frame boundary would let us batch. Lines
// are the one family still parsed and dropped.
//
// What is deferred is when the command is finished. The console takes
// time over drawing — roughly the pixels it touches, more of it for a
// texture or a blend — and everything that waits for the GPU waits on
// that: the ready bits in GPUSTAT, and the request line the DMA
// controller paces a display list against. So the ports take the
// master clock, and busy_until is the answer they are all built on.
struct Gpu {
    static constexpr u32 GP0 = 0x1F801810;
    static constexpr u32 GP1 = 0x1F801814;

    // Video memory: 1 MB, addressed as 16-bit pixels in a 1024x512
    // grid. Framebuffers, textures and palettes all live here, at
    // whatever coordinates software decides — the GPU imposes no
    // structure on it.
    static constexpr u32 VRAM_WIDTH = 1024;
    static constexpr u32 VRAM_HEIGHT = 512;
    static constexpr std::size_t VRAM_PIXELS =
        std::size_t{VRAM_WIDTH} * VRAM_HEIGHT;

    // NTSC video timing. The GPU's clock is 11/7 of the CPU's —
    // 53.2224 MHz against 33.8688 — and a scanline is 3413 of its
    // cycles, which is 2171.9 of the CPU's. Rounding that to a whole
    // cycle runs the display 0.004% fast, far below anything software
    // can notice, and keeps the scanline period an exact integer so
    // the scheduler has nothing to accumulate error in.
    //
    // 263 lines at that rate is 59.29 frames a second. That really is
    // the console's refresh rate: it is derived from the same crystal
    // as everything else rather than from the broadcast standard, and
    // comes out a little under NTSC's nominal 59.94.
    static constexpr u64 CYCLES_PER_SCANLINE = 2172;
    static constexpr u32 GPU_CYCLES_PER_SCANLINE = 3413;
    static constexpr u32 SCANLINES_PER_FRAME = 263;
    static constexpr u32 VISIBLE_SCANLINES = 240;

    void reset();

    void visit_state(State& state);

    // Advances the video signal by one scanline, and reports whether
    // vertical blanking has just begun — the moment everything else in
    // the machine is paced by.
    bool next_scanline();

    // Vertical blank is nothing more than the signal having passed the
    // last visible line.
    bool in_vblank() const { return scanline >= VISIBLE_SCANLINES; }

    bool interlaced() const { return ((display_mode >> 5) & 1) != 0; }

    // Whether the signal is the 50 Hz one. It carries more scanlines,
    // and the picture sits at a different place among them.
    bool pal() const { return ((display_mode >> 3) & 1) != 0; }

    // Whether the video signal reads VRAM three bytes to the pixel
    // instead of one halfword. Nothing can be drawn in that form — the
    // GPU only carries it — so it is a mode for a still or a movie,
    // put into VRAM by whatever decoded it.
    bool colour_24bit() const { return ((display_mode >> 4) & 1) != 0; }

    // GPUSTAT, and the two ports either side of it. All three take
    // the master clock because the ready bits are no longer a property
    // of the GPU's state alone: they answer for how much of what it
    // was given it has got through, which only a timestamp can say.
    u32 status(u64 now) const;

    // Whether the GPU is still working through what it has been
    // handed. Drawing is not deferred — a command reaches VRAM the
    // moment its last word arrives — but the console takes time over
    // it, and this is that time.
    bool drawing(u64 now) const { return now < busy_until; }

    // Whether the request line the DMA controller watches is up. It is
    // GPUSTAT bit 25, which is whichever ready bit matches the
    // direction GP1(04h) set, and the controller asks for it directly
    // rather than picking it back out of the status word.
    bool dma_ready(u64 now) const;

    // The two ready bits the DMA controller's handshake is made of:
    // GPUSTAT bit 28, room on the way in, and bit 27, something
    // waiting on the way out.
    bool ready_for_block(u64 now) const;
    bool ready_to_send(u64 now) const;

    // GP0(E1h) as it may be stored, and the part of it a textured
    // primitive brings with it.
    u32 gated_draw_mode(u32 value) const;
    void apply_texpage(u32 attribute);

    // The two halves of drawing a line family command: the command
    // itself, and each further vertex of a polyline.
    void draw_line_command();
    void extend_polyline(u32 word);

    // The clipping rectangle GP0(E3h) and GP0(E4h) set, and the shift
    // GP0(E5h) applies to every vertex. All three are kept as the words
    // software wrote, so this is where they are taken apart. The two
    // corners are inclusive: a one-pixel draw area has them equal.
    u32 clip_left() const { return draw_area_top & 0x3FF; }
    u32 clip_top() const { return (draw_area_top >> 10) & 0x1FF; }
    u32 clip_right() const { return draw_area_bottom & 0x3FF; }
    u32 clip_bottom() const { return (draw_area_bottom >> 10) & 0x1FF; }
    s32 offset_x() const;
    s32 offset_y() const;

    // The picture the video signal is carrying: the window into VRAM
    // that GP1(05h) picks and GP1(08h) sizes. Nothing inside the
    // machine depends on these — they exist so something outside it can
    // put the picture on a screen.
    u32 display_width() const;
    u32 display_height() const;

    // The scanline a full-height picture starts on, which is what the
    // range GP1(07h) carries is measured against.
    u32 first_scanline() const;

    // How many of the GPU's own cycles one pixel of the display lasts.
    // A wider mode clocks pixels out faster, so this falls as the
    // width rises. Timer 0 can be told to count in these rather than
    // in CPU cycles, which is the only reason it is public.
    u32 dot_cycles() const;

    // How long horizontal blanking lasts, in CPU cycles: whatever the
    // scanline has left once the window GP1(06h) picks has taken its
    // share. A game that narrows the picture lengthens the blank, and
    // timer 0 can be told to stop, or to start over, while it lasts.
    u64 hblank_cycles() const;

    struct Colour {
        u8 r = 0;
        u8 g = 0;
        u8 b = 0;
    };

    // One pixel of that picture. Out-of-range coordinates read black
    // rather than wrapping, since off the edge of the picture is not a
    // place VRAM has an answer for.
    Colour display_pixel(u32 x, u32 y) const;

    // The GPUREAD port. Returns transfer data while a VRAM-to-CPU copy
    // is in progress, and otherwise whatever GP1(10h) last asked for.
    u32 read(u64 now);

    void write_gp0(u32 word, u64 now);
    void write_gp1(u32 word);

    // What GP0 does with the next word it is given.
    enum class Gp0Mode : u32 {
        Command,     // collecting the words of a command
        ImageLoad,   // pixel data on its way into VRAM
        ImageStore,  // a VRAM-to-CPU copy, read back through GPUREAD
        PolyLine,    // vertices, until the terminator word
    };

    // Where a DMA transfer to or from the GPU is headed. It also
    // selects what GPUSTAT reports in its DMA request bit.
    enum class DmaDirection : u32 {
        Off = 0,
        Fifo = 1,
        CpuToGp0 = 2,
        VramToCpu = 3,
    };

    // A rectangular copy in or out of VRAM, tracked by how many pixels
    // of it have gone past.
    struct Transfer {
        u32 x = 0;
        u32 y = 0;
        u32 width = 0;
        u32 height = 0;
        u32 pixels_done = 0;

        bool done() const { return pixels_done >= width * height; }
    };

    std::array<u16, VRAM_PIXELS> vram{};

    // The palette the GPU is holding. A textured primitive's palette is
    // read out of VRAM once, before any of the primitive is drawn, and
    // stays until one asks for a different palette — or for more of one
    // than was read, which is what a 4-bit primitive followed by an
    // 8-bit one does. Overwriting the palette in VRAM in between
    // changes nothing until then. gpu/clut-cache is built around that,
    // and it is also what lets a sprite be drawn straight over its own
    // palette without eating itself halfway across.
    static constexpr u32 CLUT_ENTRIES = 256;
    std::array<u16, CLUT_ENTRIES> clut{};
    u32 clut_source = 0;   // the CLUT word the held entries came from
    u32 clut_entries = 0;  // how many of them were read

    Gp0Mode mode = Gp0Mode::Command;
    Transfer transfer;

    // GP0(E1h..E6h) and GP1(08h) are kept as the words software wrote,
    // because that is what GPUSTAT reports back: most of its bits are
    // these two registers' bits, rearranged. Storing them whole means
    // the parts this emulator does not use yet are still reported
    // correctly rather than reading as zero.
    u32 draw_mode = 0;         // GP0(E1h)
    u32 texture_window = 0;    // GP0(E2h)
    u32 draw_area_top = 0;     // GP0(E3h)
    u32 draw_area_bottom = 0;  // GP0(E4h)
    u32 draw_offset = 0;       // GP0(E5h)
    u32 mask_setting = 0;      // GP0(E6h), two bits
    u32 display_mode = 0;      // GP1(08h)

    // Where a polyline has got to: the segment just drawn ended here,
    // and the next one starts from it. A shaded polyline sends a colour
    // ahead of each position, so one may be waiting for its vertex.
    s32 polyline_x = 0;
    s32 polyline_y = 0;
    u32 polyline_colour = 0;
    u32 polyline_next_colour = 0;
    bool polyline_has_colour = false;

    // GP1(09h). Whether the draw mode's texture-disable bit is allowed
    // to mean anything: with this off, the bit is dropped as it is
    // written rather than ignored as it is used, so turning this off
    // later leaves a bit already taken in place.
    bool allow_texture_disable = false;

    // The ranges come up holding a full-height NTSC picture, which is
    // where a reset puts them too — see reset().
    u32 display_start = 0;             // GP1(05h)
    u32 display_range_x = 0x00C00200;  // GP1(06h)
    u32 display_range_y = 0x00040010;  // GP1(07h)
    DmaDirection dma_direction = DmaDirection::Off;
    bool display_disabled = true;  // the GPU powers up blanked
    bool irq = false;              // GP0(1Fh) sets it, GP1(02h) clears

    // Where the video signal has got to, and which field it is on when
    // the display is interlaced. Software reads both back out of
    // GPUSTAT to pace itself against the display, so they have to come
    // from the clock rather than sitting still.
    u32 scanline = 0;
    bool odd_field = false;

    // What GPUREAD answers with when no transfer is running: GP1(10h)
    // loads it with one of the drawing registers.
    u32 gpuread_latch = 0;

    // Words of the command being collected. The longest GP0 command,
    // a gouraud-shaded textured quad, is twelve words.
    static constexpr u32 COMMAND_MAX_WORDS = 16;
    std::array<u32, COMMAND_MAX_WORDS> command{};
    u32 command_words = 0;

    // When the GPU will have finished what it has been given, and how
    // many words are queued behind that. Together they are the command
    // FIFO: sixteen words deep, as on the chip.
    //
    // The queue is only ever occupied while there is something to wait
    // for. Once the clock passes busy_until the GPU has caught up with
    // everything handed to it, so by definition nothing is left
    // waiting — which is why the count is emptied on the first word to
    // arrive after that moment rather than drained word by word.
    static constexpr u32 FIFO_WORDS = 16;
    u64 busy_until = 0;
    u32 fifo_words = 0;

    // The part of a cycle the charges so far have not added up to. A
    // pixel costs well under one, so a charge for two of them — which
    // is what a VRAM readback pays, a word at a time — rounds down to
    // nothing at all unless the remainder is kept somewhere.
    u32 cost_owed = 0;

    // Pixels the rasterizer walked over drawing the command it is on.
    // It counts what was covered, not what came out: a pixel dropped
    // by the mask bit or by a transparent texel was still visited, and
    // still cost the console its time. Pixels outside the draw area
    // are not counted, because the console does not pay for those
    // either — gpu/bandwidth times a quad half off the screen at half
    // the whole one.
    u32 pixels_drawn = 0;

private:
    void execute_gp0();

    // Brings the clock the GPU is charged against up to the present,
    // and puts the cost of a command onto it.
    void catch_up(u64 now);
    void charge(u32 pixels, u32 cost_per_pixel);

    // How much of the FIFO is occupied, which is what GPUSTAT's
    // command bit answers for.
    u32 queued_words(u64 now) const;

    // Turning a collected command into the geometry the rasterizer
    // takes. All the fiddly part of a GP0 drawing command is here: how
    // many corners it has, which words carry colours and which carry
    // texture coordinates, and where the palette and page hide.
    void draw_polygon();
    void draw_sprite();
    Vertex vertex_at(u32 word) const;

    // Reads the destination and size words shared by both directions
    // of a VRAM copy.
    void begin_transfer();

    void store_pixel(u16 pixel);
    u16 load_pixel();
};

// How many words a GP0 command occupies, its command word included.
// Exposed for the tests: a wrong length here is invisible until some
// later command is misread, so it is checked directly.
u32 gp0_length(u32 command);
