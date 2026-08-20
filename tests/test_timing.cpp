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
// No image is loaded here: the price is the region's, not the
// content's.
TEST_CASE("a load from the BIOS ROM is the slowest of them, and pays by width")
{
    Machine machine;
    machine.load({lui(t0, 0xBFC0), lw(t1, t0, 0), lb(t1, t0, 0)});

    CHECK(machine.cpu.step() == 1);

    const u32 word = machine.cpu.step();
    const u32 byte = machine.cpu.step();
    CHECK(word == 1 + Bus::BIOS_LOAD_CYCLES_PER_BYTE * 4);
    CHECK(byte == 1 + Bus::BIOS_LOAD_CYCLES_PER_BYTE);
    CHECK(word > Bus::RAM_LOAD_CYCLES);
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

// A transfer moves as much memory as it likes, and all of it happens
// inside the store to CHCR that starts it. Billing that to the store
// would stop the clock dead for the length of the transfer, so the
// instruction pays for its own write and nothing else.
TEST_CASE("a DMA transfer is not billed to the store that starts it")
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

    // The cycles were skipped, not the work.
    CHECK(machine.bus->read32(Machine::DATA) != 0);
}
