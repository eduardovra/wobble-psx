#include <memory>
#include <vector>

#include <doctest/doctest.h>

#include "cdrom.h"
#include "console.h"
#include "irq.h"

namespace {

// The drive talked to through the bus, exactly as the BIOS does it, so
// the register banking and the byte-wide decode are part of what is
// tested. Time is moved by hand rather than by running the CPU: what
// matters here is that an answer arrives after the right interval, not
// what the processor was doing meanwhile.
struct Drive {
    std::unique_ptr<Console> console = std::make_unique<Console>();

    Drive()
    {
        console->reset();
        set_index(1);
        write(2, 0x1F);  // let every interrupt through
        set_index(0);
    }

    void set_index(u8 index) const { write(0, index); }

    void write(u32 offset, u8 value) const
    {
        console->bus.write8(CdRom::BASE + offset, value);
    }

    u8 read(u32 offset) const
    {
        return console->bus.read8(CdRom::BASE + offset);
    }

    void command(u8 code, const std::vector<u8>& parameters = {}) const
    {
        set_index(0);
        for (const u8 parameter : parameters) {
            write(2, parameter);
        }
        write(1, code);
    }

    void advance(u64 cycles) const
    {
        console->scheduler.advance(cycles);
        console->dispatch_due_events();
    }

    // The interrupt the drive is holding out, or zero when it has none.
    u8 pending() const
    {
        set_index(1);
        return read(3) & 0x07;
    }

    void acknowledge() const
    {
        set_index(1);
        write(3, 0x07);
    }

    std::vector<u8> answer() const
    {
        set_index(0);
        std::vector<u8> bytes;
        while ((read(0) & 0x20) != 0) {
            bytes.push_back(read(1));
        }
        return bytes;
    }

    bool raised() const
    {
        const u32 line = 1u << static_cast<u32>(Interrupt::CdRom);
        return (console->bus.irq.status & line) != 0;
    }
};

// Comfortably longer than any answer takes, for the cases that care
// only that one eventually arrived.
constexpr u64 LONG_ENOUGH = 0x40000;

}  // namespace

TEST_CASE("the drive answers a command, but not straight away")
{
    const Drive drive;
    drive.command(0x01);  // GetStat

    drive.advance(1024);
    CHECK(drive.pending() == 0);
    CHECK_FALSE(drive.raised());

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.raised());
    CHECK(drive.answer() == std::vector<u8>{0x02});
}

TEST_CASE("the controller reports its own firmware version")
{
    const Drive drive;
    drive.command(0x19, {0x20});  // Test, sub-function 20h

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.answer() == std::vector<u8>{0x94, 0x09, 0x19, 0xC0});
}

TEST_CASE("an empty drive reports no disc, which is what sends the "
          "BIOS to its shell")
{
    const Drive drive;
    drive.command(0x1A);  // GetID

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.answer() == std::vector<u8>{0x02});

    // The second answer is held back until the first is taken.
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);

    drive.acknowledge();
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() ==
          std::vector<u8>{0x0A, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("a masked-off interrupt leaves the CPU's line alone")
{
    const Drive drive;
    drive.set_index(1);
    drive.write(2, 0x00);  // interrupt enable, all off
    drive.command(0x01);

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK_FALSE(drive.raised());
}

TEST_CASE("an unknown command is refused rather than ignored")
{
    const Drive drive;
    drive.command(0x7F);

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() == std::vector<u8>{0x03, 0x40});
}

TEST_CASE("parameters reach the command that was written after them")
{
    const Drive drive;
    drive.command(0x0E, {0x80});  // Setmode
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    drive.acknowledge();

    drive.command(0x0F);  // Getparam reads the mode back
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x02, 0x80, 0x00, 0x00, 0x00});
}

TEST_CASE("asking for a sector already in hand does not rewind it")
{
    const Drive drive;
    CdRom& cdrom = drive.console->bus.cdrom;

    // A sector whose every byte says where in the sector it came from,
    // so a FIFO that restarted is told apart from one that carried on.
    for (u32 i = 0; i < cdrom.sector.size(); i++) {
        cdrom.sector[i] = static_cast<u8>(i);
    }

    drive.command(0x0E, {0x20});  // Setmode, whole sectors
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    // A game reads the twelve bytes of header and subheader first, and
    // asks again before taking the data behind them. The second ask
    // must leave the first where it finished.
    drive.set_index(0);
    drive.write(3, 0x80);
    for (u8 offset = 12; offset < 24; offset++) {
        CHECK(drive.read(2) == offset);
    }

    drive.write(3, 0x80);
    CHECK(drive.read(2) == 24);
    CHECK(drive.read(2) == 25);

    // Withdrawing the request does throw the rest away, and asking
    // afresh then presents the sector from its start.
    drive.write(3, 0x00);
    CHECK(drive.read(2) == 0);
    drive.write(3, 0x80);
    CHECK(drive.read(2) == 12);
}

TEST_CASE("the status register tracks both FIFOs")
{
    const Drive drive;
    drive.set_index(0);
    CHECK((drive.read(0) & 0x08) != 0);  // parameters empty
    CHECK((drive.read(0) & 0x10) != 0);  // room for more
    CHECK((drive.read(0) & 0x20) == 0);  // no answer waiting

    drive.write(2, 0x20);
    CHECK((drive.read(0) & 0x08) == 0);

    drive.write(1, 0x19);
    CHECK((drive.read(0) & 0x80) != 0);  // busy until it answers
    drive.advance(LONG_ENOUGH);
    CHECK((drive.read(0) & 0x80) == 0);
    CHECK((drive.read(0) & 0x20) != 0);
}
