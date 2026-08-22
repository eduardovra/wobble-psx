#pragma once

#include <array>
#include <vector>

#include "types.h"

struct State;

// The MDEC: the macroblock decoder, which is how the console shows
// video. It is a JPEG decompressor with the front half taken off —
// dequantise, inverse DCT, and colour space conversion — and it is
// spoken to through two words of address space:
//
//   0x1F801820  the command going in, and the picture coming back out
//   0x1F801824  what it is doing, and the control bits that reset it
//
// It does not read the compressed bitstream. The Huffman coding a
// movie is stored in is undone by the CPU, which hands the decoder a
// stream of 16-bit codes instead: six bits saying how many zero
// coefficients to skip, ten bits of signed level, and 0xFE00 for the
// end of a block. So the hard part of a codec is software here as it
// was on the console, and what this decodes is the halfway form.
//
// A command says how many words follow it, and there are three:
// decode, which takes a stream of those codes; set the quantisation
// tables; and set the scale table the inverse DCT multiplies by. The
// two tables are what a game loads once at the start of a movie, and
// nothing decodes correctly before they arrive.
//
// Blocks are 8x8 and come in sixes: Cr, Cb, and then the four Y blocks
// of the 16x16 macroblock those two chroma blocks are shared across —
// which is why colour output arrives 16 pixels square. The two
// monochrome depths skip all of that and give one 8x8 block per block
// decoded.
//
// The timing is not the hardware's. A real MDEC takes time over a
// macroblock and stalls the channel feeding it while it does; here a
// word is decoded the moment it is written, so the output is ready
// the instant the last word of a macroblock arrives rather than a
// while after it. Software sees the same words in the same order, and
// sooner. What it does not see is an output FIFO handing back words
// that are not in it: the channel reading the picture out waits for a
// block's worth, which is what `data_out_words` is for.
struct Mdec {
    static constexpr u32 BASE = 0x1F801820;
    static constexpr u32 END = 0x1F801828;

    // Writes go to the command port and reads come back from the data
    // port, but they are the same address: which one it is depends on
    // the direction, not on the bits.
    static constexpr u32 COMMAND = 0x1F801820;
    static constexpr u32 CONTROL = 0x1F801824;

    // The output depths, as the decode command numbers them.
    enum class Depth : u32 {
        FourBit = 0,
        EightBit = 1,
        TwentyFourBit = 2,
        FifteenBit = 3,
    };

    // What the words after a command are for. A command nobody
    // recognises still says how many words it brought, so they can be
    // swallowed rather than decoded as something else.
    enum class Parameters : u32 {
        None,
        Macroblocks,
        QuantTables,
        ScaleTable,
        Ignored,
    };

    // The six blocks a colour macroblock is made of, in the order they
    // arrive. Monochrome uses the first alone.
    static constexpr u32 BLOCKS_PER_MACROBLOCK = 6;
    static constexpr u32 BLOCK_PIXELS = 64;

    void reset();
    void visit_state(State& state);

    u32 read_register(u32 phys);
    void write_register(u32 phys, u32 value);

    // DMA channels 0 and 1. They do what the two ports above do — the
    // controller is only moving words software could have moved
    // itself, which is why a game can use either and some use both.
    void write_data(u32 word);
    u32 read_data();

    // MDEC1 as software reads it.
    u32 status() const;

    bool data_out_ready() const { return output_position < output.size(); }

    // How many words are waiting to be taken away. The DMA controller
    // asks before it takes a block, since a block half-full of words
    // the decoder has not made yet is a block of zeroes.
    u32 data_out_words() const
    {
        return static_cast<u32>(output.size() - output_position);
    }

    // The two quantisation tables, luminance and chrominance, and the
    // matrix the inverse DCT is done with. All three are set by
    // commands and none has a sensible default: a game loads them
    // before it decodes anything.
    std::array<u8, 64> quant_luma{};
    std::array<u8, 64> quant_chroma{};
    std::array<s16, 64> scale{};

    // The command in progress, and status bits 15-0 exactly as they
    // read: one less than the number of parameter words still wanted,
    // so 0xFFFF means none. A reset leaves it at zero, which is a
    // cleared register rather than a count.
    Parameters expecting = Parameters::None;
    bool busy = false;
    u16 words_left = 0;

    // How many parameter words have arrived, which is where in the
    // table the next one goes.
    u32 parameter_index = 0;

    // What the decode command asked its output to look like.
    Depth depth = Depth::FourBit;
    bool output_signed = false;
    bool output_bit15 = false;

    // The two request lines, switched on by the control register. The
    // DMA controller here does not look at them, but software reads
    // them back in the status register.
    bool dma_in_enabled = false;
    bool dma_out_enabled = false;

    // Which block of the macroblock is being decoded, in arrival
    // order.
    u32 current_block = 0;

    std::array<std::array<s16, BLOCK_PIXELS>, BLOCKS_PER_MACROBLOCK> blocks{};

    // The codes waiting to be decoded and the words waiting to be
    // taken away. Both are FIFOs that a position walks through and
    // that empty themselves once it reaches the end, rather than
    // arrays with a size the hardware would have fixed: a game decides
    // how much of a frame it sends in one transfer, and there is no
    // depth here that would make it wait.
    std::vector<u16> input;
    u32 input_position = 0;
    std::vector<u32> output;
    u32 output_position = 0;

private:
    void write_command(u32 word);
    void start_command(u32 word);
    void take_parameter(u32 word);

    bool colour_output() const;

    // Decodes as many complete blocks as the input holds, and emits a
    // macroblock for every six of them — or every one, monochrome.
    void decode_available();

    // Returns false when the block was not all there, having consumed
    // nothing: the caller puts the input position back and waits for
    // the rest of the transfer.
    bool decode_block(std::array<s16, BLOCK_PIXELS>& block,
                      const std::array<u8, BLOCK_PIXELS>& quant);

    bool next_code(u16& code);

    void emit_macroblock();
    void emit_mono_block();

    void push_output(u32 word);
};
