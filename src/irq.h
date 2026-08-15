#pragma once

#include "types.h"

// The devices that can interrupt the CPU, numbered by the bit each one
// owns in the controller's two registers. The order is the hardware's,
// and it doubles as the priority order the BIOS handler scans in.
enum class Interrupt : u32 {
    VBlank = 0,
    Gpu = 1,
    CdRom = 2,
    Dma = 3,
    Timer0 = 4,
    Timer1 = 5,
    Timer2 = 6,
    Controller = 7,  // and memory card
    Sio = 8,
    Spu = 9,
    Lightpen = 10,
};

// The interrupt controller: eleven device lines funnelled into the one
// interrupt input the CPU actually has.
//
// The CPU cannot tell the devices apart. All it sees is COP0's IP2 bit
// going high, and it is this controller that decides when that happens:
// whenever some line is both pending and unmasked. So the handler's
// first job is always to read I_STAT and work out who called.
//
//   I_STAT  0x1F801070  a line is set here when its device fires, and
//                       stays set until software clears it
//   I_MASK  0x1F801074  which of those lines are allowed through
//
// Writing to I_STAT acknowledges rather than assigns: a bit written as
// zero is cleared, a bit written as one is left alone. That is the
// inverse of the obvious reading, and it is what lets a handler clear
// the one line it serviced — by writing all ones except that bit —
// without racing a device that fires while it works.
struct Irq {
    static constexpr u32 STATUS = 0x1F801070;
    static constexpr u32 MASK = 0x1F801074;

    void reset();

    // Called by a device when it fires. The line latches: it stays
    // pending until software acknowledges it, whether or not it is
    // masked, so unmasking later still delivers it.
    void raise(Interrupt line);

    // A write to I_STAT. Clears every line whose bit is zero in
    // `value`, leaving the rest pending.
    void acknowledge(u16 value);

    // Whether the CPU's interrupt line is high right now. Nothing is
    // latched here — this is a wire, and it goes low again the moment
    // the last pending line is acknowledged or masked off.
    bool active() const { return (status & mask) != 0; }

    u16 status = 0;  // I_STAT
    u16 mask = 0;    // I_MASK
};
