#pragma once

#include <array>

#include "types.h"

struct State;

// The Geometry Transformation Engine: coprocessor 2, and the reason a
// 33 MHz machine with no floating point can draw 3D at all. It is
// fixed-point vector arithmetic in hardware — rotate a vertex, project
// it onto the screen, shade it by a light — each operation a single
// instruction that would otherwise be dozens.
//
// It is reached exactly as any MIPS coprocessor is: MFC2/MTC2 move
// values in and out of its data registers, CFC2/CTC2 do the same for
// its control registers, and an instruction with bit 25 set is an
// operation rather than a move. Software loads a vertex, issues RTPS,
// and reads a screen coordinate back.
//
// Two things make it awkward, and both are deliberate rather than
// accidental:
//
//   Everything saturates. There are no exceptions and no traps; a
//   result too large for its register is clamped and a bit is set in
//   FLAG to say so. Software checks FLAG once per polygon rather than
//   once per operation, so the flags have to be right even when the
//   values are not being looked at.
//
//   The intermediate accumulators are 44 bits wide while the registers
//   they land in are 32 or 16. Where a command shifts its result down
//   by 12 — the fixed-point scale — is part of the command's encoding,
//   and getting it wrong moves geometry rather than breaking it, which
//   is the hardest kind of mistake to find.
//
// The division RTPS does for perspective is not a division either: it
// is a reciprocal table and two Newton-Raphson steps, reproduced here
// because software can see the last bit of its result.
struct Gte {
    void reset();

    void visit_state(State& state);

    // The 32 data and 32 control registers, as the move instructions
    // see them. Several are not stored anywhere: IRGB is computed from
    // IR1..3 on the way out and taken apart on the way in, and LZCR is
    // a count of the bits in LZCS.
    u32 read_data(u32 index) const;
    u32 read_control(u32 index) const;
    void write_data(u32 index, u32 value);
    void write_control(u32 index, u32 value);

    // Runs one geometry operation and returns what it cost in cycles.
    // The engine runs alongside the CPU on hardware and only stalls it
    // when a result is read too soon; charging the cost to the
    // instruction that started it is the same total and needs no
    // interlock.
    u32 execute(u32 instruction);

    // A 3x3 matrix of 1.3.12 fixed-point values, and a translation in
    // whole units. There are three of each: rotation, light direction
    // and light colour, paired with translation, background colour and
    // far colour.
    using Matrix = std::array<std::array<s16, 3>, 3>;
    using Vector = std::array<s32, 3>;

    // Which matrix and which translation an operation uses. Every
    // command but MVMVA has them fixed; MVMVA takes them from its
    // encoding, including the two combinations the hardware gets
    // wrong.
    enum class MatrixKind : u32 {
        Rotation = 0,
        Light = 1,
        Colour = 2,
        Garbage = 3,
    };
    enum class TranslationKind : u32 {
        Translation = 0,
        Background = 1,
        FarColour = 2,
        None = 3,
    };

    // The three input vertices, each x, y, z.
    std::array<std::array<s16, 3>, 3> v{};

    // The colour and the "code" byte that travels with it, which is
    // the GP0 command the polygon will be drawn with.
    std::array<u8, 4> rgbc{};

    u16 otz = 0;

    // IR0 is the interpolation factor; IR1..3 are the working vector.
    std::array<s16, 4> ir{};

    // The three-deep FIFOs a projected vertex lands in. Software reads
    // a whole triangle out of them after transforming its corners,
    // which is why they are queues rather than registers.
    std::array<std::array<s16, 2>, 3> sxy{};
    std::array<u16, 4> sz{};
    std::array<std::array<u8, 4>, 3> rgb{};

    // The accumulators. MAC0 is the scalar one; MAC1..3 the vector.
    std::array<s32, 4> mac{};

    u32 lzcs = 0;
    s32 lzcr = 32;

    // cop2r23, which the hardware has and nothing uses. Software still
    // saves and restores it across a context switch, so it has to read
    // back what was written.
    u32 res1 = 0;

    std::array<Matrix, 3> matrix{};
    std::array<Vector, 3> translation{};

    // Where the screen's origin sits, and the projection plane
    // distance — the focal length, in effect.
    s32 ofx = 0;
    s32 ofy = 0;
    u16 h = 0;

    // Depth cueing: how far into the fog a vertex is, as a linear
    // function of its projected depth.
    s16 dqa = 0;
    s32 dqb = 0;

    // The weights AVSZ3 and AVSZ4 average a face's depths with, chosen
    // so the result indexes an ordering table directly.
    s16 zsf3 = 0;
    s16 zsf4 = 0;

    u32 flag = 0;

private:
    // What a command's encoding says about how its results are
    // treated: how far to shift them down, and whether IR1..3 clamp to
    // zero at the bottom or to -8000h.
    struct Modifiers {
        u32 shift = 0;
        bool clamp_positive = false;
    };

    // Stores a 44-bit accumulator result, flagging the overflow if it
    // did not fit, and shifting it down to the 32 bits MAC holds.
    void set_mac(u32 index, s64 value, u32 shift);
    s32 set_mac0(s64 value);

    // Clamps into IR, flagging if it had to.
    void set_ir(u32 index, s32 value, bool clamp_positive);
    void set_mac_and_ir(u32 index, s64 value, const Modifiers& modifiers);

    // The three FIFOs, each pushed from the accumulators.
    void push_screen(s32 x, s32 y);
    void push_depth(s64 value);
    void push_colour();

    // MAC1..3 = translation*1000h + matrix * vector, the step at the
    // bottom of nearly every command. The vector is taken by value
    // because MVMVA can be asked to use IR1..3 as its input, which the
    // first row would otherwise overwrite before the second read it.
    void transform(const Matrix& mx,
                   const Vector& tr,
                   std::array<s16, 3> vec,
                   const Modifiers& modifiers);

    // MAC1..3 = the vertex colour scaled by the light in IR1..3.
    void shade(const Modifiers& modifiers, bool clamp);

    // Pulls MAC1..3 towards the far colour by IR0, which is what makes
    // distance fade to fog.
    void interpolate(const Modifiers& modifiers);

    // One vertex through the full perspective transform.
    void project(u32 vertex, const Modifiers& modifiers);

    // The reciprocal of SZ3, as the hardware computes it: H*20000h/SZ3
    // rounded, limited to 1FFFFh, with FLAG's divide bit set if it hit
    // the limit.
    u32 perspective_divide();

    // The light-and-colour pipeline the "normal colour" family shares.
    void light(u32 vertex, const Modifiers& modifiers);

    void run_mvmva(u32 instruction, const Modifiers& modifiers);
};
