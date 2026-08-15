#include <doctest/doctest.h>

#include "scheduler.h"

TEST_CASE("an idle scheduler has nothing to report")
{
    Scheduler scheduler;

    CHECK(scheduler.next_deadline() == Scheduler::NEVER);
    CHECK_FALSE(scheduler.next_due().has_value());
}

TEST_CASE("an event fires on its deadline, not before")
{
    Scheduler scheduler;
    scheduler.schedule_in(EventKind::Hblank, 100);
    CHECK(scheduler.next_deadline() == 100);

    scheduler.advance(99);
    CHECK_FALSE(scheduler.next_due().has_value());

    scheduler.advance(1);
    const std::optional<DueEvent> event = scheduler.next_due();
    REQUIRE(event.has_value());
    CHECK(event->kind == EventKind::Hblank);
    CHECK(event->deadline == 100);
}

TEST_CASE("a fired event is not delivered twice")
{
    Scheduler scheduler;
    scheduler.schedule_in(EventKind::Hblank, 10);
    scheduler.advance(10);

    REQUIRE(scheduler.next_due().has_value());
    CHECK_FALSE(scheduler.next_due().has_value());
    CHECK(scheduler.next_deadline() == Scheduler::NEVER);
}

TEST_CASE("a cancelled event never fires")
{
    Scheduler scheduler;
    scheduler.schedule_in(EventKind::Hblank, 10);
    scheduler.cancel(EventKind::Hblank);
    scheduler.advance(100);

    CHECK(scheduler.next_deadline() == Scheduler::NEVER);
    CHECK_FALSE(scheduler.next_due().has_value());
}

TEST_CASE("rescheduling can only be done from the deadline")
{
    // Stepping in chunks that do not divide the period overshoots
    // every deadline, which is what variable instruction costs do.
    constexpr u64 PERIOD = 100;
    constexpr u64 CHUNK = 13;
    constexpr u64 RUN = 10'000;

    Scheduler scheduler;
    scheduler.schedule_in(EventKind::Hblank, PERIOD);

    u64 fired = 0;
    while (scheduler.now < RUN) {
        scheduler.advance(CHUNK);
        while (const std::optional<DueEvent> e = scheduler.next_due()) {
            fired++;
            CHECK(e->deadline == fired * PERIOD);
            scheduler.schedule_at(e->kind, e->deadline + PERIOD);
        }
    }
    CHECK(fired == RUN / PERIOD);

    // The same run counting from `now` instead folds each overshoot
    // into the period, so the event slips progressively late.
    Scheduler drifting;
    drifting.schedule_in(EventKind::Hblank, PERIOD);
    u64 drifted = 0;
    while (drifting.now < RUN) {
        drifting.advance(CHUNK);
        while (const std::optional<DueEvent> e = drifting.next_due()) {
            drifted++;
            drifting.schedule_in(e->kind, PERIOD);
        }
    }
    CHECK(drifted < fired);
}

TEST_CASE("reset clears the clock and every pending event")
{
    Scheduler scheduler;
    scheduler.schedule_in(EventKind::Hblank, 10);
    scheduler.advance(5);
    scheduler.reset();

    CHECK(scheduler.now == 0);
    CHECK(scheduler.next_deadline() == Scheduler::NEVER);
}
