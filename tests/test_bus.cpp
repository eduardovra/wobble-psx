#include <memory>

#include <doctest/doctest.h>

#include "bus.h"
#include "machine.h"

namespace {

// An address in the expansion region, which no device here claims.
constexpr u32 UNMAPPED = 0x1F900000;

}  // namespace

TEST_CASE("an unhandled address is reported once, however often it is hit")
{
    const LooseBus bus;

    CHECK(bus->note_unhandled(UNMAPPED));
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));

    // A different missing device still gets its own report.
    CHECK(bus->note_unhandled(UNMAPPED + 4));
}

TEST_CASE("a read and a write to one register are one missing device")
{
    const LooseBus bus;

    bus->read32(UNMAPPED);
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));

    bus->write32(UNMAPPED, 0);
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));
}

TEST_CASE("a register read to the exclusion of all else is a stalled guest")
{
    LooseBus bus;
    constexpr u32 GPUSTAT = 0x1F801814;
    constexpr u32 IRQ_STATUS = 0x1F801070;

    for (int i = 0; i < 1000; i++) {
        bus->read32(GPUSTAT);
    }
    CHECK(bus->poll.address == GPUSTAT);
    CHECK(bus->poll.reads == 1000);
    CHECK_FALSE(bus->poll.reported);

    // A handler interrupting the wait to read its own register is not
    // the wait moving on, and does not start the count over.
    bus->read32(IRQ_STATUS);
    bus->read32(GPUSTAT);
    CHECK(bus->poll.address == GPUSTAT);
    CHECK(bus->poll.reads == 1001);

    // A second of console time is as far as one count reaches, so a
    // register read steadily but slowly never adds up to a stall.
    bus.scheduler.now += CPU_CLOCK_HZ + 1;
    bus->read32(GPUSTAT);
    CHECK(bus->poll.reads == 1);
}

TEST_CASE("a stall is reported once and not once per read")
{
    LooseBus bus;
    constexpr u32 GPUSTAT = 0x1F801814;
    constexpr u64 STUCK_READS = 500'000;

    for (u64 i = 0; i < STUCK_READS - 1; i++) {
        bus->read32(GPUSTAT);
    }
    CHECK_FALSE(bus->poll.reported);

    bus->read32(GPUSTAT);
    CHECK(bus->poll.reported);

    // A stuck program stays stuck, so the next window reaches the same
    // count again — and finds the report already made, which is what
    // keeps one stall to one line.
    bus.scheduler.now += CPU_CLOCK_HZ + 1;
    for (u64 i = 0; i < STUCK_READS; i++) {
        bus->read32(GPUSTAT);
    }
    CHECK(bus->poll.reads == STUCK_READS);
    CHECK(bus->poll.reported);
}

TEST_CASE("KUSEG, KSEG0 and KSEG1 are windows onto the same RAM")
{
    const LooseBus bus;
    bus->write32(0x00001000, 0xDEADBEEF);

    CHECK(bus->read32(0x00001000) == 0xDEADBEEF);  // KUSEG
    CHECK(bus->read32(0x80001000) == 0xDEADBEEF);  // KSEG0, cached
    CHECK(bus->read32(0xA0001000) == 0xDEADBEEF);  // KSEG1, uncached

    // Writing through one window is visible from the others.
    bus->write32(0xA0001000, 0x12345678);
    CHECK(bus->read32(0x00001000) == 0x12345678);
}

TEST_CASE("memory is little-endian")
{
    const LooseBus bus;
    bus->write32(0x00001000, 0x12345678);

    CHECK(bus->read8(0x00001000) == 0x78);
    CHECK(bus->read8(0x00001003) == 0x12);
    CHECK(bus->read16(0x00001000) == 0x5678);
}

TEST_CASE("an absent expansion device reads as all ones")
{
    const LooseBus bus;

    CHECK(bus->read8(0x1F000000) == 0xFF);
}

TEST_CASE("an absent expansion device reads as all ones at every width")
{
    const LooseBus bus;

    // Expansion 2, the debug port, is the one the console leaves in
    // the middle of the register range.
    CHECK(bus->read8(0x1F802000) == 0xFF);
    CHECK(bus->read16(0x1F802000) == 0xFFFF);
    CHECK(bus->read32(0x1F802000) == 0xFFFFFFFF);
}

TEST_CASE("a narrow write hands a device the whole register behind it")
{
    const LooseBus bus;

    // DPCR takes all 32 bits of whatever the bus carried, so a byte
    // store leaves the register holding the word the store came from
    // rather than the byte it named.
    bus->write8(0x1F8010F0, 0x12345678);

    CHECK(bus->read32(0x1F8010F0) == 0x12345678);
}

TEST_CASE("the CD-ROM's byte reaches every lane of a wider access")
{
    const LooseBus bus;

    const u32 status = bus->read8(0x1F801800);
    CHECK(bus->read16(0x1F801800) == (status | status << 8));
    CHECK(bus->read32(0x1F801800) ==
          (status | status << 8 | status << 16 | status << 24));
}

TEST_CASE("a wider write to the CD-ROM leaves the byte it ends with")
{
    const LooseBus bus;

    // The low two bits of the first register are the index the other
    // three are read through, and a halfword write sets them from its
    // high byte rather than its low one.
    bus->write16(0x1F801800, 0x12345678);

    CHECK((bus->read8(0x1F801800) & 3) == 2);
}

TEST_CASE("there is no instruction to fetch outside memory")
{
    const LooseBus bus;

    bus->write32(0x00001000, 0xDEADBEEF);
    CHECK(bus->fetch(0x00001000) == 0xDEADBEEF);

    // The scratchpad holds data and cannot be executed from at all,
    // whatever has been written into it.
    bus->write32(Bus::SCRATCHPAD_START, 0xDEADBEEF);
    CHECK_FALSE(bus->fetch(Bus::SCRATCHPAD_START).has_value());

    // A hardware register answers a fetch with whatever it holds, but
    // the gaps between the registers answer nothing.
    bus->write32(0x1F8010F0, 0xDEADBEEF);
    CHECK(bus->fetch(0x1F8010F0) == 0xDEADBEEF);
    CHECK_FALSE(bus->fetch(0x1F801078).has_value());
    CHECK_FALSE(bus->fetch(UNMAPPED).has_value());
}
