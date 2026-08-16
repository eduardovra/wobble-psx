#pragma once

#include <string>
#include <vector>

#include "bus.h"
#include "cpu.h"
#include "scheduler.h"

struct State;

// The whole console: memory, devices, a CPU, and the clock they share.
//
// It exists so the run loop is written once. The window, the debugger
// and the tests are all just different things holding one of these,
// and none of them has to know that a scanline event has to be
// dispatched or that the CPU may only run as far as the next deadline.
struct Console {
    // The clock is declared first because the bus is built around it:
    // a device asked for its state has to answer for the instant it
    // was asked, so the read paths need the time.
    Scheduler scheduler;
    Bus bus{scheduler};
    Cpu cpu{bus};

    // Frames completed since reset. Not hardware state — the console
    // does not count its own frames — but the cheapest way to say how
    // far a run has got.
    u64 frames = 0;

    // Puts every part back to its power-on state and starts the video
    // signal running. The BIOS image is left alone, since loading it
    // is not something a reset undoes.
    void reset();

    // Runs one instruction and lets any event it reached fire. Returns
    // the cycles it cost.
    u32 step();

    // Runs until `cycles` of emulated time have passed, or the CPU
    // halts. The CPU is let loose only as far as the next deadline, so
    // an event lands on the exact cycle it asked for rather than
    // whenever the loop next looks.
    void run_cycles(u64 cycles);

    // Everything a scheduled event does, which for now is the video
    // signal advancing a line and vertical blank falling out of it.
    void dispatch_due_events();

    void visit_state(State& state);

    // Serialises the whole machine, or restores it. A state does not
    // include the BIOS, so it must be reloaded into the same image it
    // was taken from; save_state/load_state check a version and refuse
    // rather than restoring nonsense.
    std::vector<u8> save_state();
    bool load_state(const std::vector<u8>& bytes);
};
