#include <doctest/doctest.h>

#include "machine.h"

using namespace mips;

TEST_CASE("a load's value is not visible to the next instruction")
{
    Machine m;
    m.bus->write32(Machine::DATA, 0xDEADBEEF);
    m.load({
        addiu(t1, zero, Machine::DATA),
        lw(t0, t1, 0),
        addu(t2, t0, zero),  // load delay slot: still sees the old t0
        addu(t3, t0, zero),  // the loaded value has landed by now
    });
    m.run(4);

    CHECK(m.reg(t2) == 0);
    CHECK(m.reg(t3) == 0xDEADBEEF);
}

TEST_CASE("a second load into one register cancels the first")
{
    Machine m;
    m.bus->write32(Machine::DATA, 0x11111111);
    m.bus->write32(Machine::DATA + 4, 0x22222222);
    m.load({
        addiu(t1, zero, Machine::DATA),
        lw(t0, t1, 0),
        lw(t0, t1, 4),
        addu(t2, t0, zero),  // the first load is never visible here
        addu(t3, t0, zero),  // and the second arrives only now
    });
    m.run(5);

    CHECK(m.reg(t2) == 0);
    CHECK(m.reg(t3) == 0x22222222);
}

TEST_CASE("a write in the load delay slot beats the load")
{
    Machine m;
    m.bus->write32(Machine::DATA, 0xDEADBEEF);
    m.load({
        addiu(t1, zero, Machine::DATA),
        lw(t0, t1, 0),
        addiu(t0, zero, 5),  // same register, one instruction later
        addu(t2, t0, zero),
    });
    m.run(4);

    CHECK(m.reg(t0) == 5);
    CHECK(m.reg(t2) == 5);
}

// The four unaligned instructions. Memory is laid out so the bytes
// are their own labels: DATA holds 11 22 33 44 55 66 77 88, and the
// word read from DATA+n is the four bytes starting there.
namespace {

constexpr u32 FIRST_WORD = 0x44332211;
constexpr u32 SECOND_WORD = 0x88776655;

void fill_bytes(Machine& m)
{
    m.bus->write32(Machine::DATA, FIRST_WORD);
    m.bus->write32(Machine::DATA + 4, SECOND_WORD);
}

}  // namespace

TEST_CASE("an LWL/LWR pair loads a word from any alignment")
{
    Machine m;
    fill_bytes(m);

    s32 offset = 0;
    u32 expected = 0;
    SUBCASE("aligned")
    {
        offset = 0;
        expected = 0x44332211;
    }
    SUBCASE("one byte in")
    {
        offset = 1;
        expected = 0x55443322;
    }
    SUBCASE("two bytes in")
    {
        offset = 2;
        expected = 0x66554433;
    }
    SUBCASE("three bytes in")
    {
        offset = 3;
        expected = 0x77665544;
    }

    m.load({
        addiu(t1, zero, Machine::DATA),
        addiu(t0, zero, -1),  // prefilled, so nothing stale may show
        lwl(t0, t1, offset + 3),
        lwr(t0, t1, offset),
        nop(),  // the pair's result lands here
    });
    m.run(5);

    CHECK(m.reg(t0) == expected);
}

TEST_CASE("the halves of an unaligned load need no nop between them")
{
    Machine m;
    fill_bytes(m);
    m.load({
        addiu(t1, zero, Machine::DATA),
        addiu(t0, zero, -1),
        lwl(t0, t1, 4),  // its result is still in the load delay slot
        lwr(t0, t1, 1),  // and this must merge into that, not into $t0
        nop(),
    });
    m.run(5);

    // Merging into the stale register would leave the prefill behind
    // as 0xFF443322 instead.
    CHECK(m.reg(t0) == 0x55443322);
}

TEST_CASE("LWL and LWR each leave the rest of the register alone")
{
    Machine m;
    fill_bytes(m);

    SUBCASE("LWL replaces only the high bytes")
    {
        m.load({
            addiu(t1, zero, Machine::DATA),
            lui(t0, 0xAABB),
            ori(t0, t0, 0xCCDD),
            lwl(t0, t1, 0),
            nop(),
        });
        m.run(5);
        CHECK(m.reg(t0) == 0x11BBCCDD);
    }
    SUBCASE("LWR replaces only the low bytes")
    {
        m.load({
            addiu(t1, zero, Machine::DATA),
            lui(t0, 0xAABB),
            ori(t0, t0, 0xCCDD),
            lwr(t0, t1, 3),
            nop(),
        });
        m.run(5);
        CHECK(m.reg(t0) == 0xAABBCC44);
    }
}

TEST_CASE("an SWL/SWR pair stores a word without disturbing its neighbours")
{
    Machine m;
    fill_bytes(m);
    m.load({
        addiu(t1, zero, Machine::DATA),
        lui(t0, 0xDDCC),
        ori(t0, t0, 0xBBAA),
        swl(t0, t1, 4),  // the word being stored starts at DATA+1
        swr(t0, t1, 1),
    });
    m.run(5);

    CHECK(m.bus->read32(Machine::DATA) == 0xCCBBAA11);
    CHECK(m.bus->read32(Machine::DATA + 4) == 0x887766DD);

    // The bytes to either side of the four written are untouched.
    CHECK(m.bus->read8(Machine::DATA) == 0x11);
    CHECK(m.bus->read8(Machine::DATA + 5) == 0x66);
}

TEST_CASE("the unaligned instructions never fault on alignment")
{
    Machine m;
    fill_bytes(m);
    m.load({
        addiu(t1, zero, Machine::DATA),
        lwl(t0, t1, 1),
        lwr(t0, t1, 2),
        swl(t0, t1, 1),
        swr(t0, t1, 2),
    });
    m.run(5);

    CHECK_FALSE(m.cpu.halted);
    // Still running straight through, rather than sitting on a vector.
    CHECK(m.cpu.pc == Machine::CODE + 5 * 4);
}

TEST_CASE("the branch delay slot runs and the branch still takes")
{
    Machine m;
    m.load({
        addiu(t0, zero, 1),
        beq(zero, zero, 2),
        addiu(t1, zero, 1),  // delay slot: runs even though we branch
        addiu(t2, zero, 1),  // jumped over
        addiu(t3, zero, 1),  // branch target
    });
    m.run(4);

    CHECK(m.reg(t0) == 1);
    CHECK(m.reg(t1) == 1);
    CHECK(m.reg(t2) == 0);
    CHECK(m.reg(t3) == 1);
}

TEST_CASE("JAL links past the delay slot")
{
    const u32 target = Machine::CODE + 0x40;
    Machine m;
    m.load({jal(target), nop()});
    m.run(2);

    CHECK(m.reg(ra) == Machine::CODE + 8);
    CHECK(m.cpu.pc == target);
}

TEST_CASE("an exception in a delay slot reports the branch")
{
    Machine m;
    m.load({beq(zero, zero, 2), syscall_op()});
    m.run(2);

    CHECK(m.exc_code() == 8);
    CHECK(m.cpu.epc == Machine::CODE);  // the branch, not the slot
    CHECK(m.in_branch_delay());
}

TEST_CASE("a later exception clears the branch delay flag")
{
    Machine m;
    m.load({beq(zero, zero, 2), syscall_op()});
    m.run(2);
    REQUIRE(m.in_branch_delay());

    m.load({syscall_op()});
    m.run(1);

    CHECK(m.exc_code() == 8);
    CHECK_FALSE(m.in_branch_delay());
}

TEST_CASE("an exception keeps the pending interrupt bits in Cause")
{
    Machine m;
    m.cpu.cause = 1u << 10;  // IP2, as the interrupt controller sets it
    m.load({syscall_op()});
    m.run(1);

    CHECK(m.exc_code() == 8);
    CHECK((m.cpu.cause & (1u << 10)) != 0);
}

TEST_CASE("an exception pushes the mode stack and RFE pops it")
{
    Machine m;
    m.cpu.sr = 0b000011;  // current level: interrupts on, user mode
    m.load({syscall_op()});
    m.run(1);
    CHECK((m.cpu.sr & 0x3F) == 0b001100);  // pushed, kernel with IRQs off

    m.load({rfe()});
    m.run(1);
    CHECK((m.cpu.sr & 0x3F) == 0b000011);
}

TEST_CASE("an unaligned load raises AdEL with the address in BadVaddr")
{
    Machine m;
    m.load({addiu(t1, zero, Machine::DATA + 2), lw(t0, t1, 0)});
    m.run(2);

    CHECK(m.exc_code() == 4);
    CHECK(m.cpu.bad_vaddr == Machine::DATA + 2);
    CHECK_FALSE(m.cpu.halted);
}

TEST_CASE("an unaligned store raises AdES")
{
    Machine m;
    m.load({addiu(t1, zero, Machine::DATA + 1), sw(t0, t1, 0)});
    m.run(2);

    CHECK(m.exc_code() == 5);
    CHECK(m.cpu.bad_vaddr == Machine::DATA + 1);
    CHECK_FALSE(m.cpu.halted);
}

TEST_CASE("signed overflow traps and leaves the destination alone")
{
    Machine m;
    m.load({
        lui(t0, 0x7FFF),
        ori(t0, t0, 0xFFFF),  // t0 = INT32_MAX
        addiu(t1, zero, 1),
        add(t2, t0, t1),
    });
    m.run(4);

    CHECK(m.exc_code() == 0xC);
    CHECK(m.reg(t2) == 0);
    CHECK_FALSE(m.cpu.halted);
}

TEST_CASE("SUB subtracts, and traps rather than wrapping")
{
    SUBCASE("an ordinary difference")
    {
        Machine m;
        m.load({
            addiu(t0, zero, 7),
            addiu(t1, zero, 9),
            sub(t2, t0, t1),
        });
        m.run(3);

        CHECK(m.reg(t2) == static_cast<u32>(-2));
        CHECK_FALSE(m.cpu.halted);
    }

    SUBCASE("one that will not fit")
    {
        Machine m;
        m.load({
            lui(t0, 0x8000),  // t0 = INT32_MIN
            addiu(t1, zero, 1),
            sub(t2, t0, t1),
        });
        m.run(3);

        CHECK(m.exc_code() == 0xC);
        CHECK(m.reg(t2) == 0);
        CHECK_FALSE(m.cpu.halted);
    }
}

TEST_CASE("an unmasked interrupt is taken before the instruction")
{
    Machine m;
    m.cpu.sr = (1u << 10) | 1;  // Im2 set, interrupts enabled
    m.cpu.cause = 1u << 10;     // IP2 pending
    m.load({addiu(t0, zero, 1)});
    m.run(1);

    CHECK(m.exc_code() == 0);
    CHECK(m.cpu.epc == Machine::CODE);
    CHECK(m.reg(t0) == 0);  // aborted, and re-runs on return
}

TEST_CASE("an interrupt is ignored while interrupts are disabled")
{
    Machine m;
    m.cpu.sr = 1u << 10;     // unmasked, but IEc clear
    m.cpu.cause = 1u << 10;  // IP2 pending
    m.load({addiu(t0, zero, 1)});
    m.run(1);

    CHECK(m.reg(t0) == 1);
}

TEST_CASE("an unimplemented opcode halts instead of trapping")
{
    Machine m;
    m.load({0xFFFFFFFF});
    m.run(1);

    CHECK(m.cpu.halted);
}

TEST_CASE("MFC0 reads back through the load delay slot")
{
    Machine m;
    m.cpu.cause = 1u << 10;
    m.load({
        mfc0(t0, 13),        // Cause
        addu(t1, t0, zero),  // delay slot: t0 not updated yet
        addu(t2, t0, zero),
    });
    m.run(3);

    CHECK(m.reg(t1) == 0);
    CHECK(m.reg(t2) == (1u << 10));
}
