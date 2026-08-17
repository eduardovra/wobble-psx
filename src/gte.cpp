#include "gte.h"

#include <algorithm>
#include <bit>

#include "savestate.h"

namespace {

// The operations, by the low six bits of the instruction word.
enum Operation : u32 {
    RTPS = 0x01,   // perspective transform, one vertex
    NCLIP = 0x06,  // the winding of a projected triangle
    OP = 0x0C,     // cross product with the rotation matrix diagonal
    DPCS = 0x10,   // depth cue the vertex colour
    INTPL = 0x11,  // depth cue IR1..3
    MVMVA = 0x12,  // multiply by a chosen matrix and vector
    NCDS = 0x13,   // normal colour, depth cued, one vertex
    CDP = 0x14,    // colour depth cue
    NCDT = 0x16,   // normal colour, depth cued, three vertices
    NCCS = 0x1B,   // normal colour with the vertex colour, one vertex
    CC = 0x1C,     // colour colour
    NCS = 0x1D,    // normal colour, one vertex
    NCT = 0x20,    // normal colour, three vertices
    SQR = 0x28,    // square of IR1..3
    DCPL = 0x29,   // depth cue the lit vertex colour
    DPCT = 0x2A,   // depth cue the colour FIFO, three times
    AVSZ3 = 0x2D,  // average of three depths
    AVSZ4 = 0x2E,  // average of four depths
    RTPT = 0x30,   // perspective transform, three vertices
    GPF = 0x3D,    // general purpose interpolation
    GPL = 0x3E,    // the same, accumulating
    NCCT = 0x3F,   // normal colour with the vertex colour, three
};

// What each operation costs, in CPU cycles, measured on hardware. The
// spread is real: RTPT does three vertices' worth of work and NCCT
// three lights' worth, so they cost what they do rather than being
// rounded to something tidy.
u32 operation_cycles(u32 operation)
{
    // Grouped by what they cost rather than by opcode, because several
    // unrelated operations happen to take the same time — they are
    // built from the same multiplier passes.
    switch (operation) {
    case SQR:
    case AVSZ3:
    case GPF:
    case GPL:
        return 5;
    case OP:
    case AVSZ4:
        return 6;
    case NCLIP:
    case DPCS:
    case INTPL:
    case MVMVA:
    case DCPL:
        return 8;
    case CC:
        return 11;
    case CDP:
        return 13;
    case NCS:
        return 14;
    case RTPS:
        return 15;
    case NCCS:
    case DPCT:
        return 17;
    case NCDS:
        return 19;
    case RTPT:
        return 23;
    case NCT:
        return 30;
    case NCCT:
        return 39;
    case NCDT:
        return 44;
    default:
        return 1;
    }
}

// FLAG, cop2r63. Bit 31 is not stored but computed: it is the or of
// every bit software is expected to care about, so one test after a
// polygon says whether any of it went wrong.
constexpr u32 FLAG_ERROR = 1u << 31;
constexpr u32 FLAG_ERROR_MASK = 0x7F87E000;
constexpr u32 FLAG_WRITABLE = 0x7FFFF000;

constexpr u32 FLAG_DIVIDE_OVERFLOW = 1u << 17;
constexpr u32 FLAG_MAC0_POSITIVE = 1u << 16;
constexpr u32 FLAG_MAC0_NEGATIVE = 1u << 15;
constexpr u32 FLAG_SX2_SATURATED = 1u << 14;
constexpr u32 FLAG_SY2_SATURATED = 1u << 13;
constexpr u32 FLAG_IR0_SATURATED = 1u << 12;
constexpr u32 FLAG_DEPTH_SATURATED = 1u << 18;

// MAC1..3 and IR1..3 each have their own bit, and they are laid out so
// the index can be subtracted from a base rather than looked up.
constexpr u32 flag_mac_positive(u32 index) { return 1u << (31 - index); }
constexpr u32 flag_mac_negative(u32 index) { return 1u << (28 - index); }
constexpr u32 flag_ir_saturated(u32 index) { return 1u << (25 - index); }
constexpr u32 flag_colour_saturated(u32 index) { return 1u << (22 - index); }

// The accumulators are 44 bits wide, which is what makes a product of
// two 16-bit values summed three times fit without rounding.
constexpr s64 MAC_MAX = (s64{1} << 43) - 1;
constexpr s64 MAC_MIN = -(s64{1} << 43);

// What is left of a 64-bit word once the accumulator has had its 44.
constexpr u32 MAC_UNUSED_BITS = 20;

constexpr s32 IR_MAX = 0x7FFF;
constexpr s32 IR_MIN = -0x8000;
constexpr s32 IR0_MAX = 0x1000;
constexpr s32 SCREEN_MIN = -0x400;
constexpr s32 SCREEN_MAX = 0x3FF;
constexpr s32 DEPTH_MAX = 0xFFFF;
constexpr s32 COLOUR_MAX = 0xFF;

// The fixed-point scale everything in here is expressed in: twelve
// fractional bits, so 1.0 is 1000h.
constexpr u32 FRACTION_BITS = 12;
constexpr s64 ONE = 1 << FRACTION_BITS;

// The table behind the perspective divide. The hardware holds 257
// bytes of reciprocal seeds and refines them with two Newton-Raphson
// steps; the seeds are a plain function of their index, so they are
// computed here rather than written out.
constexpr std::array<u8, 257> make_reciprocal_table()
{
    std::array<u8, 257> table{};
    for (u32 i = 0; i < table.size(); i++) {
        const s32 seed = 0x40000 / static_cast<s32>(i + 0x100);
        table[i] = static_cast<u8>(std::max(0, (seed + 1) / 2 - 0x101));
    }
    return table;
}

constexpr std::array<u8, 257> RECIPROCAL = make_reciprocal_table();

// Packs the two halves of a register that holds a pair of 16-bit
// values, and takes them apart again.
u32 pack(s32 low, s32 high)
{
    return (static_cast<u32>(low) & 0xFFFF) | (static_cast<u32>(high) << 16);
}

s16 low_half(u32 value) { return static_cast<s16>(value); }
s16 high_half(u32 value) { return static_cast<s16>(value >> 16); }

u32 sign_extend(s32 value) { return static_cast<u32>(value); }

// The five-word packing every 3x3 matrix uses: rows run on across
// register boundaries, and the ninth value has a register to itself.
u32 read_matrix(const Gte::Matrix& mx, u32 offset)
{
    switch (offset) {
    case 0:
        return pack(mx[0][0], mx[0][1]);
    case 1:
        return pack(mx[0][2], mx[1][0]);
    case 2:
        return pack(mx[1][1], mx[1][2]);
    case 3:
        return pack(mx[2][0], mx[2][1]);
    default:
        return sign_extend(mx[2][2]);
    }
}

void write_matrix(Gte::Matrix& mx, u32 offset, u32 value)
{
    switch (offset) {
    case 0:
        mx[0][0] = low_half(value);
        mx[0][1] = high_half(value);
        break;
    case 1:
        mx[0][2] = low_half(value);
        mx[1][0] = high_half(value);
        break;
    case 2:
        mx[1][1] = low_half(value);
        mx[1][2] = high_half(value);
        break;
    case 3:
        mx[2][0] = low_half(value);
        mx[2][1] = high_half(value);
        break;
    default:
        mx[2][2] = low_half(value);
        break;
    }
}

// IRGB and ORGB: IR1..3 seen as a 15-bit colour, five bits each, which
// is the form a GP0 command wants them in.
constexpr s32 IRGB_SCALE = 0x80;
constexpr s32 IRGB_MAX = 0x1F;

u32 to_irgb(const std::array<s16, 4>& ir)
{
    const u32 red =
        static_cast<u32>(std::clamp(ir[1] / IRGB_SCALE, 0, IRGB_MAX));
    const u32 green =
        static_cast<u32>(std::clamp(ir[2] / IRGB_SCALE, 0, IRGB_MAX));
    const u32 blue =
        static_cast<u32>(std::clamp(ir[3] / IRGB_SCALE, 0, IRGB_MAX));
    return red | (green << 5) | (blue << 10);
}

// How many bits at the top of LZCS match its sign bit — a count of
// leading zeroes for a positive value and of leading ones for a
// negative one. It exists so software can normalise a value before
// dividing by it.
s32 count_leading_sign_bits(u32 value)
{
    const bool negative = (value & (1u << 31)) != 0;
    if (negative) {
        return std::countl_one(value);
    }
    return std::countl_zero(value);
}

}  // namespace

void Gte::reset()
{
    v = {};
    rgbc = {};
    otz = 0;
    ir = {};
    sxy = {};
    sz = {};
    rgb = {};
    mac = {};
    lzcs = 0;
    lzcr = 32;
    res1 = 0;
    matrix = {};
    translation = {};
    ofx = 0;
    ofy = 0;
    h = 0;
    dqa = 0;
    dqb = 0;
    zsf3 = 0;
    zsf4 = 0;
    flag = 0;
}

void Gte::visit_state(State& state)
{
    state(v);
    state(rgbc);
    state(otz);
    state(ir);
    state(sxy);
    state(sz);
    state(rgb);
    state(mac);
    state(lzcs);
    state(lzcr);
    state(res1);
    state(matrix);
    state(translation);
    state(ofx);
    state(ofy);
    state(h);
    state(dqa);
    state(dqb);
    state(zsf3);
    state(zsf4);
    state(flag);
}

void Gte::set_mac(u32 index, s64 value, u32 shift)
{
    // The overflow is judged on the full 44-bit result, before the
    // shift throws its low bits away.
    if (value > MAC_MAX) {
        flag |= flag_mac_positive(index);
    } else if (value < MAC_MIN) {
        flag |= flag_mac_negative(index);
    }
    mac[index] = static_cast<s32>(value >> shift);
}

// A sum is checked as it is built rather than once at the end, because
// that is where the hardware can see it: a term that carries the total
// out of range sets the flag even if a later one brings it back, and a
// sum that leaves in both directions leaves both flags behind.
s64 Gte::accumulate(u32 index, s64 sum, s64 term)
{
    const s64 total = sum + term;
    if (total > MAC_MAX) {
        flag |= flag_mac_positive(index);
    } else if (total < MAC_MIN) {
        flag |= flag_mac_negative(index);
    }
    // 44 bits is all there is to hold it; anything above is dropped and
    // the sign taken from the top of what remains.
    return (total << MAC_UNUSED_BITS) >> MAC_UNUSED_BITS;
}

s32 Gte::set_mac0(s64 value)
{
    if (value > INT32_MAX) {
        flag |= FLAG_MAC0_POSITIVE;
    } else if (value < INT32_MIN) {
        flag |= FLAG_MAC0_NEGATIVE;
    }
    mac[0] = static_cast<s32>(value);
    return mac[0];
}

void Gte::set_ir(u32 index, s32 value, bool clamp_positive)
{
    const s32 low = clamp_positive ? 0 : IR_MIN;
    if (value < low || value > IR_MAX) {
        flag |= flag_ir_saturated(index);
    }
    ir[index] = static_cast<s16>(std::clamp(value, low, IR_MAX));
}

void Gte::set_mac_and_ir(u32 index, s64 value, const Modifiers& modifiers)
{
    set_mac(index, value, modifiers.shift);
    set_ir(index, mac[index], modifiers.clamp_positive);
}

void Gte::push_screen(s64 x, s64 y)
{
    if (x < SCREEN_MIN || x > SCREEN_MAX) {
        flag |= FLAG_SX2_SATURATED;
    }
    if (y < SCREEN_MIN || y > SCREEN_MAX) {
        flag |= FLAG_SY2_SATURATED;
    }
    sxy[0] = sxy[1];
    sxy[1] = sxy[2];
    sxy[2] = {static_cast<s16>(std::clamp<s64>(x, SCREEN_MIN, SCREEN_MAX)),
              static_cast<s16>(std::clamp<s64>(y, SCREEN_MIN, SCREEN_MAX))};
}

void Gte::push_depth(s64 value)
{
    if (value < 0 || value > DEPTH_MAX) {
        flag |= FLAG_DEPTH_SATURATED;
    }
    sz[0] = sz[1];
    sz[1] = sz[2];
    sz[2] = sz[3];
    sz[3] = static_cast<u16>(std::clamp<s64>(value, 0, DEPTH_MAX));
}

void Gte::push_colour()
{
    std::array<u8, 4> entry{};
    for (u32 i = 0; i < 3; i++) {
        // Shifted rather than divided: the hardware drops four bits,
        // which for a negative value rounds down and not towards zero.
        // A colour just below black divides to exactly 0 and looks in
        // range, where the shift keeps it negative and saturates.
        const s32 value = mac[i + 1] >> 4;
        if (value < 0 || value > COLOUR_MAX) {
            flag |= flag_colour_saturated(i + 1);
        }
        entry[i] = static_cast<u8>(std::clamp(value, 0, COLOUR_MAX));
    }
    entry[3] = rgbc[3];

    rgb[0] = rgb[1];
    rgb[1] = rgb[2];
    rgb[2] = entry;
}

void Gte::transform(const Matrix& mx,
                    const Vector& tr,
                    std::array<s16, 3> vec,
                    const Modifiers& modifiers)
{
    for (u32 row = 0; row < 3; row++) {
        s64 value = accumulate(row + 1, 0, s64{tr[row]} * ONE);
        for (u32 column = 0; column < 3; column++) {
            value =
                accumulate(row + 1, value, s64{mx[row][column]} * vec[column]);
        }
        set_mac_and_ir(row + 1, value, modifiers);
    }
}

void Gte::shade(const Modifiers& modifiers, bool clamp)
{
    // The vertex colour is 8-bit and IR is 1.3.12, so the product needs
    // four bits of shifting up before it is on the same scale as
    // everything else.
    for (u32 i = 0; i < 3; i++) {
        const s64 value = (s64{rgbc[i]} << 4) * ir[i + 1];
        if (clamp) {
            set_mac_and_ir(i + 1, value, modifiers);
        } else {
            set_mac(i + 1, value, 0);
        }
    }
}

void Gte::interpolate(const Modifiers& modifiers)
{
    // The far colour is where everything ends up at maximum distance;
    // IR0 says how far along the way this vertex is. The shaded colour
    // has to be kept because both steps need it — the accumulators
    // hold the difference in between.
    const std::array<s64, 3> shaded = {mac[1], mac[2], mac[3]};

    for (u32 i = 0; i < 3; i++) {
        const s64 difference = (s64{translation[2][i]} << FRACTION_BITS);
        set_mac_and_ir(i + 1, difference - shaded[i], {modifiers.shift, false});
    }
    for (u32 i = 0; i < 3; i++) {
        set_mac_and_ir(i + 1, s64{ir[i + 1]} * ir[0] + shaded[i], modifiers);
    }
}

u32 Gte::perspective_divide()
{
    const u32 divisor = sz[3];

    // Anything closer than half the projection distance is behind the
    // eye as far as this is concerned, and division by zero lands here
    // too. The result is pinned and the flag says the answer is not to
    // be trusted.
    if (h >= divisor * 2) {
        flag |= FLAG_DIVIDE_OVERFLOW;
        return 0x1FFFF;
    }

    // Normalise the divisor into 8000h..FFFFh, look up a seed for its
    // reciprocal, and refine it twice. Each step roughly doubles the
    // number of correct bits, which is why two are enough.
    const u32 shift = static_cast<u32>(std::countl_zero(sz[3]));
    const u32 numerator = static_cast<u32>(h) << shift;
    u32 normalised = divisor << shift;

    const u32 seed = RECIPROCAL[(normalised - 0x7FC0) >> 7] + 0x101u;
    normalised = (0x2000080 - normalised * seed) >> 8;
    normalised = (0x0000080 + normalised * seed) >> 8;

    // 64-bit only because the product can exceed 32 before the
    // shift brings it back; the hardware's result is 17 bits.
    const u64 product = u64{numerator} * normalised;
    return std::min<u32>(0x1FFFF, static_cast<u32>((product + 0x8000) >> 16));
}

void Gte::project(u32 vertex, const Modifiers& modifiers)
{
    const Matrix& rotation = matrix[0];
    const Vector& tr = translation[0];
    const std::array<s16, 3>& vec = v[vertex];

    for (u32 row = 0; row < 2; row++) {
        s64 value = accumulate(row + 1, 0, s64{tr[row]} * ONE);
        for (u32 column = 0; column < 3; column++) {
            value = accumulate(
                row + 1, value, s64{rotation[row][column]} * vec[column]);
        }
        set_mac_and_ir(row + 1, value, modifiers);
    }

    s64 depth = accumulate(3, 0, s64{tr[2]} * ONE);
    for (u32 column = 0; column < 3; column++) {
        depth = accumulate(3, depth, s64{rotation[2][column]} * vec[column]);
    }
    set_mac(3, depth, modifiers.shift);

    // A hardware quirk worth reproducing because software leans on it:
    // IR3 is clamped from MAC3 as usual, but the flag that says it was
    // clamped is decided from the depth *before* the command's shift.
    // With sf=0 the two disagree, and code that checks FLAG sees the
    // answer the hardware gives rather than the tidy one.
    //
    // The accumulator is 44 bits and the register it lands in is 32,
    // so the shifted depth is narrowed on the way — and a depth too
    // large to fit comes back wrapped, often negative, which is what
    // puts the vertex behind the camera rather than infinitely far in
    // front of it. The shift comes first: it is the shifted value that
    // has to fit, not the accumulator.
    const s64 unshifted = static_cast<s32>(depth >> FRACTION_BITS);
    if (unshifted < IR_MIN || unshifted > IR_MAX) {
        flag |= flag_ir_saturated(3);
    }
    const s32 low = modifiers.clamp_positive ? 0 : IR_MIN;
    ir[3] = static_cast<s16>(std::clamp<s32>(mac[3], low, IR_MAX));

    push_depth(unshifted);

    // Perspective: divide by the depth, and scale the projected vector
    // by the result. The screen offsets are in 16.16, which is why the
    // coordinates come out of the top half.
    //
    // They are taken from the full result rather than from MAC0, which
    // is only 32 bits wide. A vertex projected far enough off screen
    // overflows that register — and the coordinate the hardware then
    // saturates is the one it computed, not the wrapped remains left
    // behind. Reading it back through MAC0 turns a point way off to
    // one side into one a couple of pixels from the origin.
    const s64 factor = perspective_divide();
    const s64 screen_x = factor * ir[1] + ofx;
    const s64 screen_y = factor * ir[2] + ofy;
    set_mac0(screen_x);
    set_mac0(screen_y);
    push_screen(screen_x >> 16, screen_y >> 16);

    // Depth cueing rides on the same reciprocal: how much fog this
    // vertex gets is linear in its distance. Saturated from the whole
    // result too, for the same reason.
    const s64 depth_cue = factor * dqa + dqb;
    set_mac0(depth_cue);
    const s64 fog = depth_cue >> FRACTION_BITS;
    if (fog < 0 || fog > IR0_MAX) {
        flag |= FLAG_IR0_SATURATED;
    }
    ir[0] = static_cast<s16>(std::clamp<s64>(fog, 0, IR0_MAX));
}

void Gte::light(u32 vertex, const Modifiers& modifiers)
{
    // How much of each light falls on the vertex normal, and then what
    // colour that adds up to. The background colour is what the vertex
    // gets where no light reaches it at all.
    transform(matrix[1], {}, v[vertex], modifiers);
    transform(matrix[2], translation[1], {ir[1], ir[2], ir[3]}, modifiers);
}

void Gte::run_mvmva(u32 instruction, const Modifiers& modifiers)
{
    const auto which_matrix =
        static_cast<MatrixKind>((instruction >> 17) & 0x3);
    const u32 which_vector = (instruction >> 15) & 0x3;
    const auto which_translation =
        static_cast<TranslationKind>((instruction >> 13) & 0x3);

    // Selecting matrix 3 does not select a matrix: the hardware reads
    // a register file that has nothing there and gets a mixture of the
    // colour register and two entries of the rotation matrix. Software
    // is not supposed to ask for it, and a couple of games do.
    Matrix mx{};
    if (which_matrix == MatrixKind::Garbage) {
        const auto red = static_cast<s16>(rgbc[0] << 4);
        mx = {{{static_cast<s16>(-red), red, ir[0]},
               {matrix[0][0][2], matrix[0][0][2], matrix[0][0][2]},
               {matrix[0][1][1], matrix[0][1][1], matrix[0][1][1]}}};
    } else {
        mx = matrix[static_cast<u32>(which_matrix)];
    }

    std::array<s16, 3> vec{};
    if (which_vector == 3) {
        vec = {ir[1], ir[2], ir[3]};
    } else {
        vec = v[which_vector];
    }

    if (which_translation == TranslationKind::FarColour) {
        // The other broken combination, and this one is documented as
        // such: the far colour is added, the first column multiplied
        // in, and the whole of that thrown away except for the flags
        // it set. Only the remaining two columns reach MAC.
        for (u32 row = 0; row < 3; row++) {
            const s64 discarded = (s64{translation[2][row]} << FRACTION_BITS) +
                s64{mx[row][0]} * vec[0];
            set_mac(row + 1, discarded, modifiers.shift);
            set_ir(row + 1, mac[row + 1], false);

            const s64 value =
                s64{mx[row][1]} * vec[1] + s64{mx[row][2]} * vec[2];
            set_mac_and_ir(row + 1, value, modifiers);
        }
        return;
    }

    Vector tr{};
    if (which_translation != TranslationKind::None) {
        tr = translation[static_cast<u32>(which_translation)];
    }
    transform(mx, tr, vec, modifiers);
}

u32 Gte::execute(u32 instruction)
{
    const u32 operation = instruction & 0x3F;
    const Modifiers modifiers = {
        ((instruction >> 19) & 1) != 0 ? FRACTION_BITS : 0,
        ((instruction >> 10) & 1) != 0,
    };

    // FLAG reports what this one command did, so it starts clean.
    flag = 0;

    switch (operation) {
    case RTPS:
        project(0, modifiers);
        break;

    case RTPT:
        for (u32 vertex = 0; vertex < 3; vertex++) {
            project(vertex, modifiers);
        }
        break;

    case NCLIP:
        // Twice the signed area of the projected triangle. Software
        // uses its sign to drop back-facing polygons, which is why it
        // is a whole instruction.
        set_mac0(s64{sxy[0][0]} * sxy[1][1] + s64{sxy[1][0]} * sxy[2][1] +
                 s64{sxy[2][0]} * sxy[0][1] - s64{sxy[0][0]} * sxy[2][1] -
                 s64{sxy[1][0]} * sxy[0][1] - s64{sxy[2][0]} * sxy[1][1]);
        break;

    case OP: {
        // The cross product of IR1..3 with the rotation matrix's
        // diagonal, which is where a surface normal comes from.
        const s64 d1 = matrix[0][0][0];
        const s64 d2 = matrix[0][1][1];
        const s64 d3 = matrix[0][2][2];
        const std::array<s16, 3> in = {ir[1], ir[2], ir[3]};
        set_mac_and_ir(1, in[2] * d2 - in[1] * d3, modifiers);
        set_mac_and_ir(2, in[0] * d3 - in[2] * d1, modifiers);
        set_mac_and_ir(3, in[1] * d1 - in[0] * d2, modifiers);
        break;
    }

    case SQR:
        for (u32 i = 0; i < 3; i++) {
            set_mac_and_ir(i + 1, s64{ir[i + 1]} * ir[i + 1], modifiers);
        }
        break;

    case MVMVA:
        run_mvmva(instruction, modifiers);
        break;

    case AVSZ3: {
        const s64 sum = s64{sz[1]} + sz[2] + sz[3];
        const s32 average = set_mac0(s64{zsf3} * sum) >> FRACTION_BITS;
        if (average < 0 || average > DEPTH_MAX) {
            flag |= FLAG_DEPTH_SATURATED;
        }
        otz = static_cast<u16>(std::clamp(average, 0, DEPTH_MAX));
        break;
    }

    case AVSZ4: {
        const s64 sum = s64{sz[0]} + sz[1] + sz[2] + sz[3];
        const s32 average = set_mac0(s64{zsf4} * sum) >> FRACTION_BITS;
        if (average < 0 || average > DEPTH_MAX) {
            flag |= FLAG_DEPTH_SATURATED;
        }
        otz = static_cast<u16>(std::clamp(average, 0, DEPTH_MAX));
        break;
    }

    case DPCS:
        for (u32 i = 0; i < 3; i++) {
            set_mac(i + 1, s64{rgbc[i]} << 16, 0);
        }
        interpolate(modifiers);
        push_colour();
        break;

    case DPCT:
        // Three times over the colour FIFO's oldest entry, which each
        // push moves along — so this fogs the three colours already
        // queued rather than the same one three times.
        for (u32 pass = 0; pass < 3; pass++) {
            for (u32 i = 0; i < 3; i++) {
                set_mac(i + 1, s64{rgb[0][i]} << 16, 0);
            }
            interpolate(modifiers);
            push_colour();
        }
        break;

    case INTPL:
        for (u32 i = 0; i < 3; i++) {
            set_mac(i + 1, s64{ir[i + 1]} << FRACTION_BITS, 0);
        }
        interpolate(modifiers);
        push_colour();
        break;

    case DCPL:
        shade(modifiers, false);
        interpolate(modifiers);
        push_colour();
        break;

    case CDP:
        transform(matrix[2], translation[1], {ir[1], ir[2], ir[3]}, modifiers);
        shade(modifiers, false);
        interpolate(modifiers);
        push_colour();
        break;

    case CC:
        transform(matrix[2], translation[1], {ir[1], ir[2], ir[3]}, modifiers);
        shade(modifiers, true);
        push_colour();
        break;

    case NCS:
        light(0, modifiers);
        push_colour();
        break;

    case NCT:
        for (u32 vertex = 0; vertex < 3; vertex++) {
            light(vertex, modifiers);
            push_colour();
        }
        break;

    case NCDS:
        light(0, modifiers);
        shade(modifiers, false);
        interpolate(modifiers);
        push_colour();
        break;

    case NCDT:
        for (u32 vertex = 0; vertex < 3; vertex++) {
            light(vertex, modifiers);
            shade(modifiers, false);
            interpolate(modifiers);
            push_colour();
        }
        break;

    case NCCS:
        light(0, modifiers);
        shade(modifiers, true);
        push_colour();
        break;

    case NCCT:
        for (u32 vertex = 0; vertex < 3; vertex++) {
            light(vertex, modifiers);
            shade(modifiers, true);
            push_colour();
        }
        break;

    case GPF:
        for (u32 i = 0; i < 3; i++) {
            set_mac_and_ir(i + 1, s64{ir[i + 1]} * ir[0], modifiers);
        }
        push_colour();
        break;

    case GPL:
        for (u32 i = 0; i < 3; i++) {
            const s64 accumulated = s64{mac[i + 1]} << modifiers.shift;
            set_mac_and_ir(
                i + 1, accumulated + s64{ir[i + 1]} * ir[0], modifiers);
        }
        push_colour();
        break;

    default:
        // The engine has no way to refuse an operation it does not
        // have; an unassigned code runs whatever the microcode happens
        // to do. Leaving the registers alone is the least wrong answer
        // available and keeps the emulator running.
        break;
    }

    if ((flag & FLAG_ERROR_MASK) != 0) {
        flag |= FLAG_ERROR;
    }
    return operation_cycles(operation);
}

u32 Gte::read_data(u32 index) const
{
    switch (index) {
    case 0:
        return pack(v[0][0], v[0][1]);
    case 1:
        return sign_extend(v[0][2]);
    case 2:
        return pack(v[1][0], v[1][1]);
    case 3:
        return sign_extend(v[1][2]);
    case 4:
        return pack(v[2][0], v[2][1]);
    case 5:
        return sign_extend(v[2][2]);
    case 6:
        return rgbc[0] | (u32{rgbc[1]} << 8) | (u32{rgbc[2]} << 16) |
            (u32{rgbc[3]} << 24);
    case 7:
        return otz;
    case 8:
    case 9:
    case 10:
    case 11:
        return sign_extend(ir[index - 8]);
    case 12:
    case 13:
    case 14:
        return pack(sxy[index - 12][0], sxy[index - 12][1]);
    case 15:
        // Reading the push register gives the newest entry; only
        // writing it does anything unusual.
        return pack(sxy[2][0], sxy[2][1]);
    case 16:
    case 17:
    case 18:
    case 19:
        return sz[index - 16];
    case 20:
    case 21:
    case 22: {
        const std::array<u8, 4>& entry = rgb[index - 20];
        return entry[0] | (u32{entry[1]} << 8) | (u32{entry[2]} << 16) |
            (u32{entry[3]} << 24);
    }
    case 23:
        return res1;
    case 24:
    case 25:
    case 26:
    case 27:
        return static_cast<u32>(mac[index - 24]);
    case 28:
    case 29:
        return to_irgb(ir);
    case 30:
        return lzcs;
    default:
        return static_cast<u32>(lzcr);
    }
}

void Gte::write_data(u32 index, u32 value)
{
    switch (index) {
    case 0:
        v[0][0] = low_half(value);
        v[0][1] = high_half(value);
        break;
    case 1:
        v[0][2] = low_half(value);
        break;
    case 2:
        v[1][0] = low_half(value);
        v[1][1] = high_half(value);
        break;
    case 3:
        v[1][2] = low_half(value);
        break;
    case 4:
        v[2][0] = low_half(value);
        v[2][1] = high_half(value);
        break;
    case 5:
        v[2][2] = low_half(value);
        break;
    case 6:
        rgbc = {static_cast<u8>(value),
                static_cast<u8>(value >> 8),
                static_cast<u8>(value >> 16),
                static_cast<u8>(value >> 24)};
        break;
    case 7:
        otz = static_cast<u16>(value);
        break;
    case 8:
    case 9:
    case 10:
    case 11:
        ir[index - 8] = low_half(value);
        break;
    case 12:
    case 13:
    case 14:
        sxy[index - 12] = {low_half(value), high_half(value)};
        break;
    case 15:
        // Writing SXYP pushes rather than assigns, which is how a
        // vertex computed on the CPU joins ones the engine projected.
        sxy[0] = sxy[1];
        sxy[1] = sxy[2];
        sxy[2] = {low_half(value), high_half(value)};
        break;
    case 16:
    case 17:
    case 18:
    case 19:
        sz[index - 16] = static_cast<u16>(value);
        break;
    case 20:
    case 21:
    case 22:
        rgb[index - 20] = {static_cast<u8>(value),
                           static_cast<u8>(value >> 8),
                           static_cast<u8>(value >> 16),
                           static_cast<u8>(value >> 24)};
        break;
    case 23:
        res1 = value;
        break;
    case 24:
    case 25:
    case 26:
    case 27:
        mac[index - 24] = static_cast<s32>(value);
        break;
    case 28:
        // The 15-bit colour form, expanded back into the working
        // vector five bits at a time.
        ir[1] = static_cast<s16>((value & 0x1F) * IRGB_SCALE);
        ir[2] = static_cast<s16>(((value >> 5) & 0x1F) * IRGB_SCALE);
        ir[3] = static_cast<s16>(((value >> 10) & 0x1F) * IRGB_SCALE);
        break;
    case 30:
        lzcs = value;
        lzcr = count_leading_sign_bits(value);
        break;
    default:
        // 29 (ORGB) and 31 (LZCR) are outputs; writing them does
        // nothing at all.
        break;
    }
}

u32 Gte::read_control(u32 index) const
{
    if (index < 37) {
        return read_matrix(matrix[0], index - 32);
    }
    if (index < 40) {
        return static_cast<u32>(translation[0][index - 37]);
    }
    if (index < 45) {
        return read_matrix(matrix[1], index - 40);
    }
    if (index < 48) {
        return static_cast<u32>(translation[1][index - 45]);
    }
    if (index < 53) {
        return read_matrix(matrix[2], index - 48);
    }
    if (index < 56) {
        return static_cast<u32>(translation[2][index - 53]);
    }

    switch (index) {
    case 56:
        return static_cast<u32>(ofx);
    case 57:
        return static_cast<u32>(ofy);
    case 58:
        // H is unsigned everywhere it is used and sign-extended when
        // it is read back, which is the hardware's inconsistency
        // rather than this emulator's: a projection distance over
        // 8000h reads as negative.
        return sign_extend(static_cast<s16>(h));
    case 59:
        return sign_extend(dqa);
    case 60:
        return static_cast<u32>(dqb);
    case 61:
        return sign_extend(zsf3);
    case 62:
        return sign_extend(zsf4);
    default:
        return flag;
    }
}

void Gte::write_control(u32 index, u32 value)
{
    if (index < 37) {
        write_matrix(matrix[0], index - 32, value);
        return;
    }
    if (index < 40) {
        translation[0][index - 37] = static_cast<s32>(value);
        return;
    }
    if (index < 45) {
        write_matrix(matrix[1], index - 40, value);
        return;
    }
    if (index < 48) {
        translation[1][index - 45] = static_cast<s32>(value);
        return;
    }
    if (index < 53) {
        write_matrix(matrix[2], index - 48, value);
        return;
    }
    if (index < 56) {
        translation[2][index - 53] = static_cast<s32>(value);
        return;
    }

    switch (index) {
    case 56:
        ofx = static_cast<s32>(value);
        break;
    case 57:
        ofy = static_cast<s32>(value);
        break;
    case 58:
        h = static_cast<u16>(value);
        break;
    case 59:
        dqa = low_half(value);
        break;
    case 60:
        dqb = static_cast<s32>(value);
        break;
    case 61:
        zsf3 = low_half(value);
        break;
    case 62:
        zsf4 = low_half(value);
        break;
    default:
        flag = value & FLAG_WRITABLE;
        if ((flag & FLAG_ERROR_MASK) != 0) {
            flag |= FLAG_ERROR;
        }
        break;
    }
}
