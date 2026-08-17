#include <array>

#include <doctest/doctest.h>

#include "bus.h"
#include "machine.h"
#include "mdec.h"

namespace {

constexpr u32 COMMAND = Mdec::COMMAND;
constexpr u32 CONTROL = Mdec::CONTROL;

// The three commands, as the top three bits of a command word.
constexpr u32 DECODE = 1u << 29;
constexpr u32 SET_QUANT_TABLES = 2u << 29;
constexpr u32 SET_SCALE_TABLE = 3u << 29;

constexpr u32 DEPTH_FOUR_BIT = 0u << 27;
constexpr u32 DEPTH_EIGHT_BIT = 1u << 27;
constexpr u32 DEPTH_TWENTY_FOUR_BIT = 2u << 27;
constexpr u32 DEPTH_FIFTEEN_BIT = 3u << 27;

constexpr u32 RESET = 1u << 31;

// The status field that says how many parameter words are still
// wanted, and the value it holds when none are.
constexpr u32 WORDS_LEFT = 0xFFFF;
constexpr u32 BUSY = 1u << 29;
constexpr u32 OUTPUT_EMPTY = 1u << 31;

// A block that is one DC coefficient and nothing else: the code
// carrying it, and then the end of the block. Two codes make a word.
constexpr u32 flat_block(u32 factor, u32 level)
{
    const u32 dc = (factor << 10) | level;
    return dc | (0xFE00u << 16);
}

// Feeds the decoder the codes for `blocks` flat blocks.
void write_blocks(Bus& bus, u32 blocks)
{
    for (u32 block = 0; block < blocks; block++) {
        bus.write32(COMMAND, flat_block(1, 100));
    }
}

// Takes everything the decoder is holding, a word at a time.
u32 drain(Bus& bus)
{
    u32 words = 0;
    while ((bus.read32(CONTROL) & OUTPUT_EMPTY) == 0) {
        bus.read32(COMMAND);
        words++;
        REQUIRE(words < 4096);
    }
    return words;
}

}  // namespace

TEST_CASE("a command says how many words follow it")
{
    const LooseBus bus;

    bus->write32(COMMAND, SET_QUANT_TABLES);
    CHECK((bus->read32(CONTROL) & 0xFFFF) == 15);
    CHECK((bus->read32(CONTROL) & BUSY) != 0);

    for (u32 word = 0; word < 16; word++) {
        bus->write32(COMMAND, 0x04030201);
    }

    // Out of words, and so out of the command.
    CHECK((bus->read32(CONTROL) & 0xFFFF) == WORDS_LEFT);
    CHECK((bus->read32(CONTROL) & BUSY) == 0);
    CHECK(bus->mdec.quant_luma[0] == 1);
    CHECK(bus->mdec.quant_luma[3] == 4);
    CHECK(bus->mdec.quant_luma[63] == 4);
    // Only the luminance table was asked for, so the other is untouched.
    CHECK(bus->mdec.quant_chroma[0] == 0);
}

TEST_CASE("the colour form of the command loads both tables")
{
    const LooseBus bus;

    bus->write32(COMMAND, SET_QUANT_TABLES | 1);
    CHECK((bus->read32(CONTROL) & 0xFFFF) == 31);

    for (u32 word = 0; word < 16; word++) {
        bus->write32(COMMAND, 0x01010101);
    }
    for (u32 word = 0; word < 16; word++) {
        bus->write32(COMMAND, 0x02020202);
    }

    CHECK((bus->read32(CONTROL) & BUSY) == 0);
    CHECK(bus->mdec.quant_luma[63] == 1);
    CHECK(bus->mdec.quant_chroma[0] == 2);
    CHECK(bus->mdec.quant_chroma[63] == 2);
}

TEST_CASE("the scale table arrives as signed halfwords")
{
    const LooseBus bus;

    bus->write32(COMMAND, SET_SCALE_TABLE);
    for (u32 word = 0; word < 32; word++) {
        bus->write32(COMMAND, 0xFFFF0004);
    }

    CHECK(bus->mdec.scale[0] == 4);
    CHECK(bus->mdec.scale[1] == -1);
    CHECK(bus->mdec.scale[63] == -1);
}

TEST_CASE("a reset abandons the command and empties the output")
{
    const LooseBus bus;

    bus->write32(COMMAND, DECODE | DEPTH_FIFTEEN_BIT | 6);
    write_blocks(*bus, 6);
    REQUIRE((bus->read32(CONTROL) & OUTPUT_EMPTY) == 0);

    bus->write32(CONTROL, RESET);
    CHECK(bus->read32(CONTROL) == 0x80040000);
}

TEST_CASE("what a decode produces is as wide as its depth")
{
    const LooseBus bus;

    SUBCASE("four bits a pixel, one block")
    {
        bus->write32(COMMAND, DECODE | DEPTH_FOUR_BIT | 1);
        write_blocks(*bus, 1);
        CHECK(drain(*bus) == 8);
    }

    SUBCASE("eight bits a pixel, one block")
    {
        bus->write32(COMMAND, DECODE | DEPTH_EIGHT_BIT | 1);
        write_blocks(*bus, 1);
        CHECK(drain(*bus) == 16);
    }

    SUBCASE("fifteen bits a pixel, six blocks to the macroblock")
    {
        bus->write32(COMMAND, DECODE | DEPTH_FIFTEEN_BIT | 6);
        write_blocks(*bus, 6);
        CHECK(drain(*bus) == 128);
    }

    SUBCASE("twenty-four bits a pixel, six blocks to the macroblock")
    {
        bus->write32(COMMAND, DECODE | DEPTH_TWENTY_FOUR_BIT | 6);
        write_blocks(*bus, 6);
        CHECK(drain(*bus) == 192);
    }
}

TEST_CASE("a macroblock comes out as sixteen-pixel rows")
{
    const LooseBus bus;

    // Quantisation of sixteen throughout, and a scale table with one
    // entry: the inverse DCT then leaves the block's first coefficient
    // in its top-left pixel and zero everywhere else, so each of the
    // four luminance blocks marks one corner of the macroblock and
    // nothing else moves.
    bus->write32(COMMAND, SET_QUANT_TABLES | 1);
    for (u32 word = 0; word < 32; word++) {
        bus->write32(COMMAND, 0x10101010);
    }
    bus->write32(COMMAND, SET_SCALE_TABLE);
    bus->write32(COMMAND, 0x00004000);
    for (u32 word = 1; word < 32; word++) {
        bus->write32(COMMAND, 0);
    }

    bus->write32(COMMAND, DECODE | DEPTH_FIFTEEN_BIT | 6);
    bus->write32(COMMAND, flat_block(1, 0));  // Cr: no colour
    bus->write32(COMMAND, flat_block(1, 0));  // Cb
    for (u32 luma = 0; luma < 4; luma++) {
        bus->write32(COMMAND, flat_block(1, 64));
    }

    std::array<u32, 128> words{};
    for (u32& word : words) {
        word = bus->read32(COMMAND);
    }

    // Grey is what a block of nothing decodes to, and the marked
    // corner is brighter.
    constexpr u32 GREY = 0x4210;
    constexpr u32 MARKED = 0x6318;

    // Two pixels to the word, sixteen pixels to the row: the second
    // block's corner lands four words in, on the same row as the
    // first, and the two below it eight rows further on. Were the
    // blocks written out one after another instead, they would be at
    // words 32, 64 and 96.
    CHECK((words[0] & 0xFFFF) == MARKED);
    CHECK((words[4] & 0xFFFF) == MARKED);
    CHECK((words[64] & 0xFFFF) == MARKED);
    CHECK((words[68] & 0xFFFF) == MARKED);
    CHECK(words[1] == (GREY << 16 | GREY));
    CHECK(words[32] == (GREY << 16 | GREY));
    CHECK(words[96] == (GREY << 16 | GREY));
}

TEST_CASE("a macroblock arrives only once all six of its blocks have")
{
    const LooseBus bus;

    bus->write32(COMMAND, DECODE | DEPTH_FIFTEEN_BIT | 6);
    write_blocks(*bus, 5);
    CHECK((bus->read32(CONTROL) & OUTPUT_EMPTY) != 0);

    write_blocks(*bus, 1);
    CHECK((bus->read32(CONTROL) & OUTPUT_EMPTY) == 0);
    CHECK(drain(*bus) == 128);
}

TEST_CASE("a block split across two transfers waits for the rest of it")
{
    const LooseBus bus;

    bus->write32(COMMAND, DECODE | DEPTH_EIGHT_BIT | 2);
    // Half a block: the coefficient, with the end of it still to come.
    bus->write32(COMMAND, (1u << 10) | 100);
    CHECK((bus->read32(CONTROL) & OUTPUT_EMPTY) != 0);

    bus->write32(COMMAND, 0xFE00);
    CHECK(drain(*bus) == 16);
}

TEST_CASE("the decoder is fed and emptied by its two DMA channels")
{
    const LooseBus bus;

    constexpr u32 SOURCE = 0x00010000;
    constexpr u32 DESTINATION = 0x00020000;
    constexpr u32 BLOCKS = 6;

    for (u32 block = 0; block < BLOCKS; block++) {
        bus->write32(SOURCE + block * 4, flat_block(1, 100));
    }

    bus->write32(Dma::BASE + 0x70, 0x88888888);
    bus->write32(COMMAND, DECODE | DEPTH_FIFTEEN_BIT | BLOCKS);

    // Channel 0 in: one block of BLOCKS words, RAM to device.
    bus->write32(Dma::BASE + 0x00, SOURCE);
    bus->write32(Dma::BASE + 0x04, BLOCKS | (1u << 16));
    bus->write32(Dma::BASE + 0x08, (1u << 24) | (1u << 9) | 1);

    CHECK((bus->read32(CONTROL) & BUSY) == 0);
    CHECK((bus->read32(CONTROL) & OUTPUT_EMPTY) == 0);

    // Channel 1 out: the 128 words of a 15-bit macroblock, back to RAM.
    bus->write32(Dma::BASE + 0x10, DESTINATION);
    bus->write32(Dma::BASE + 0x14, 128 | (1u << 16));
    bus->write32(Dma::BASE + 0x18, (1u << 24) | (1u << 9));

    CHECK((bus->read32(CONTROL) & OUTPUT_EMPTY) != 0);

    // A flat block is a flat macroblock, whatever the tables make of
    // its one coefficient.
    const u32 first = bus->read32(DESTINATION);
    for (u32 word = 1; word < 128; word++) {
        REQUIRE(bus->read32(DESTINATION + word * 4) == first);
    }
}
