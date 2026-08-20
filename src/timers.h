#pragma once

#include <array>

#include "types.h"

struct Gpu;
struct Irq;
struct State;

// The three root counters. Each is a 16-bit counter with a target, and
// the interesting part is what it counts:
//
//   Timer 0  CPU cycles, or pixels of the display being clocked out
//   Timer 1  CPU cycles, or scanlines
//   Timer 2  CPU cycles, or CPU cycles divided by eight
//
//   0x1F801100 + n*0x10  the current value
//                 + 0x4  mode: what to count, when to reset, and
//                        which of the two events raises an interrupt
//                 + 0x8  the target value
//
// Counting pixels and scanlines is what makes them useful: a game
// wanting to change something part way down the screen sets timer 1 to
// a scanline and gets an interrupt there, and nothing else in the
// machine can say where the beam is.
//
// The mode's bottom three bits point the same two counters at blanking
// rather than at the picture. Timer 0 can be stopped during horizontal
// blanking, put back to zero as each one begins, both at once, or held
// until the first one and then let go; timer 1 does the same against
// vertical blanking. Timer 2 has no signal to watch, so two of its four
// settings simply stop it where it stands.
//
// A counter is not stepped as time passes. It records the moment it
// was last correct and works out its value when something asks, so a
// counter nobody reads costs nothing at all. What that cannot do is
// notice a target being passed, so the machine brings them up to date
// on a heartbeat as well — an interrupt is then at most that late,
// while a value software reads is always exact.
struct Timers {
    static constexpr u32 BASE = 0x1F801100;
    static constexpr u32 END = 0x1F801130;
    static constexpr u32 COUNT = 3;

    // The heartbeat above. A scanline is the shortest interval
    // anything in the machine reacts to, so a counter that is up to
    // date once per scanline is late by nothing that can be seen.
    static constexpr u64 TICK_CYCLES = 2172;

    struct Timer {
        u16 value = 0;
        u16 target = 0;
        u16 mode = 0;

        // The instant `value` is correct as of. What has happened
        // since is worked out from the clocks' own positions rather
        // than from a remainder carried along, so a divided clock
        // stays in step with whatever it divides.
        u64 updated = 0;

        // Set once a one-shot interrupt has fired, so it does not fire
        // again until the mode is written.
        bool fired = false;

        // Set once sync mode 3 has seen its blanking interval and let
        // the counter go. It is the counter that is freed, not the
        // mode register — software reads back what it wrote.
        bool released = false;
    };

    void reset();

    void visit_state(State& state);

    // Brings every counter up to `now`, raising the interrupt of any
    // that passed its target or wrapped on the way.
    void advance(u64 now, const Gpu& gpu, Irq& irq);

    // Clocks one counter the given number of times, which is the part
    // of a step that does not depend on how long it took.
    void step(u32 index, u64 counted, Irq& irq);

    u32 read_register(u32 phys, u64 now, const Gpu& gpu, Irq& irq);
    void write_register(u32 phys, u32 value, u64 now, const Gpu& gpu, Irq& irq);

    std::array<Timer, COUNT> timers{};
};
