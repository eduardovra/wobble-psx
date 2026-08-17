#include <doctest/doctest.h>

#include "gte.h"
#include "machine.h"

namespace {

// A geometry operation, as the instruction word encodes it. Bit 25 is
// what makes it an operation rather than a register move; sf and lm are
// the two modifiers every command reads.
constexpr u32 COP2_OPERATION = 0x4A000000;
constexpr u32 SHIFT_RESULT = 1 << 19;
constexpr u32 CLAMP_POSITIVE = 1 << 10;

constexpr u32 command(u32 operation) { return COP2_OPERATION | operation; }

// One in 1.3.12 fixed point, which is what a matrix entry of "no
// rotation and no scale" looks like.
constexpr s16 ONE = 0x1000;

// A matrix that leaves a vertex where it is, so a test can be about
// one thing at a time.
void set_identity(Gte::Matrix& matrix)
{
    matrix = {{{ONE, 0, 0}, {0, ONE, 0}, {0, 0, ONE}}};
}

}  // namespace

TEST_CASE("a vertex is projected by dividing through by its depth")
{
    Gte gte;
    set_identity(gte.matrix[0]);

    // A projection distance of 100h with the vertex at twice that
    // depth: everything ends up at half scale, and the division comes
    // out exact, so the expected values can be reasoned about rather
    // than copied from a run.
    gte.h = 0x100;
    gte.v[0] = {100, 200, 0x200};

    gte.execute(command(0x01) | SHIFT_RESULT);  // RTPS

    CHECK(gte.ir[1] == 100);
    CHECK(gte.ir[2] == 200);
    CHECK(gte.ir[3] == 0x200);
    CHECK(gte.sz[3] == 0x200);
    CHECK(gte.sxy[2][0] == 50);
    CHECK(gte.sxy[2][1] == 100);
    CHECK(gte.flag == 0);
}

TEST_CASE("the screen offset moves the projection rather than the vertex")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.h = 0x100;
    gte.v[0] = {100, 200, 0x200};

    // The offsets are 16.16, so a whole pixel is 10000h.
    gte.ofx = 320 << 16;
    gte.ofy = 240 << 16;

    gte.execute(command(0x01) | SHIFT_RESULT);

    CHECK(gte.sxy[2][0] == 370);
    CHECK(gte.sxy[2][1] == 340);
}

TEST_CASE("depth cueing is linear in the projected depth")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.h = 0x100;
    gte.v[0] = {0, 0, 0x200};
    gte.dqa = 2;
    gte.dqb = 0x1000;

    gte.execute(command(0x01) | SHIFT_RESULT);

    // The divide gives 8000h, so MAC0 is 8000h*2 + 1000h and IR0 is
    // that over 1000h.
    CHECK(gte.mac[0] == 0x11000);
    CHECK(gte.ir[0] == 0x11);
}

TEST_CASE("a vertex at zero depth flags the division rather than trapping")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.h = 0x100;
    gte.v[0] = {10, 10, 0};

    gte.execute(command(0x01) | SHIFT_RESULT);

    CHECK(gte.sz[3] == 0);
    CHECK((gte.flag & (1u << 17)) != 0);  // divide overflow
    CHECK((gte.flag & (1u << 31)) != 0);  // and so the error bit
}

TEST_CASE("three vertices are projected in one instruction")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.h = 0x100;
    gte.v[0] = {100, 0, 0x200};
    gte.v[1] = {0, 100, 0x200};
    gte.v[2] = {-100, 0, 0x200};

    gte.execute(command(0x30) | SHIFT_RESULT);  // RTPT

    CHECK(gte.sxy[0][0] == 50);
    CHECK(gte.sxy[0][1] == 0);
    CHECK(gte.sxy[1][0] == 0);
    CHECK(gte.sxy[1][1] == 50);
    CHECK(gte.sxy[2][0] == -50);
}

TEST_CASE("the translation vector is added at full scale")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.translation[0] = {7, -3, 0x200};
    gte.h = 0x100;
    gte.v[0] = {100, 200, 0};

    gte.execute(command(0x01) | SHIFT_RESULT);

    CHECK(gte.ir[1] == 107);
    CHECK(gte.ir[2] == 197);
    CHECK(gte.sz[3] == 0x200);
}

TEST_CASE("NCLIP gives the signed area a back-face test needs")
{
    Gte gte;
    gte.sxy[0] = {0, 0};
    gte.sxy[1] = {10, 0};
    gte.sxy[2] = {0, 10};

    gte.execute(command(0x06));
    CHECK(gte.mac[0] == 100);

    // The same triangle wound the other way is the same area negated,
    // which is the whole point of the instruction.
    gte.sxy[1] = {0, 10};
    gte.sxy[2] = {10, 0};
    gte.execute(command(0x06));
    CHECK(gte.mac[0] == -100);
}

TEST_CASE("AVSZ3 averages three depths into an ordering table index")
{
    Gte gte;
    gte.zsf3 = 0x555;  // a third, in 1.3.12
    gte.sz[1] = 300;
    gte.sz[2] = 300;
    gte.sz[3] = 300;

    gte.execute(command(0x2D));

    CHECK(gte.mac[0] == 0x555 * 900);
    CHECK(gte.otz == 299);
}

TEST_CASE("AVSZ4 takes the fourth depth in as well")
{
    Gte gte;
    gte.zsf4 = 0x400;  // a quarter
    gte.sz[0] = 100;
    gte.sz[1] = 200;
    gte.sz[2] = 300;
    gte.sz[3] = 400;

    gte.execute(command(0x2E));

    CHECK(gte.otz == 250);
}

TEST_CASE("SQR squares the working vector")
{
    Gte gte;
    gte.ir[1] = 3;
    gte.ir[2] = -4;
    gte.ir[3] = 5;

    gte.execute(command(0x28));

    CHECK(gte.mac[1] == 9);
    CHECK(gte.mac[2] == 16);
    CHECK(gte.mac[3] == 25);
    CHECK(gte.ir[1] == 9);
}

TEST_CASE("MVMVA picks its matrix, vector and translation from the "
          "instruction")
{
    Gte gte;
    set_identity(gte.matrix[1]);     // the light matrix
    gte.translation[1] = {1, 2, 3};  // the background colour
    gte.v[1] = {10, 20, 30};

    // Matrix 1, vector 1, translation 1.
    const u32 word =
        command(0x12) | SHIFT_RESULT | (1 << 17) | (1 << 15) | (1 << 13);
    gte.execute(word);

    CHECK(gte.mac[1] == 11);
    CHECK(gte.mac[2] == 22);
    CHECK(gte.mac[3] == 33);
}

TEST_CASE("MVMVA with no translation leaves the product alone")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.translation[0] = {1000, 1000, 1000};
    gte.v[0] = {10, 20, 30};

    const u32 word = command(0x12) | SHIFT_RESULT | (3 << 13);
    gte.execute(word);

    CHECK(gte.mac[1] == 10);
    CHECK(gte.mac[2] == 20);
    CHECK(gte.mac[3] == 30);
}

TEST_CASE("a result too large for IR is clamped and flagged")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.v[0] = {0x7FFF, 0, 0};
    gte.translation[0] = {0x7FFF, 0, 0};

    gte.execute(command(0x01) | SHIFT_RESULT);

    CHECK(gte.mac[1] == 0x7FFF + 0x7FFF);
    CHECK(gte.ir[1] == 0x7FFF);
    CHECK((gte.flag & (1u << 24)) != 0);  // IR1 saturated
    CHECK((gte.flag & (1u << 31)) != 0);
}

TEST_CASE("lm clamps the working vector at zero instead of at -8000h")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.v[0] = {-100, 0, 0};

    gte.execute(command(0x01) | SHIFT_RESULT);
    CHECK(gte.ir[1] == -100);

    gte.execute(command(0x01) | SHIFT_RESULT | CLAMP_POSITIVE);
    CHECK(gte.ir[1] == 0);
}

TEST_CASE("FLAG reports one command at a time")
{
    Gte gte;
    set_identity(gte.matrix[0]);
    gte.h = 0x100;
    gte.v[0] = {0, 0, 0};

    gte.execute(command(0x01) | SHIFT_RESULT);
    CHECK(gte.flag != 0);

    gte.v[0] = {0, 0, 0x200};
    gte.execute(command(0x01) | SHIFT_RESULT);
    CHECK(gte.flag == 0);
}

TEST_CASE("the colour FIFO is pushed rather than assigned")
{
    Gte gte;
    set_identity(gte.matrix[1]);
    set_identity(gte.matrix[2]);
    gte.rgbc = {0x10, 0x20, 0x30, 0x2C};
    gte.v[0] = {0x100, 0x100, 0x100};

    gte.execute(command(0x1D) | SHIFT_RESULT);  // NCS
    const std::array<u8, 4> first = gte.rgb[2];

    gte.v[0] = {0x080, 0x080, 0x080};
    gte.execute(command(0x1D) | SHIFT_RESULT);

    CHECK(gte.rgb[1] == first);
    CHECK(gte.rgb[2][3] == 0x2C);  // the GP0 command byte travels along
    CHECK(gte.rgb[2] != first);
}

TEST_CASE("writing SXYP pushes the screen FIFO")
{
    Gte gte;
    gte.write_data(12, 0x00010002);
    gte.write_data(13, 0x00030004);
    gte.write_data(14, 0x00050006);
    gte.write_data(15, 0x00070008);

    CHECK(gte.read_data(12) == 0x00030004);
    CHECK(gte.read_data(13) == 0x00050006);
    CHECK(gte.read_data(14) == 0x00070008);
    CHECK(gte.read_data(15) == 0x00070008);
}

TEST_CASE("LZCR counts the bits that match the sign")
{
    Gte gte;

    gte.write_data(30, 0x00000000);
    CHECK(gte.read_data(31) == 32);

    gte.write_data(30, 0xFFFFFFFF);
    CHECK(gte.read_data(31) == 32);

    gte.write_data(30, 0x00FFFFFF);
    CHECK(gte.read_data(31) == 8);

    gte.write_data(30, 0xFF000000);
    CHECK(gte.read_data(31) == 8);
}

TEST_CASE("IRGB is the working vector seen as a 15-bit colour")
{
    Gte gte;
    gte.write_data(28, 0x7FFF);  // all three at maximum

    CHECK(gte.ir[1] == 0x1F * 0x80);
    CHECK(gte.ir[2] == 0x1F * 0x80);
    CHECK(gte.ir[3] == 0x1F * 0x80);
    CHECK(gte.read_data(28) == 0x7FFF);
    CHECK(gte.read_data(29) == 0x7FFF);
}

TEST_CASE("the control registers read back what was written")
{
    Gte gte;

    gte.write_control(32, 0x1111F222);
    CHECK(gte.matrix[0][0][0] == static_cast<s16>(0xF222));
    CHECK(gte.matrix[0][0][1] == 0x1111);
    CHECK(gte.read_control(32) == 0x1111F222);

    // The three that hold a single 16-bit value come back sign
    // extended, which is what software saving the register file sees.
    gte.write_control(36, 0x0000FFFF);
    CHECK(gte.read_control(36) == 0xFFFFFFFF);

    gte.write_control(56, 0x12345678);
    CHECK(gte.read_control(56) == 0x12345678);
}

TEST_CASE("the CPU moves values in and out of the geometry engine")
{
    using namespace mips;

    Machine machine;
    // MTC2 $t0, cop2d0 then MFC2 $t1, cop2d0, with the nop the load
    // delay slot needs before $t1 can be read.
    constexpr u32 MTC2 = 0x48800000 | (t0 << 16) | (0 << 11);
    constexpr u32 MFC2 = 0x48000000 | (t1 << 16) | (0 << 11);
    machine.load({
        lui(t0, 0x0002),
        ori(t0, t0, 0x0001),
        MTC2,
        MFC2,
        nop(),
    });
    machine.run(5);

    CHECK(machine.cpu.gte.v[0][0] == 1);
    CHECK(machine.cpu.gte.v[0][1] == 2);
    CHECK(machine.reg(t1) == 0x00020001);
    CHECK_FALSE(machine.cpu.halted);
}

TEST_CASE("the CPU names the control registers from the start of their file")
{
    using namespace mips;

    Machine machine;
    // CTC2 $t0, cop2c0 then CFC2 $t1, cop2c0. Control register 0 is
    // cop2r32, the first word of the rotation matrix.
    constexpr u32 CTC2 = 0x48C00000 | (t0 << 16) | (0 << 11);
    constexpr u32 CFC2 = 0x48400000 | (t1 << 16) | (0 << 11);
    machine.load({
        lui(t0, 0x0002),
        ori(t0, t0, 0x0001),
        CTC2,
        CFC2,
        nop(),
    });
    machine.run(5);

    CHECK(machine.cpu.gte.matrix[0][0][0] == 1);
    CHECK(machine.cpu.gte.matrix[0][0][1] == 2);
    CHECK(machine.reg(t1) == 0x00020001);
    CHECK_FALSE(machine.cpu.halted);
}

TEST_CASE("a geometry operation runs from an ordinary instruction")
{
    Machine machine;
    machine.cpu.gte.sxy[0] = {0, 0};
    machine.cpu.gte.sxy[1] = {10, 0};
    machine.cpu.gte.sxy[2] = {0, 10};

    machine.load({command(0x06)});  // NCLIP
    machine.run(1);

    CHECK(machine.cpu.gte.mac[0] == 100);
    CHECK_FALSE(machine.cpu.halted);
}
