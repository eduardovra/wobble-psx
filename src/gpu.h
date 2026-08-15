#pragma once

#include <array>
#include <cstddef>

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
// Drawing is not implemented. Commands that would rasterize are parsed
// and their words consumed, then discarded; everything that only moves
// data or sets state is real. That split is what lets GPUSTAT be
// derived from actual state rather than answering with a constant.
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

    u32 status() const;

    // The GPUREAD port. Returns transfer data while a VRAM-to-CPU copy
    // is in progress, and otherwise whatever GP1(10h) last asked for.
    u32 read();

    void write_gp0(u32 word);
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

    u32 display_start = 0;    // GP1(05h)
    u32 display_range_x = 0;  // GP1(06h)
    u32 display_range_y = 0;  // GP1(07h)
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

private:
    void execute_gp0();

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
