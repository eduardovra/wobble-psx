#include <cstddef>
#include <memory>

#include <doctest/doctest.h>

#include "bus.h"
#include "gpu.h"
#include "machine.h"

namespace {

// Builds a GP0 command word from its command byte, the rest being
// parameters the length does not depend on.
constexpr u32 gp0(u32 op) { return op << 24; }

}  // namespace

// A wrong length is invisible until some later command is misread, so
// each family is checked directly rather than through its effects.
TEST_CASE("GP0 command lengths follow the command byte")
{
    SUBCASE("the bare commands are one word")
    {
        CHECK(gp0_length(gp0(0x00)) == 1);  // NOP
        CHECK(gp0_length(gp0(0x01)) == 1);  // clear texture cache
        CHECK(gp0_length(gp0(0x1F)) == 1);  // interrupt request
        CHECK(gp0_length(gp0(0xE1)) == 1);  // draw mode
        CHECK(gp0_length(gp0(0xE6)) == 1);  // mask settings
    }

    SUBCASE("a rectangle fill carries a corner and a size")
    {
        CHECK(gp0_length(gp0(0x02)) == 3);
    }

    SUBCASE("polygons grow with vertices, texture and shading")
    {
        CHECK(gp0_length(gp0(0x20)) == 4);   // flat triangle
        CHECK(gp0_length(gp0(0x28)) == 5);   // flat quad
        CHECK(gp0_length(gp0(0x24)) == 7);   // textured triangle
        CHECK(gp0_length(gp0(0x2C)) == 9);   // textured quad
        CHECK(gp0_length(gp0(0x30)) == 6);   // gouraud triangle
        CHECK(gp0_length(gp0(0x38)) == 8);   // gouraud quad
        CHECK(gp0_length(gp0(0x3C)) == 12);  // gouraud textured quad
    }

    SUBCASE("the longest command still fits the buffer")
    {
        CHECK(gp0_length(gp0(0x3C)) <= Gpu::COMMAND_MAX_WORDS);
    }

    SUBCASE("lines are two vertices, plus a colour when shaded")
    {
        CHECK(gp0_length(gp0(0x40)) == 3);
        CHECK(gp0_length(gp0(0x50)) == 4);
    }

    SUBCASE("rectangles carry a size only when it is not fixed")
    {
        CHECK(gp0_length(gp0(0x60)) == 3);  // variable size
        CHECK(gp0_length(gp0(0x68)) == 2);  // 1x1
        CHECK(gp0_length(gp0(0x70)) == 2);  // 8x8
        CHECK(gp0_length(gp0(0x78)) == 2);  // 16x16
        CHECK(gp0_length(gp0(0x64)) == 4);  // textured, variable size
        CHECK(gp0_length(gp0(0x74)) == 3);  // textured 8x8
    }

    SUBCASE("the transfer commands carry their rectangle")
    {
        CHECK(gp0_length(gp0(0x80)) == 4);  // VRAM to VRAM
        CHECK(gp0_length(gp0(0xA0)) == 3);  // CPU to VRAM
        CHECK(gp0_length(gp0(0xC0)) == 3);  // VRAM to CPU
    }
}

TEST_CASE("a command takes effect only once all its words have arrived")
{
    Gpu gpu;

    // A rectangle fill is three words; the first two are just
    // collected.
    gpu.write_gp0(gp0(0x02));
    gpu.write_gp0(0x00000000);
    CHECK(gpu.command_words == 2);

    gpu.write_gp0(0x00100010);
    CHECK(gpu.command_words == 0);  // executed and the buffer released
}

TEST_CASE("the drawing registers are what GPUSTAT reports back")
{
    Gpu gpu;

    // GP0(E1h) bits 0..10 appear unchanged in GPUSTAT, and its
    // texture-disable bit moves from 11 to 15.
    gpu.write_gp0(gp0(0xE1) | 0x7FF);
    CHECK((gpu.status() & 0x7FF) == 0x7FF);
    CHECK((gpu.status() & (1u << 15)) == 0);

    // That bit needs GP1(09h)'s permission first, and is dropped as it
    // is written without it rather than ignored afterwards.
    gpu.write_gp0(gp0(0xE1) | (1u << 11));
    CHECK((gpu.status() & (1u << 15)) == 0);

    gpu.write_gp1((0x09u << 24) | 1);
    gpu.write_gp0(gp0(0xE1) | (1u << 11));
    CHECK((gpu.status() & (1u << 15)) != 0);

    // Permission withdrawn does not take back a bit already stored.
    gpu.write_gp1(0x09u << 24);
    CHECK((gpu.status() & (1u << 15)) != 0);

    // GP0(E6h)'s two mask bits land at 11 and 12.
    gpu.write_gp0(gp0(0xE6) | 0x3);
    CHECK((gpu.status() & (0x3u << 11)) == (0x3u << 11));
}

TEST_CASE("the display mode word is unpacked into GPUSTAT")
{
    Gpu gpu;

    gpu.write_gp1((0x08u << 24) | (1u << 3));  // PAL
    CHECK((gpu.status() & (1u << 20)) != 0);

    gpu.write_gp1((0x08u << 24) | (1u << 5));  // vertical interlace
    CHECK((gpu.status() & (1u << 22)) != 0);
    CHECK((gpu.status() & (1u << 20)) == 0);  // and no longer PAL
}

namespace {

// Runs the video signal on by whole scanlines, reporting how many of
// them began a vertical blank.
u32 advance_scanlines(Gpu& gpu, u32 count)
{
    u32 vblanks = 0;
    for (u32 i = 0; i < count; i++) {
        if (gpu.next_scanline()) {
            vblanks++;
        }
    }
    return vblanks;
}

}  // namespace

TEST_CASE("vertical blank begins once the last visible line is done")
{
    Gpu gpu;

    CHECK_FALSE(gpu.in_vblank());
    CHECK(advance_scanlines(gpu, Gpu::VISIBLE_SCANLINES) == 1);
    CHECK(gpu.in_vblank());

    // It lasts the rest of the frame, and the next one comes a whole
    // frame later rather than at the end of the blanking interval.
    CHECK(advance_scanlines(gpu, Gpu::SCANLINES_PER_FRAME - 1) == 0);
    CHECK_FALSE(gpu.in_vblank());
    CHECK(advance_scanlines(gpu, Gpu::SCANLINES_PER_FRAME) == 1);
}

TEST_CASE("a frame's worth of scanlines is one frame of emulated time")
{
    // The whole machine is paced off this, so the arithmetic is worth
    // pinning: 263 lines of 2172 cycles is 59.3 frames a second.
    constexpr u64 CYCLES_PER_FRAME =
        Gpu::CYCLES_PER_SCANLINE * Gpu::SCANLINES_PER_FRAME;
    constexpr u64 CPU_CLOCK = 33'868'800;

    CHECK(CYCLES_PER_FRAME == 571'236);
    CHECK(CPU_CLOCK / CYCLES_PER_FRAME == 59);
}

TEST_CASE("GPUSTAT's drawing bit follows the video signal")
{
    Gpu gpu;
    constexpr u32 FIELD = 1u << 13;
    constexpr u32 DRAWING_ODD = 1u << 31;

    SUBCASE("a progressive display alternates it every scanline")
    {
        gpu.write_gp1(0x08000000);  // interlace off

        // With no fields to speak of, bit 13 sits at one.
        CHECK((gpu.status() & FIELD) != 0);

        CHECK((gpu.status() & DRAWING_ODD) == 0);
        gpu.next_scanline();
        CHECK((gpu.status() & DRAWING_ODD) != 0);
        gpu.next_scanline();
        CHECK((gpu.status() & DRAWING_ODD) == 0);
    }

    SUBCASE("an interlaced one alternates it every frame instead")
    {
        gpu.write_gp1((0x08u << 24) | (1u << 5));

        CHECK((gpu.status() & FIELD) == 0);
        CHECK((gpu.status() & DRAWING_ODD) == 0);

        // A single scanline changes nothing: the field is what moves.
        gpu.next_scanline();
        CHECK((gpu.status() & DRAWING_ODD) == 0);

        advance_scanlines(gpu, Gpu::SCANLINES_PER_FRAME);
        CHECK((gpu.status() & FIELD) != 0);
        CHECK((gpu.status() & DRAWING_ODD) != 0);

        advance_scanlines(gpu, Gpu::SCANLINES_PER_FRAME);
        CHECK((gpu.status() & FIELD) == 0);
    }

    SUBCASE("nothing is being drawn during vertical blank")
    {
        gpu.write_gp1(0x08000000);
        advance_scanlines(gpu, Gpu::VISIBLE_SCANLINES + 1);

        REQUIRE(gpu.in_vblank());
        // On a line whose number is odd, which anywhere else in the
        // frame would show up in the bit.
        REQUIRE((gpu.scanline & 1) != 0);
        CHECK((gpu.status() & DRAWING_ODD) == 0);
    }
}

TEST_CASE("loading an image stops commands but is what wants a block")
{
    Gpu gpu;
    constexpr u32 READY_FOR_DMA = 1u << 28;
    constexpr u32 READY_FOR_COMMAND = 1u << 26;
    constexpr u32 READY_TO_SEND = 1u << 27;

    CHECK((gpu.status() & READY_FOR_DMA) != 0);
    CHECK((gpu.status() & READY_FOR_COMMAND) != 0);
    CHECK((gpu.status() & READY_TO_SEND) == 0);

    // A 2x1 transfer into VRAM: one word of pixels to come.
    gpu.write_gp0(gp0(0xA0));
    gpu.write_gp0(0);
    gpu.write_gp0((1u << 16) | 2);
    CHECK((gpu.status() & READY_FOR_COMMAND) == 0);
    // Still asking for the pixels, which is the whole reason it is not
    // ready for anything else. Software uploading a picture waits on
    // this bit between blocks and would wait forever if it cleared.
    CHECK((gpu.status() & READY_FOR_DMA) != 0);

    gpu.write_gp0(0x11112222);
    CHECK((gpu.status() & READY_FOR_DMA) != 0);
    CHECK((gpu.status() & READY_FOR_COMMAND) != 0);
}

TEST_CASE("an image is written into VRAM two pixels to a word")
{
    Gpu gpu;

    gpu.write_gp0(gp0(0xA0));
    gpu.write_gp0((3u << 16) | 5);  // to x=5, y=3
    gpu.write_gp0((2u << 16) | 2);  // a 2x2 rectangle
    gpu.write_gp0(0xBBBBAAAA);      // the low half is the first pixel
    gpu.write_gp0(0xDDDDCCCC);

    CHECK(gpu.vram[std::size_t{3} * Gpu::VRAM_WIDTH + 5] == 0xAAAA);
    CHECK(gpu.vram[std::size_t{3} * Gpu::VRAM_WIDTH + 6] == 0xBBBB);
    CHECK(gpu.vram[std::size_t{4} * Gpu::VRAM_WIDTH + 5] == 0xCCCC);
    CHECK(gpu.vram[std::size_t{4} * Gpu::VRAM_WIDTH + 6] == 0xDDDD);

    // The rectangle ended where it said it would.
    CHECK(gpu.vram[std::size_t{3} * Gpu::VRAM_WIDTH + 7] == 0);
}

TEST_CASE("an odd-sized image ignores the spare half of its last word")
{
    Gpu gpu;

    gpu.write_gp0(gp0(0xA0));
    gpu.write_gp0(0);
    gpu.write_gp0((1u << 16) | 1);  // a single pixel
    gpu.write_gp0(0xFFFF1234);

    CHECK(gpu.vram[0] == 0x1234);
    CHECK(gpu.vram[1] == 0);
    // And the GPU is back to taking commands rather than expecting
    // another word of pixels.
    CHECK(gpu.command_words == 0);
    gpu.write_gp0(gp0(0xE1) | 0x5);
    CHECK((gpu.status() & 0x7FF) == 0x5);
}

TEST_CASE("a VRAM to CPU transfer reads back through GPUREAD")
{
    Gpu gpu;
    gpu.vram[0] = 0x1111;
    gpu.vram[1] = 0x2222;

    gpu.write_gp0(gp0(0xC0));
    gpu.write_gp0(0);
    gpu.write_gp0((1u << 16) | 2);

    CHECK((gpu.status() & (1u << 27)) != 0);  // ready to send
    // Waiting to be read from occupies the way out and leaves the way
    // in alone: a command or a block would still be taken. Software
    // that starts a read and then waits for the GPU — which is what
    // every PSn00bSDK primitive ends with — waits on exactly this bit.
    CHECK((gpu.status() & (1u << 28)) != 0);  // and still ready for a block
    CHECK((gpu.status() & (1u << 26)) != 0);  // and for a command
    CHECK(gpu.read() == 0x22221111);
    CHECK((gpu.status() & (1u << 27)) == 0);  // and done
}

TEST_CASE("a polyline runs until its terminator")
{
    Gpu gpu;

    gpu.write_gp0(gp0(0x48));  // polyline, flat shaded
    gpu.write_gp0(0x00000000);
    gpu.write_gp0(0x00100010);
    // Further vertices, however many, are consumed rather than being
    // read as new commands.
    gpu.write_gp0(0x00200020);
    gpu.write_gp0(0x00300030);
    gpu.write_gp0(0x55555555);

    // Back in step: the next word starts a command again.
    gpu.write_gp0(gp0(0xE1) | 0x123);
    CHECK((gpu.status() & 0x7FF) == 0x123);
}

TEST_CASE("GP1 resets the GPU and abandons a partial command")
{
    Gpu gpu;
    gpu.write_gp0(gp0(0xE1) | 0x3FF);
    gpu.write_gp0(gp0(0x02));  // a three-word command, left unfinished

    SUBCASE("a full reset clears the drawing registers too")
    {
        gpu.write_gp1(0x00000000);
        CHECK((gpu.status() & 0x7FF) == 0);
    }
    SUBCASE("resetting the command buffer leaves them alone")
    {
        gpu.write_gp1(0x01000000);
        CHECK((gpu.status() & 0x7FF) == 0x3FF);
    }

    CHECK(gpu.command_words == 0);
    // Either way the next word is read as a command.
    gpu.write_gp0(gp0(0xE6) | 0x1);
    CHECK((gpu.status() & (1u << 11)) != 0);
}

TEST_CASE("horizontal blanking is whatever the scanline has left over")
{
    Gpu gpu;

    // The window the GPU comes up with is 2560 of the scanline's 3413
    // cycles, leaving 853 of the GPU's — 542 of the CPU's — blanked.
    CHECK(gpu.hblank_cycles() == 542);

    // Narrowing the picture lengthens the blank on either side of it.
    gpu.write_gp1((0x06u << 24) | (0x900u << 12) | 0x300u);
    CHECK(gpu.hblank_cycles() == 1194);

    // A window wider than the scanline leaves nothing at all.
    gpu.write_gp1((0x06u << 24) | (0xFFFu << 12));
    CHECK(gpu.hblank_cycles() == 0);
}

TEST_CASE("the DMA direction decides what the request bit answers")
{
    Gpu gpu;
    constexpr u32 DMA_REQUEST = 1u << 25;

    gpu.write_gp1(0x04000000);  // off
    CHECK((gpu.status() & DMA_REQUEST) == 0);
    CHECK((gpu.status() & (0x3u << 29)) == 0);

    gpu.write_gp1(0x04000002);  // CPU to GP0, which is ready
    CHECK((gpu.status() & DMA_REQUEST) != 0);
    CHECK((gpu.status() & (0x3u << 29)) == (2u << 29));

    // Mid-transfer it is still asking: the pixels going into VRAM are
    // exactly what the channel is there to deliver.
    gpu.write_gp0(gp0(0xA0));
    gpu.write_gp0(0);
    gpu.write_gp0((1u << 16) | 2);
    CHECK((gpu.status() & DMA_REQUEST) != 0);

    // The other direction has nothing to send until a read is asked
    // for, and answers the same bit with that.
    gpu.write_gp0(0x11112222);
    gpu.write_gp1(0x04000003);  // VRAM to CPU
    CHECK((gpu.status() & DMA_REQUEST) == 0);
}

TEST_CASE("the GPU ports are reachable through the bus")
{
    LooseBus bus;

    bus->write32(Gpu::GP1, 0x08000000 | (1u << 3));
    CHECK((bus->read32(Gpu::GP1) & (1u << 20)) != 0);
    CHECK(bus->read32(Gpu::GP1) == bus->gpu.status());

    bus->write32(Gpu::GP0, gp0(0xE1) | 0x2A);
    CHECK((bus->gpu.status() & 0x7FF) == 0x2A);

    // GP1(10h) loads GPUREAD, which is the other half of 0x1F801810.
    bus->write32(Gpu::GP1, 0x10000007);
    CHECK(bus->read32(Gpu::GP0) == 2);  // GPU version
}
