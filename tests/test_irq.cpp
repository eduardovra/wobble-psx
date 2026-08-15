#include <doctest/doctest.h>

#include "irq.h"
#include "machine.h"

namespace {

// SR bit 0 turns interrupts on; bit 10 is IM2, the mask for the one
// hardware line the PSX uses. Together they are the 0x401 the BIOS
// settles on once it is ready to be interrupted.
constexpr u32 SR_INTERRUPTS_ON = 1 << 0;
constexpr u32 SR_ALLOW_HARDWARE = 1 << 10;

// The matching pending bit in Cause.
constexpr u32 CAUSE_HARDWARE_INTERRUPT = 1 << 10;

constexpr u32 EXCEPTION_VECTOR = 0x80000080;
constexpr u32 EXC_INTERRUPT = 0x0;

constexpr u16 bit(Interrupt line)
{
    return static_cast<u16>(1u << static_cast<u32>(line));
}

}  // namespace

TEST_CASE("a raised line stays pending even while masked")
{
    Irq irq;

    irq.raise(Interrupt::VBlank);
    CHECK(irq.status == bit(Interrupt::VBlank));
    CHECK_FALSE(irq.active());

    // Unmasking later still delivers it: the line was latched, not
    // dropped for arriving at an inconvenient moment.
    irq.mask = bit(Interrupt::VBlank);
    CHECK(irq.active());
}

TEST_CASE("writing I_STAT clears the bits written as zero")
{
    Irq irq;
    irq.raise(Interrupt::VBlank);
    irq.raise(Interrupt::CdRom);

    // How a handler dismisses the one line it serviced: all ones with
    // that bit alone held low.
    irq.acknowledge(static_cast<u16>(~bit(Interrupt::VBlank)));

    CHECK(irq.status == bit(Interrupt::CdRom));
}

TEST_CASE("acknowledging a line that fired again in the meantime keeps it")
{
    Irq irq;
    irq.raise(Interrupt::VBlank);
    irq.acknowledge(bit(Interrupt::VBlank));

    CHECK(irq.status == bit(Interrupt::VBlank));
}

TEST_CASE("the controller registers are reachable through the bus")
{
    Machine m;
    m.bus->write32(Irq::MASK, bit(Interrupt::VBlank));
    m.bus->irq.raise(Interrupt::VBlank);

    CHECK(m.bus->read32(Irq::MASK) == bit(Interrupt::VBlank));
    CHECK(m.bus->read32(Irq::STATUS) == bit(Interrupt::VBlank));

    // A device that has no implementation yet still reads as zero
    // rather than being reported as a hole in the memory map.
    CHECK(m.bus->read32(0x1F801810) == 0);
}

TEST_CASE("Cause reports the controller's line without storing it")
{
    Machine m;
    m.bus->irq.mask = bit(Interrupt::VBlank);

    CHECK((m.cpu.cause_register() & CAUSE_HARDWARE_INTERRUPT) == 0);

    m.bus->irq.raise(Interrupt::VBlank);
    CHECK((m.cpu.cause_register() & CAUSE_HARDWARE_INTERRUPT) != 0);

    // Nothing wrote to Cause, so clearing the line at the controller
    // is enough to lower the bit again.
    m.bus->write32(Irq::STATUS, 0);
    CHECK((m.cpu.cause_register() & CAUSE_HARDWARE_INTERRUPT) == 0);
}

TEST_CASE("an interrupt is ignored while it is masked or disabled")
{
    Machine m;
    m.load({mips::nop(), mips::nop()});
    m.bus->irq.raise(Interrupt::VBlank);

    SUBCASE("masked at the controller")
    {
        m.cpu.sr = SR_INTERRUPTS_ON | SR_ALLOW_HARDWARE;
    }
    SUBCASE("masked at the CPU")
    {
        m.bus->irq.mask = bit(Interrupt::VBlank);
        m.cpu.sr = SR_INTERRUPTS_ON;
    }
    SUBCASE("interrupts disabled")
    {
        m.bus->irq.mask = bit(Interrupt::VBlank);
        m.cpu.sr = SR_ALLOW_HARDWARE;
    }

    m.run(1);

    CHECK(m.cpu.pc == Machine::CODE + 4);
}

TEST_CASE("an interrupt is taken in place of the instruction it interrupts")
{
    Machine m;
    m.load({mips::addiu(mips::t0, mips::zero, 1)});
    m.bus->irq.mask = bit(Interrupt::VBlank);
    m.bus->irq.raise(Interrupt::VBlank);
    m.cpu.sr = SR_INTERRUPTS_ON | SR_ALLOW_HARDWARE;

    m.run(1);

    CHECK(m.cpu.pc == EXCEPTION_VECTOR);
    CHECK(m.exc_code() == EXC_INTERRUPT);
    // Not the instruction after: the interrupted one never ran, and
    // epc points at it so the handler's return re-runs it.
    CHECK(m.cpu.epc == Machine::CODE);
    CHECK(m.reg(mips::t0) == 0);
}

TEST_CASE("an interrupt in a delay slot returns to the branch")
{
    Machine m;
    m.load({mips::beq(mips::zero, mips::zero, 1), mips::nop()});
    m.bus->irq.mask = bit(Interrupt::VBlank);
    m.cpu.sr = SR_INTERRUPTS_ON | SR_ALLOW_HARDWARE;

    m.run(1);  // the branch, which leaves the next step in its slot
    m.bus->irq.raise(Interrupt::VBlank);
    m.run(1);

    CHECK(m.cpu.epc == Machine::CODE);
    CHECK(m.in_branch_delay());
}

TEST_CASE("an unacknowledged interrupt is taken again on return")
{
    Machine m;
    m.load({mips::nop()});
    m.bus->irq.mask = bit(Interrupt::VBlank);
    m.bus->irq.raise(Interrupt::VBlank);
    m.cpu.sr = SR_INTERRUPTS_ON | SR_ALLOW_HARDWARE;

    m.run(1);
    REQUIRE(m.cpu.pc == EXCEPTION_VECTOR);

    // A handler that re-enables interrupts without clearing the line
    // at the controller is interrupted straight back out of itself,
    // because IP2 is still high.
    m.cpu.pc = Machine::CODE;
    m.cpu.next_pc = Machine::CODE + 4;
    m.cpu.sr |= SR_INTERRUPTS_ON;
    m.run(1);
    CHECK(m.cpu.pc == EXCEPTION_VECTOR);

    // Acknowledging is what actually ends it.
    m.bus->write32(Irq::STATUS, 0);
    m.cpu.pc = Machine::CODE;
    m.cpu.next_pc = Machine::CODE + 4;
    m.cpu.sr |= SR_INTERRUPTS_ON;
    m.run(1);
    CHECK(m.cpu.pc == Machine::CODE + 4);
}
