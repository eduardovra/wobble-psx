#include "mdec.h"

#include <algorithm>
#include <cstddef>

#include "savestate.h"

namespace {

// Where in the block the coefficients land, in the order they arrive:
// a zigzag out from the top-left corner. Coding them along that path
// puts the low frequencies first and so gathers the zeroes at the end,
// where a run length can pay for them cheaply.
constexpr std::array<u8, 64> ZIGZAG = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// The code that ends a block. It also serves as padding, since a
// stream of 16-bit codes has to end on a word boundary: a run of 63
// zeroes runs off the end of the block either way.
constexpr u16 END_OF_BLOCK = 0xFE00;

// A colour macroblock is sixteen pixels square, and the four luminance
// blocks are its quarters.
constexpr u32 MACROBLOCK_WIDTH = 16;
constexpr u32 MACROBLOCK_PIXELS = MACROBLOCK_WIDTH * MACROBLOCK_WIDTH;

// The level a code carries is ten bits with the sign in the top one.
s32 signed10(u16 code) { return static_cast<s32>(u32{code} << 22) >> 22; }

// A component on its way out. The decoder works in signed values
// either side of zero, and the flag in the decode command says whether
// software wants them that way or with the zero moved to the middle of
// the range.
u8 to_component(s32 value, bool output_signed)
{
    const s32 clamped = std::clamp(value, -128, 127);
    if (output_signed) {
        return static_cast<u8>(clamped);
    }
    return static_cast<u8>(clamped + 128);
}

struct Colour {
    u8 red = 0;
    u8 green = 0;
    u8 blue = 0;
};

// The inverse DCT, as two passes of a matrix multiply against the
// table software loaded. That table is the transform: nothing here
// knows a cosine, and a game that has not sent one gets zeroes back.
//
// The two passes keep their full product and divide once at the end,
// where the bit below the result rounds it. Halving the precision
// between them costs the odd value a step of brightness, which is
// visible in a still and free to avoid.
void inverse_dct(std::array<s16, 64>& block, const std::array<s16, 64>& scale)
{
    std::array<s64, 64> columns{};
    for (u32 x = 0; x < 8; x++) {
        for (u32 y = 0; y < 8; y++) {
            s64 sum = 0;
            for (u32 z = 0; z < 8; z++) {
                sum += s64{scale[z * 8 + y]} * block[x + z * 8];
            }
            columns[x + y * 8] = sum;
        }
    }

    for (u32 x = 0; x < 8; x++) {
        for (u32 y = 0; y < 8; y++) {
            s64 sum = 0;
            for (u32 z = 0; z < 8; z++) {
                sum += columns[z + y * 8] * scale[x + z * 8];
            }
            const s64 rounding = (sum >> 31) & 1;
            block[x + y * 8] = static_cast<s16>((sum >> 32) + rounding);
        }
    }
}

// One luminance block turned into its quarter of the macroblock. The
// two chroma blocks cover all sixteen pixels of a side between them,
// so every second pixel across and down shares a chroma sample — the
// halving that makes the format worth the trouble, and the reason the
// quarter has to know where in the macroblock it sits.
void yuv_to_rgb(std::array<Colour, 64>& pixels,
                u32 left,
                u32 top,
                const std::array<s16, 64>& luma,
                const std::array<s16, 64>& cr,
                const std::array<s16, 64>& cb,
                bool output_signed)
{
    for (u32 y = 0; y < 8; y++) {
        for (u32 x = 0; x < 8; x++) {
            const u32 chroma = ((x + left) / 2) + ((y + top) / 2) * 8;
            const s32 cr_value = cr[chroma];
            const s32 cb_value = cb[chroma];
            // The three constants are 1.402, 0.344136 and 0.714136 in
            // sixteen fractional bits, and the division is a division
            // rather than a shift: it has to round towards zero from
            // both sides, or half the picture comes out a step dark.
            const s32 red = (91881 * cr_value) / 65536;
            const s32 green = (-22554 * cb_value - 46802 * cr_value) / 65536;
            const s32 blue = (116130 * cb_value) / 65536;
            const s32 luminance = luma[x + y * 8];

            Colour& pixel = pixels[x + y * 8];
            pixel.red = to_component(luminance + red, output_signed);
            pixel.green = to_component(luminance + green, output_signed);
            pixel.blue = to_component(luminance + blue, output_signed);
        }
    }
}

// A pixel as the 15-bit depth writes it: five bits a component, and
// the sixteenth bit whatever the decode command said it should be —
// the GPU reads that bit as the mask bit, so a movie can decide there
// what it will be drawn over.
u32 to_five_bits(u8 component)
{
    return std::min<u32>((u32{component} + 4) >> 3, 31);
}

u32 to_fifteen_bit(const Colour& pixel, bool bit15)
{
    u32 value = u32{bit15} << 15;
    value |= to_five_bits(pixel.blue) << 10;
    value |= to_five_bits(pixel.green) << 5;
    value |= to_five_bits(pixel.red);
    return value & 0xFFFF;
}

}  // namespace

void Mdec::reset()
{
    quant_luma = {};
    quant_chroma = {};
    scale = {};
    expecting = Parameters::None;
    busy = false;
    words_left = 0;
    parameter_index = 0;
    depth = Depth::FourBit;
    output_signed = false;
    output_bit15 = false;
    dma_in_enabled = false;
    dma_out_enabled = false;
    current_block = 0;
    blocks = {};
    input.clear();
    input_position = 0;
    output.clear();
    output_position = 0;
}

void Mdec::visit_state(State& state)
{
    state(quant_luma);
    state(quant_chroma);
    state(scale);
    state(expecting);
    state(busy);
    state(words_left);
    state(parameter_index);
    state(depth);
    state(output_signed);
    state(output_bit15);
    state(dma_in_enabled);
    state(dma_out_enabled);
    state(current_block);
    state(blocks);
    state(input);
    state(input_position);
    state(output);
    state(output_position);
}

u32 Mdec::read_register(u32 phys)
{
    if (phys == COMMAND) {
        return read_data();
    }
    return status();
}

void Mdec::write_register(u32 phys, u32 value)
{
    if (phys == COMMAND) {
        write_command(value);
        return;
    }

    // Bit 31 aborts whatever is going on, which is how software starts
    // a movie without knowing what the last one left behind. The three
    // tables survive it: they are memory the decoder was given rather
    // than anything it is doing.
    if ((value & (1u << 31)) != 0) {
        expecting = Parameters::None;
        busy = false;
        words_left = 0;
        parameter_index = 0;
        current_block = 0;
        depth = Depth::FourBit;
        output_signed = false;
        output_bit15 = false;
        input.clear();
        input_position = 0;
        output.clear();
        output_position = 0;
    }
    dma_in_enabled = (value & (1u << 30)) != 0;
    dma_out_enabled = (value & (1u << 29)) != 0;
}

void Mdec::write_data(u32 word) { write_command(word); }

u32 Mdec::read_data()
{
    if (!data_out_ready()) {
        return 0;
    }
    const u32 word = output[output_position++];
    if (!data_out_ready()) {
        output.clear();
        output_position = 0;
    }
    return word;
}

u32 Mdec::status() const
{
    // The block numbering software sees is not the order they arrive
    // in: the four luminance blocks are 0 to 3 and the chroma pair
    // comes after them, while a macroblock is decoded chroma first.
    static constexpr std::array<u32, BLOCKS_PER_MACROBLOCK> REPORTED = {
        4, 5, 0, 1, 2, 3};

    u32 value = words_left;
    value |= REPORTED[current_block] << 16;
    value |= u32{output_bit15} << 23;
    value |= u32{output_signed} << 24;
    value |= static_cast<u32>(depth) << 25;
    if (dma_out_enabled && data_out_ready()) {
        value |= 1u << 27;
    }
    if (dma_in_enabled && busy) {
        value |= 1u << 28;
    }
    if (busy) {
        value |= 1u << 29;
    }
    // Bit 30 says the data-in FIFO is full, which cannot happen here:
    // a word written is decoded before the store that wrote it ends.
    if (!data_out_ready()) {
        value |= 1u << 31;
    }
    return value;
}

void Mdec::write_command(u32 word)
{
    if (busy) {
        take_parameter(word);
        return;
    }
    start_command(word);
}

void Mdec::start_command(u32 word)
{
    const u32 command = word >> 29;
    u32 words = 0;

    switch (command) {
    case 1:
        depth = static_cast<Depth>((word >> 27) & 3);
        output_signed = (word & (1u << 26)) != 0;
        output_bit15 = (word & (1u << 25)) != 0;
        words = word & 0xFFFF;
        expecting = Parameters::Macroblocks;
        break;
    case 2:
        // Bit 0 asks for the chrominance table as well as the
        // luminance one, at 64 bytes each.
        words = (word & 1) != 0 ? 32 : 16;
        expecting = Parameters::QuantTables;
        break;
    case 3:
        words = 32;
        expecting = Parameters::ScaleTable;
        break;
    default:
        expecting = Parameters::Ignored;
        words = word & 0xFFFF;
        break;
    }

    parameter_index = 0;
    busy = words > 0;
    words_left = static_cast<u16>(words - 1);
    if (!busy) {
        expecting = Parameters::None;
    }
}

void Mdec::take_parameter(u32 word)
{
    switch (expecting) {
    case Parameters::Macroblocks:
        // Two codes to the word, the first in the low half.
        input.push_back(static_cast<u16>(word));
        input.push_back(static_cast<u16>(word >> 16));
        decode_available();
        break;
    case Parameters::QuantTables: {
        std::array<u8, 64>& table =
            parameter_index < 16 ? quant_luma : quant_chroma;
        const u32 offset = (parameter_index % 16) * 4;
        for (u32 byte = 0; byte < 4; byte++) {
            table[offset + byte] = static_cast<u8>(word >> (byte * 8));
        }
        break;
    }
    case Parameters::ScaleTable: {
        // Two entries to the word, since the table is halfwords.
        const std::size_t entry = std::size_t{parameter_index} * 2;
        scale[entry] = static_cast<s16>(word);
        scale[entry + 1] = static_cast<s16>(word >> 16);
        break;
    }
    default:
        break;
    }

    parameter_index++;
    const bool last = words_left == 0;
    words_left--;
    if (last) {
        busy = false;
        expecting = Parameters::None;
    }
}

bool Mdec::colour_output() const
{
    return depth == Depth::TwentyFourBit || depth == Depth::FifteenBit;
}

void Mdec::decode_available()
{
    const u32 needed = colour_output() ? BLOCKS_PER_MACROBLOCK : 1;

    for (;;) {
        const u32 start = input_position;
        const bool chroma = colour_output() && current_block < 2;
        const std::array<u8, 64>& quant = chroma ? quant_chroma : quant_luma;
        if (!decode_block(blocks[current_block], quant)) {
            // The transfer stopped part way through a block. Nothing
            // is thrown away: the codes stay where they are and the
            // block is decoded again when the rest of it arrives.
            input_position = start;
            break;
        }

        current_block++;
        if (current_block < needed) {
            continue;
        }
        current_block = 0;
        if (colour_output()) {
            emit_macroblock();
        } else {
            emit_mono_block();
        }
    }

    if (input_position >= input.size()) {
        input.clear();
        input_position = 0;
    }
}

bool Mdec::decode_block(std::array<s16, BLOCK_PIXELS>& block,
                        const std::array<u8, BLOCK_PIXELS>& quant)
{
    u16 code = 0;
    do {
        if (!next_code(code)) {
            return false;
        }
    } while (code == END_OF_BLOCK);

    block = {};

    // The first code of a block carries the quantisation factor the
    // rest of it is scaled by, alongside the DC coefficient. A factor
    // of zero is the exception it looks like: the coefficients are
    // doubled rather than scaled, and go in in the order they arrive
    // rather than along the zigzag.
    const u32 factor = (code >> 10) & 0x3F;
    s32 value = signed10(code) * quant[0];

    for (u32 index = 0;;) {
        if (factor == 0) {
            value = signed10(code) * 2;
        }
        value = std::clamp(value, -0x400, 0x3FF);
        if (factor > 0) {
            block[ZIGZAG[index]] = static_cast<s16>(value);
        } else {
            block[index] = static_cast<s16>(value);
        }

        // A block that has all 64 of its coefficients is over without
        // an end-of-block code to say so, and waiting for one would
        // stop on the last block of a transfer.
        if (index == 63) {
            break;
        }

        if (!next_code(code)) {
            return false;
        }
        index += ((code >> 10) & 0x3F) + 1;
        if (index > 63) {
            break;
        }
        const s32 quantised = signed10(code) * s32{quant[index]};
        value = (quantised * static_cast<s32>(factor) + 4) / 8;
    }

    inverse_dct(block, scale);
    return true;
}

bool Mdec::next_code(u16& code)
{
    if (input_position >= input.size()) {
        return false;
    }
    code = input[input_position++];
    return true;
}

void Mdec::emit_macroblock()
{
    // A macroblock leaves as it was drawn:
    // it leaves as sixteen-pixel rows, top to bottom. Software copies
    // what it reads straight into a sixteen-wide strip of VRAM and
    // gets the picture back, which is the only reason a movie can be
    // played by moving words about and nothing else.
    std::array<Colour, MACROBLOCK_PIXELS> pixels{};
    for (u32 quarter = 0; quarter < 4; quarter++) {
        const u32 left = (quarter & 1) * 8;
        const u32 top = (quarter >> 1) * 8;
        std::array<Colour, 64> quadrant{};
        yuv_to_rgb(quadrant,
                   left,
                   top,
                   blocks[2 + quarter],
                   blocks[0],
                   blocks[1],
                   output_signed);
        for (u32 index = 0; index < 64; index++) {
            const u32 x = left + index % 8;
            const u32 y = top + index / 8;
            pixels[x + y * MACROBLOCK_WIDTH] = quadrant[index];
        }
    }

    if (depth == Depth::FifteenBit) {
        for (u32 index = 0; index < MACROBLOCK_PIXELS; index += 2) {
            u32 pair = to_fifteen_bit(pixels[index], output_bit15);
            pair |= to_fifteen_bit(pixels[index + 1], output_bit15) << 16;
            push_output(pair);
        }
        return;
    }

    // Three bytes a pixel, packed end to end with no regard for where
    // a word boundary falls in one. Three bytes divide into 256 pixels
    // evenly, so a macroblock still ends on a word.
    u32 word = 0;
    u32 filled = 0;
    for (const Colour& pixel : pixels) {
        const std::array<u8, 3> components = {
            pixel.red, pixel.green, pixel.blue};
        for (const u8 component : components) {
            word |= u32{component} << (filled * 8);
            filled++;
            if (filled == 4) {
                push_output(word);
                word = 0;
                filled = 0;
            }
        }
    }
}

void Mdec::emit_mono_block()
{
    const std::array<s16, BLOCK_PIXELS>& block = blocks[0];

    if (depth == Depth::EightBit) {
        for (u32 index = 0; index < BLOCK_PIXELS; index += 4) {
            u32 word = 0;
            for (u32 byte = 0; byte < 4; byte++) {
                const u8 value =
                    to_component(block[index + byte], output_signed);
                word |= u32{value} << (byte * 8);
            }
            push_output(word);
        }
        return;
    }

    // Four bits a pixel: the same value with the low half dropped,
    // eight of them to the word and the leftmost in the low nibble.
    for (u32 index = 0; index < BLOCK_PIXELS; index += 8) {
        u32 word = 0;
        for (u32 nibble = 0; nibble < 8; nibble++) {
            const u8 value = to_component(block[index + nibble], output_signed);
            word |= u32{static_cast<u8>(value >> 4)} << (nibble * 4);
        }
        push_output(word);
    }
}

void Mdec::push_output(u32 word) { output.push_back(word); }
