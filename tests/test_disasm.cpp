#include <doctest/doctest.h>

#include "disasm.h"
#include "machine.h"

using namespace mips;

namespace {

// Somewhere to disassemble from. Only the branch and jump forms care
// where they were read, but passing it everywhere keeps the tests
// reading the same.
constexpr u32 AT = 0x80001000;

}  // namespace

// The mini-assembler the CPU tests are written in gives the
// disassembler something to be checked against: what one encodes, the
// other has to name.
TEST_CASE("the assembler's own encodings disassemble back")
{
    CHECK(disassemble(nop(), AT) == "nop");
    CHECK(disassemble(addiu(t0, zero, 1), AT) == "addiu   $t0, $zero, 0x1");
    CHECK(disassemble(ori(t0, t1, 0x1234), AT) == "ori     $t0, $t1, 0x1234");
    CHECK(disassemble(lui(t0, 0xBFC0), AT) == "lui     $t0, 0xBFC0");
    CHECK(disassemble(lw(t0, t1, 8), AT) == "lw      $t0, 0x8($t1)");
    CHECK(disassemble(sw(t0, t1, 8), AT) == "sw      $t0, 0x8($t1)");
    CHECK(disassemble(addu(t2, t0, t1), AT) == "addu    $t2, $t0, $t1");
    CHECK(disassemble(add(t2, t0, t1), AT) == "add     $t2, $t0, $t1");
    CHECK(disassemble(syscall_op(), AT) == "syscall");
    CHECK(disassemble(rfe(), AT) == "rfe");
    CHECK(disassemble(mfc0(t0, 12), AT) == "mfc0    $t0, cop0_r12");
    CHECK(disassemble(lwl(t0, t1, 3), AT) == "lwl     $t0, 0x3($t1)");
    CHECK(disassemble(lwr(t0, t1, 0), AT) == "lwr     $t0, 0x0($t1)");
    CHECK(disassemble(swl(t0, t1, 3), AT) == "swl     $t0, 0x3($t1)");
    CHECK(disassemble(swr(t0, t1, 0), AT) == "swr     $t0, 0x0($t1)");
}

TEST_CASE("a branch shows where it lands, not how far it goes")
{
    // The offset counts instructions from the delay slot, which is the
    // arithmetic a reader should never have to do.
    CHECK(disassemble(beq(zero, zero, 1), AT) ==
          "beq     $zero, $zero, 0x80001008");
    CHECK(disassemble(beq(t0, t1, -3), AT) == "beq     $t0, $t1, 0x80000FF8");
}

TEST_CASE("a jump keeps the region it was read from")
{
    // A jump supplies only the low 28 bits, so where it was read from
    // decides the rest — it cannot leave its 256 MB region.
    CHECK(disassemble(jal(0x00001000), 0xBFC00000) == "jal     0xB0001000");
}

TEST_CASE("a negative offset reads as one")
{
    CHECK(disassemble(lw(ra, 29, -4), AT) == "lw      $ra, -0x4($sp)");
    CHECK(disassemble(addiu(29, 29, -8), AT) == "addiu   $sp, $sp, -0x8");
}

TEST_CASE("an all-zero word is a nop, but a real shift is not")
{
    CHECK(disassemble(0x00000000, AT) == "nop");
    // sll $t0, $t1, 4 shares the encoding but is not a nop.
    CHECK(disassemble(r_type(0, t1, t0, 4, 0x00), AT) == "sll     $t0, $t1, 4");
}

TEST_CASE("instructions the emulator lacks still disassemble")
{
    // The disassembler is for reading the machine, not for describing
    // this emulator, so it names what the hardware defines.
    CHECK(disassemble(r_type(t0, t1, t2, 0, 0x22), AT) ==
          "sub     $t2, $t0, $t1");
    CHECK(disassemble(r_type(0, 0, 0, 0, 0x0D), AT) == "break");
    CHECK(disassemble(r_type(t0, t1, 0, 0, 0x1A), AT) == "div     $t0, $t1");
    CHECK(disassemble(i_type(0x01, t0, 0x01, 2), AT) ==
          "bgez    $t0, 0x8000100C");
}

TEST_CASE("a word that decodes to nothing says so rather than guessing")
{
    // funct 0x3F is not an instruction.
    CHECK(disassemble(r_type(0, 0, 0, 0, 0x3F), AT) == ".word   0x0000003F");
    // Primary opcode 0x1D is unassigned.
    CHECK(disassemble(0x74000000, AT) == ".word   0x74000000");
}
