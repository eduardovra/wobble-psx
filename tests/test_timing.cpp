#include <doctest/doctest.h>

#include "dma.h"
#include "machine.h"

using namespace mips;

// What an instruction costs the master clock is what paces the whole
// machine against the video signal, so these assert cycle counts
// rather than register values. The figures are the hardware
// measurements in Bus, and the point of each test is which of them a
// given instruction ends up paying.

TEST_CASE("an instruction that touches no memory costs one cycle")
{
    Machine machine;
    machine.load({addiu(t0, zero, 1), addu(t1, t0, t0), nop()});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == 1);
}

// The fetch reads RAM like anything else, so if it were billed the nop
// above would cost seven rather than one. It is not, because the
// instruction cache that makes it nearly free on hardware is not
// modelled, and charging nothing is the nearer of the two answers.
TEST_CASE("fetching the instruction is not charged")
{
    Machine machine;
    machine.load({nop(), nop()});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == 1);
}

TEST_CASE("a load pays for the region it reads")
{
    Machine machine;
    machine.bus->write32(Machine::DATA, 0xDEADBEEF);
    machine.load({lui(t0, 0), lw(t1, t0, Machine::DATA)});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == Bus::RAM_LOAD_CYCLES);
}

TEST_CASE("a hardware register reads faster than RAM does")
{
    Machine machine;
    // I_MASK, which hands back whatever was last written to it.
    machine.load({lui(t0, 0x1F80), lw(t1, t0, 0x1074)});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == Bus::IO_LOAD_CYCLES);
    CHECK(Bus::IO_LOAD_CYCLES < Bus::RAM_LOAD_CYCLES);
}

// The BIOS ROM is on an 8-bit bus, so it is read a byte at a time and
// a load is charged by the width it asks for rather than a flat price.
// The figures are ps1-tests' cpu/access-time off a console: 7.6, 12.94
// and 24.94 cycles for the three widths. No image is loaded here: the
// price is the region's, not the content's.
TEST_CASE("a load from the BIOS ROM pays by width")
{
    Machine machine;
    machine.load(
        {lui(t0, 0xBFC0), lw(t1, t0, 0), lh(t1, t0, 0), lb(t1, t0, 0)});

    CHECK(machine.cpu.step() == 1);

    CHECK(machine.cpu.step() == 25);
    CHECK(machine.cpu.step() == 13);
    CHECK(machine.cpu.step() == 7);
}

// The two devices that a game polls hardest are the two slowest things
// on the bus. Console figures again: the CD-ROM answers a byte in 8
// cycles, and the SPU takes 18 for a halfword — six times what a
// register on the main bus costs, which is what makes a polling loop
// on either of them run at the speed it does.
TEST_CASE("the CD-ROM and the SPU are the slow devices")
{
    Machine machine;
    machine.load({lui(t0, 0x1F80),
                  lb(t1, t0, 0x1800),
                  lw(t1, t0, 0x1800),
                  lh(t1, t0, 0x1DAA),
                  lw(t1, t0, 0x1DA8)});

    CHECK(machine.cpu.step() == 1);

    CHECK(machine.cpu.step() == 8);
    CHECK(machine.cpu.step() == 26);
    CHECK(machine.cpu.step() == 18);
    CHECK(machine.cpu.step() == 39);
}

// The costs are not constants: they come out of the memory-control
// registers, and software may write them. A game that tells the CD-ROM
// it may answer in a single cycle is charged for a single cycle.
TEST_CASE("what a device costs follows the memory-control registers")
{
    Machine machine;
    constexpr u32 CDROM_DELAY = 0x1F801018;

    machine.bus->write32(CDROM_DELAY, 0x00020800);  // no delay at all
    machine.load({lui(t0, 0x1F80), lb(t1, t0, 0x1800)});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == 4);
}

// Stores go to the write queue and the CPU carries on without waiting,
// which is why a loop that writes costs so much less than one that
// reads. The queue is four deep and can fill; that is not modelled.
TEST_CASE("a store costs no more than an instruction that stores nothing")
{
    Machine machine;
    machine.load({lui(t0, 0), sw(t0, t0, Machine::DATA)});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == 1);
}

// LWL and LWR each read the aligned word holding their end of the
// target, so moving one unaligned word is two full accesses.
TEST_CASE("each half of an unaligned load pays a whole access")
{
    Machine machine;
    machine.load({lui(t0, 0),
                  lwl(t1, t0, Machine::DATA + 3),
                  lwr(t1, t0, Machine::DATA)});

    CHECK(machine.cpu.step() == 1);
    CHECK(machine.cpu.step() == Bus::RAM_LOAD_CYCLES);
    CHECK(machine.cpu.step() == Bus::RAM_LOAD_CYCLES);
}

// The store to CHCR arms a channel; it does not run it. So it pays for
// its own write and nothing else, and none of the transfer is billed
// to it. What the CPU does pay is the bus it has not got while the
// words move — and that lands on the instruction that comes after,
// which is the difference between a transfer taking time and a
// transfer stopping the clock dead.
TEST_CASE("the store that starts a transfer pays only for itself")
{
    Machine machine;
    constexpr u32 IO_BASE = 0x1F800000;  // what the lui below leaves in t0
    constexpr u32 OTC = Dma::BASE + 6 * 0x10;

    // Channel 6 is off in the default DPCR, so enable it first — with
    // CHCR still clear, nothing starts yet.
    machine.bus->write32(Dma::BASE + 0x70, 0x0F654321);
    machine.bus->write32(OTC + 0x0, Machine::DATA);
    machine.bus->write32(OTC + 0x4, 8);

    // An ordering table of eight entries: enable, trigger, descending.
    machine.load({lui(t0, 0x1F80),
                  lui(t1, 0x1100),
                  ori(t1, t1, 2),
                  sw(t1, t0, static_cast<s32>(OTC + 0x8 - IO_BASE))});
    machine.run(3);

    CHECK(machine.cpu.step() == 1);

    // Nothing has moved yet: the channel is armed, and waiting for the
    // first turn the controller gives it.
    CHECK(machine.bus->read32(Machine::DATA) == 0);

    machine.settle();
    CHECK(machine.bus->read32(Machine::DATA) != 0);

    // Eight words of ordering table, and the controller had the bus
    // for every one of them — which is the wait the next instruction
    // to want RAM is charged, in place of the store being charged for
    // a transfer it only asked for.
    CHECK(machine.bus->dma_hold_until >= 8);
}
