#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "types.h"

struct State;

// The master clock everything on the console is timed against:
// 33.8688 MHz, which is 44100 * 768 — every clock in the machine is
// derived from the audio sample rate.
constexpr u64 CPU_CLOCK_HZ = 33'868'800;

// Devices that can ask to be woken at a future time. Each kind holds
// at most one pending event, which is all the hardware needs: a timer
// has one next overflow, the video signal one next scanline.
enum class EventKind : u32 {
    // The end of a scanline, which is the machine's finest video
    // heartbeat. Vertical blank is not scheduled separately: it is
    // whichever scanline the GPU says begins it.
    Hblank,

    // The drive's heartbeat. Unlike the video signal it has nothing to
    // do at most of them: it is a mechanism running to its own timing,
    // and this is how often the rest of the machine looks to see
    // whether it has finished what it was asked.
    CdRom,

    // The root counters being brought up to date. They work out their
    // own value whenever one is read, so this exists only so a target
    // they pass unread still raises its interrupt on time.
    Timers,

    // One sample coming out of the sound processor. The finest
    // heartbeat the machine has, and the one the rest are derived
    // from: everything else here is some whole number of these.
    Spu,

    // A controller acknowledging the byte it was just sent. Unlike the
    // others this is scheduled when it is needed and not repeated:
    // there is nothing to wake up for until something is said.
    Sio,

    Count,  // sentinel: number of kinds, never a real event
};

// A fired event, with the cycle it was scheduled for. That timestamp
// is usually a little behind `now`, because the CPU can only stop on
// an instruction boundary and the last one may straddle the deadline.
// A repeating event must schedule its next occurrence relative to
// `deadline`, never to `now` — otherwise each overshoot is added to
// the period and the event slowly drifts late.
struct DueEvent {
    EventKind kind;
    u64 deadline;
};

// Timekeeping for the whole machine.
//
// `now` is the master clock, driven forward by the CPU as it retires
// instructions. Nothing else is stepped in lockstep with it: a device
// that will need attention in 500,000 cycles says so, and costs
// nothing until that moment arrives. The run loop asks for the next
// deadline, lets the CPU run freely up to it, and only then dispatches
// — so events land on the exact cycle they were scheduled for without
// anything being polled per instruction.
struct Scheduler {
    Scheduler() { reset(); }

    void reset();

    void visit_state(State& state);

    // Advances the master clock. The CPU is the only thing that
    // should call this; everything else is scheduled against it.
    void advance(u64 cycles) { now += cycles; }

    // schedule_at takes an absolute timestamp, which is what a
    // repeating event wants: it can add its period to the deadline it
    // just fired at and stay exactly on rate.
    void schedule_at(EventKind kind, u64 timestamp);
    void schedule_in(EventKind kind, u64 delay);
    void cancel(EventKind kind);

    // Timestamp of the earliest pending event, or NEVER when nothing
    // is scheduled. The run loop uses it as the limit on how long the
    // CPU may run undisturbed.
    u64 next_deadline() const;

    // Removes and returns the earliest event that is now due, oldest
    // first, until none are left. A handler that wants to repeat must
    // schedule itself again — nothing here is periodic by itself.
    std::optional<DueEvent> next_due();

    // Far enough away to never arrive: at 33.9 MHz a u64 of cycles
    // outlasts the machine by some seventeen thousand years.
    static constexpr u64 NEVER = UINT64_MAX;

    u64 now = 0;

private:
    static constexpr std::size_t EVENT_COUNT =
        static_cast<std::size_t>(EventKind::Count);

    // deadlines[kind] == NEVER means nothing is pending for it. A flat
    // array rather than a heap: with a handful of kinds a scan is
    // cheaper than keeping them ordered, and it makes rescheduling an
    // existing event a plain assignment.
    std::array<u64, EVENT_COUNT> deadlines{};
};
