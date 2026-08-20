#include <doctest/doctest.h>

#include "bus.h"
#include "gpu.h"
#include "irq.h"
#include "machine.h"
#include "timers.h"

namespace {

constexpr u32 value_at(u32 index) { return Timers::BASE + index * 0x10; }
constexpr u32 mode_at(u32 index) { return value_at(index) + 4; }
constexpr u32 target_at(u32 index) { return value_at(index) + 8; }

// Mode bits, by the names the hardware documentation gives them.
constexpr u32 SYNC_ENABLED = 1 << 0;
constexpr u32 RESET_ON_TARGET = 1 << 3;
constexpr u32 IRQ_ON_TARGET = 1 << 4;
constexpr u32 IRQ_ON_WRAP = 1 << 5;
constexpr u32 IRQ_REPEATS = 1 << 6;
constexpr u32 SOURCE_VIDEO = 1 << 8;    // dots for timer 0, lines for 1
constexpr u32 SOURCE_DIVIDED = 2 << 8;  // timer 2's slow clock

// Which of the four things sync enable makes the counter do.
constexpr u32 sync(u32 which) { return which << 1; }

constexpr u32 TIMER2 = 2;

bool raised(const Bus& bus, u32 index)
{
    const u32 line = 1u << (static_cast<u32>(Interrupt::Timer0) + index);
    return (bus.irq.status & line) != 0;
}

}  // namespace

TEST_CASE("a counter answers for the instant it is read")
{
    LooseBus bus;
    bus->write32(mode_at(TIMER2), 0);

    bus.scheduler.advance(1000);
    CHECK(bus->read32(value_at(TIMER2)) == 1000);

    bus.scheduler.advance(234);
    CHECK(bus->read32(value_at(TIMER2)) == 1234);
}

TEST_CASE("the divided clock counts one for every eight cycles")
{
    LooseBus bus;
    bus->write32(mode_at(TIMER2), SOURCE_DIVIDED);

    bus.scheduler.advance(800);
    CHECK(bus->read32(value_at(TIMER2)) == 100);

    // The cycles that did not make a whole count are not lost: four
    // now and four more later still add up to one.
    bus.scheduler.advance(4);
    CHECK(bus->read32(value_at(TIMER2)) == 100);
    bus.scheduler.advance(4);
    CHECK(bus->read32(value_at(TIMER2)) == 101);
}

TEST_CASE("writing the mode restarts the counter")
{
    LooseBus bus;
    bus->write32(mode_at(TIMER2), 0);
    bus.scheduler.advance(500);
    CHECK(bus->read32(value_at(TIMER2)) == 500);

    bus->write32(mode_at(TIMER2), 0);
    CHECK(bus->read32(value_at(TIMER2)) == 0);
}

TEST_CASE("a counter wraps at sixteen bits")
{
    LooseBus bus;
    bus->write32(mode_at(TIMER2), 0);

    bus.scheduler.advance(0x10005);
    CHECK(bus->read32(value_at(TIMER2)) == 5);
}

TEST_CASE("reaching the target can reset the counter instead")
{
    LooseBus bus;
    bus->write32(target_at(TIMER2), 99);
    bus->write32(mode_at(TIMER2), RESET_ON_TARGET);

    bus.scheduler.advance(250);
    CHECK(bus->read32(value_at(TIMER2)) == 50);
}

TEST_CASE("passing the target raises the timer's interrupt")
{
    LooseBus bus;
    bus->write32(target_at(TIMER2), 1000);
    bus->write32(mode_at(TIMER2), IRQ_ON_TARGET);

    bus.scheduler.advance(999);
    bus->read32(value_at(TIMER2));
    CHECK_FALSE(raised(*bus, TIMER2));

    bus.scheduler.advance(1);
    bus->read32(value_at(TIMER2));
    CHECK(raised(*bus, TIMER2));
}

TEST_CASE("wrapping raises it too, when that is what was asked for")
{
    LooseBus bus;
    bus->write32(mode_at(TIMER2), IRQ_ON_WRAP);

    bus.scheduler.advance(0xFFFF);
    bus->read32(value_at(TIMER2));
    CHECK_FALSE(raised(*bus, TIMER2));

    bus.scheduler.advance(1);
    bus->read32(value_at(TIMER2));
    CHECK(raised(*bus, TIMER2));
}

TEST_CASE("a one-shot interrupt fires once and a repeating one keeps going")
{
    {
        LooseBus bus;
        bus->write32(target_at(TIMER2), 100);
        bus->write32(mode_at(TIMER2), IRQ_ON_TARGET | RESET_ON_TARGET);

        bus.scheduler.advance(101);
        bus->read32(value_at(TIMER2));
        CHECK(raised(*bus, TIMER2));

        bus->irq.acknowledge(0);
        bus.scheduler.advance(101);
        bus->read32(value_at(TIMER2));
        CHECK_FALSE(raised(*bus, TIMER2));
    }

    LooseBus bus;
    bus->write32(target_at(TIMER2), 100);
    bus->write32(mode_at(TIMER2),
                 IRQ_ON_TARGET | RESET_ON_TARGET | IRQ_REPEATS);

    bus.scheduler.advance(101);
    bus->read32(value_at(TIMER2));
    CHECK(raised(*bus, TIMER2));

    bus->irq.acknowledge(0);
    bus.scheduler.advance(101);
    bus->read32(value_at(TIMER2));
    CHECK(raised(*bus, TIMER2));
}

TEST_CASE("reading the mode clears the bits saying what happened")
{
    LooseBus bus;
    bus->write32(target_at(TIMER2), 100);
    bus->write32(mode_at(TIMER2), 0);

    bus.scheduler.advance(101);
    bus->read32(value_at(TIMER2));

    const u32 mode = bus->read32(mode_at(TIMER2));
    CHECK((mode & (1 << 11)) != 0);  // it reached its target
    CHECK((bus->read32(mode_at(TIMER2)) & (1 << 11)) == 0);
}

TEST_CASE("timer 1 can count scanlines instead of cycles")
{
    LooseBus bus;
    bus->write32(mode_at(1), SOURCE_VIDEO);

    bus.scheduler.advance(Gpu::CYCLES_PER_SCANLINE * 10);
    CHECK(bus->read32(value_at(1)) == 10);
}

TEST_CASE("timer 0 counts pixels, and a wider mode clocks them out faster")
{
    LooseBus bus;
    bus->gpu.display_mode = 0;  // 256 pixels across
    bus->write32(mode_at(0), SOURCE_VIDEO);
    bus.scheduler.advance(1000);
    const u32 narrow = bus->read32(value_at(0));

    bus->gpu.display_mode = 3;  // 640 across
    bus->write32(mode_at(0), SOURCE_VIDEO);
    bus.scheduler.advance(1000);
    const u32 wide = bus->read32(value_at(0));

    CHECK(wide > narrow);
}

TEST_CASE("a pixel lasts a fraction of a cycle rather than a whole one")
{
    LooseBus bus;
    bus->gpu.display_mode = 1;  // 320 across, eight GPU cycles a pixel
    bus->write32(mode_at(0), SOURCE_VIDEO);

    // Eight of the GPU's cycles are 56/11 of ours, so a thousand of
    // ours is 196 pixels — not the 200 that rounding a pixel down to
    // five whole cycles would count, nor the 143 that rounding it up
    // to six would.
    bus.scheduler.advance(1000);
    CHECK(bus->read32(value_at(0)) == 196);
}

TEST_CASE("timer 0 stops while the line is blanking")
{
    LooseBus bus;
    const u64 blank = bus->gpu.hblank_cycles();
    bus->write32(mode_at(0), SYNC_ENABLED | sync(0));

    // A scanline begins in blanking, so the first stretch is not
    // counted at all.
    bus.scheduler.advance(blank);
    CHECK(bus->read32(value_at(0)) == 0);

    bus.scheduler.advance(1000);
    CHECK(bus->read32(value_at(0)) == 1000);
}

TEST_CASE("timer 0 starts over as each blanking begins")
{
    LooseBus bus;
    bus->write32(mode_at(0), SYNC_ENABLED | sync(1));

    // It counts through the whole line and is put back to zero as the
    // next one begins, so what is left is the time since.
    bus.scheduler.advance(Gpu::CYCLES_PER_SCANLINE + 300);
    CHECK(bus->read32(value_at(0)) == 300);
}

TEST_CASE("timer 0 can be held to the blanking and nothing else")
{
    LooseBus bus;
    const u64 blank = bus->gpu.hblank_cycles();
    bus->write32(mode_at(0), SYNC_ENABLED | sync(2));

    // Counted through the blanking it started in, then held there for
    // the rest of the line.
    bus.scheduler.advance(Gpu::CYCLES_PER_SCANLINE - 100);
    CHECK(bus->read32(value_at(0)) == blank);

    // The next blanking zeroes it on its way in.
    bus.scheduler.advance(400);
    CHECK(bus->read32(value_at(0)) == 300);
}

TEST_CASE("timer 0 can wait for one blanking and then run free")
{
    LooseBus bus;
    bus.scheduler.advance(1000);  // partway along a line
    bus->write32(mode_at(0), SYNC_ENABLED | sync(3));

    bus.scheduler.advance(1000);
    CHECK(bus->read32(value_at(0)) == 0);

    // Let go by the blanking a scanline in, and not caught by any of
    // the ones after it.
    bus.scheduler.advance(Gpu::CYCLES_PER_SCANLINE);
    CHECK(bus->read32(value_at(0)) == 2000);
    bus.scheduler.advance(Gpu::CYCLES_PER_SCANLINE);
    CHECK(bus->read32(value_at(0)) == 2000 + Gpu::CYCLES_PER_SCANLINE);
}

TEST_CASE("timer 1 watches the frame where timer 0 watches the line")
{
    LooseBus bus;
    constexpr u64 PICTURE = Gpu::CYCLES_PER_SCANLINE * Gpu::VISIBLE_SCANLINES;

    bus.scheduler.advance(PICTURE - 1000);
    bus->write32(mode_at(1), SYNC_ENABLED | sync(0));

    bus.scheduler.advance(1000);
    CHECK(bus->read32(value_at(1)) == 1000);

    // Vertical blanking has begun, and nothing more is counted.
    bus.scheduler.advance(5000);
    CHECK(bus->read32(value_at(1)) == 1000);
}

TEST_CASE("two of timer 2's sync settings stop it and two leave it alone")
{
    for (const u32 setting : {0u, 3u}) {
        LooseBus bus;
        bus->write32(mode_at(TIMER2), SYNC_ENABLED | sync(setting));
        bus.scheduler.advance(1000);
        CHECK(bus->read32(value_at(TIMER2)) == 0);
    }

    for (const u32 setting : {1u, 2u}) {
        LooseBus bus;
        bus->write32(mode_at(TIMER2), SYNC_ENABLED | sync(setting));
        bus.scheduler.advance(1000);
        CHECK(bus->read32(value_at(TIMER2)) == 1000);
    }
}
