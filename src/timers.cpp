#include "timers.h"

#include <algorithm>
#include <limits>

#include "gpu.h"
#include "irq.h"
#include "savestate.h"

namespace {

// Mode bits.
constexpr u16 MODE_SYNC_ENABLED = 1 << 0;
constexpr u16 MODE_SYNC = 0x6;  // bits 2..1
constexpr u16 MODE_RESET_ON_TARGET = 1 << 3;
constexpr u16 MODE_IRQ_ON_TARGET = 1 << 4;
constexpr u16 MODE_IRQ_ON_WRAP = 1 << 5;
constexpr u16 MODE_IRQ_REPEATS = 1 << 6;
constexpr u16 MODE_NO_INTERRUPT = 1 << 10;  // low while one is asserted
constexpr u16 MODE_REACHED_TARGET = 1 << 11;
constexpr u16 MODE_REACHED_WRAP = 1 << 12;
constexpr u16 MODE_SOURCE = 0x300;  // bits 9..8
constexpr u16 MODE_WRITABLE = 0x3FF;

constexpr u32 REGISTER_STRIDE = 0x10;
constexpr u32 VALUE_OFFSET = 0x0;
constexpr u32 MODE_OFFSET = 0x4;
constexpr u32 TARGET_OFFSET = 0x8;

constexpr u32 WRAP = 0x10000;

constexpr u64 NEVER = std::numeric_limits<u64>::max();

// How many counts a timer's clock has delivered by `at`, measured from
// the machine starting. Reading a clock as a position rather than as a
// rate is what keeps a divided one in step with the thing it divides:
// the dividers run whether or not a counter is listening, so a counter
// switched on halfway through a tick gets the rest of that tick and no
// more. Two of the four source values mean the CPU clock for every
// timer; the other two mean something different for each.
u64 clock_at(u32 index, u16 mode, const Gpu& gpu, u64 at)
{
    const u16 source = (mode & MODE_SOURCE) >> 8;
    switch (index) {
    case 0:
        if ((source & 1) == 0) {
            return at;
        }
        // Pixels. A dot lasts a whole number of the GPU's cycles,
        // which are 7/11 of ours, so the division happens in the GPU's
        // — rounding the dot to a CPU cycle first would run the widest
        // mode a fifth too fast.
        return at * 11 / (7 * u64{gpu.dot_cycles()});
    case 1:
        if ((source & 1) == 0) {
            return at;
        }
        // Horizontal blankings, counted as they go by rather than
        // worked out from a rate, so a frame holds exactly the 263 of
        // them that it has scanlines.
        return at / Gpu::CYCLES_PER_SCANLINE;
    default:
        return (source & 2) != 0 ? at / 8 : at;
    }
}

// A blanking signal, as the period it repeats on and the stretch of
// that period it is asserted for. Every sync mode is some arrangement
// of "count while it is asserted", "count while it is not" and "start
// over as it begins", so describing the signal once serves all of them.
struct Blank {
    u64 period = 0;
    u64 begins = 0;
    u64 ends = 0;
};

// The signal the given timer watches. Both follow from the clock
// alone: scanlines begin on multiples of CYCLES_PER_SCANLINE and frames
// on multiples of SCANLINES_PER_FRAME of those, because that is how the
// event stepping the GPU's scanline is scheduled. Which is what lets a
// counter be brought up to date for a moment already gone by.
Blank blanking(u32 index, const Gpu& gpu)
{
    if (index == 0) {
        return {Gpu::CYCLES_PER_SCANLINE, 0, gpu.hblank_cycles()};
    }
    constexpr u64 FRAME = Gpu::CYCLES_PER_SCANLINE * Gpu::SCANLINES_PER_FRAME;
    constexpr u64 PICTURE = Gpu::CYCLES_PER_SCANLINE * Gpu::VISIBLE_SCANLINES;
    return {FRAME, PICTURE, FRAME};
}

// What the sync setting is doing to a counter at a given moment.
struct Gate {
    bool counting = true;  // whether the counter is being clocked
    u64 until = NEVER;     // when that stops being so
    bool zeroes = false;   // whether the counter restarts then
    bool frees = false;    // ... and stops watching the signal for good
};

Gate gate_at(u32 index, const Timers::Timer& timer, const Gpu& gpu, u64 at)
{
    if ((timer.mode & MODE_SYNC_ENABLED) == 0 || timer.released) {
        return {};
    }

    const u16 sync = (timer.mode & MODE_SYNC) >> 1;

    // Timer 2 has no blanking signal to watch: two of its settings
    // stop it where it stands and the other two leave it alone.
    if (index == 2) {
        return {sync == 1 || sync == 2, NEVER, false, false};
    }

    const Blank blank = blanking(index, gpu);
    const u64 offset = at % blank.period;
    const u64 started = at - offset;

    // Where the signal next changes, and whether that change is it
    // beginning — which it is whenever it is not asserted now.
    const bool asserted = offset >= blank.begins && offset < blank.ends;
    u64 edge = started + blank.period + blank.begins;
    if (offset < blank.begins) {
        edge = started + blank.begins;
    } else if (asserted) {
        edge = started + blank.ends;
    }

    switch (sync) {
    case 0:  // paused while the signal is asserted
        return {!asserted, edge, false, false};
    case 1:  // put back to zero as it begins
        return {true, edge, !asserted, false};
    case 2:  // both at once: only counts inside, and starts over
        return {asserted, edge, !asserted, false};
    default:  // held until it has been seen once, then free
        return {false, edge, false, !asserted};
    }
}

}  // namespace

void Timers::reset() { timers = {}; }

void Timers::visit_state(State& state)
{
    for (Timer& timer : timers) {
        state(timer.value);
        state(timer.target);
        state(timer.mode);
        state(timer.updated);
        state(timer.fired);
        state(timer.released);
    }
}

void Timers::step(u32 index, u64 counted, Irq& irq)
{
    Timer& timer = timers[index];
    if (counted == 0) {
        return;
    }

    // Where the counter would get to with nothing in the way, and then
    // which of the two events it passed on the journey. Both are worked
    // out from the whole span rather than step by step, so a timer left
    // alone for a second costs the same as one read every instruction.
    const u64 reached = timer.value + counted;
    const bool wrapped = reached >= WRAP;
    const bool hit_target =
        timer.value < timer.target && reached >= timer.target;

    if ((timer.mode & MODE_RESET_ON_TARGET) != 0 && timer.target != 0) {
        timer.value = static_cast<u16>(reached % (timer.target + 1));
    } else {
        timer.value = static_cast<u16>(reached % WRAP);
    }

    if (hit_target) {
        timer.mode |= MODE_REACHED_TARGET;
    }
    if (wrapped) {
        timer.mode |= MODE_REACHED_WRAP;
    }

    const bool wanted_target =
        hit_target && (timer.mode & MODE_IRQ_ON_TARGET) != 0;
    const bool wanted_wrap = wrapped && (timer.mode & MODE_IRQ_ON_WRAP) != 0;
    if (!wanted_target && !wanted_wrap) {
        return;
    }
    // A one-shot interrupt fires once and then stays quiet until the
    // mode is written again, which is what the mode register's repeat
    // bit selects between.
    if (timer.fired && (timer.mode & MODE_IRQ_REPEATS) == 0) {
        return;
    }
    timer.fired = true;
    timer.mode &= static_cast<u16>(~MODE_NO_INTERRUPT);
    irq.raise(
        static_cast<Interrupt>(static_cast<u32>(Interrupt::Timer0) + index));
}

void Timers::advance(u64 now, const Gpu& gpu, Irq& irq)
{
    for (u32 index = 0; index < COUNT; index++) {
        Timer& timer = timers[index];

        // Walk the span in pieces, one for each stretch over which the
        // sync signal leaves the counter alone. With sync off that is
        // the whole span in a single piece; with it on there are a
        // handful, since the heartbeat never lets a counter fall more
        // than a scanline behind.
        while (timer.updated < now) {
            const Gate gate = gate_at(index, timer, gpu, timer.updated);
            const u64 until = std::min(now, gate.until);

            if (gate.counting) {
                const u16 mode = timer.mode;
                const u64 from = clock_at(index, mode, gpu, timer.updated);
                const u64 to = clock_at(index, mode, gpu, until);
                step(index, to - from, irq);
            }
            timer.updated = until;

            if (until != gate.until) {
                continue;
            }
            if (gate.zeroes) {
                timer.value = 0;
            }
            if (gate.frees) {
                timer.released = true;
            }
        }
    }
}

u32 Timers::read_register(u32 phys, u64 now, const Gpu& gpu, Irq& irq)
{
    advance(now, gpu, irq);

    const u32 offset = phys - BASE;
    const u32 index = offset / REGISTER_STRIDE;
    if (index >= COUNT) {
        return 0;
    }
    Timer& timer = timers[index];

    switch (offset % REGISTER_STRIDE) {
    case VALUE_OFFSET:
        return timer.value;
    case MODE_OFFSET: {
        // Reading the mode is what clears the two "it happened" bits,
        // so a handler can tell one cause from the other.
        const u16 value = timer.mode;
        timer.mode &=
            static_cast<u16>(~(MODE_REACHED_TARGET | MODE_REACHED_WRAP));
        timer.mode |= MODE_NO_INTERRUPT;
        return value;
    }
    case TARGET_OFFSET:
        return timer.target;
    default:
        return 0;
    }
}

void Timers::write_register(
    u32 phys, u32 value, u64 now, const Gpu& gpu, Irq& irq)
{
    advance(now, gpu, irq);

    const u32 offset = phys - BASE;
    const u32 index = offset / REGISTER_STRIDE;
    if (index >= COUNT) {
        return;
    }
    Timer& timer = timers[index];

    switch (offset % REGISTER_STRIDE) {
    case VALUE_OFFSET:
        timer.value = static_cast<u16>(value);
        break;
    case MODE_OFFSET:
        // Writing the mode restarts the counter. Software relies on
        // that: setting up a timer and zeroing it are the same act.
        timer.mode =
            static_cast<u16>((value & MODE_WRITABLE) | MODE_NO_INTERRUPT);
        timer.value = 0;
        timer.fired = false;
        timer.released = false;
        break;
    case TARGET_OFFSET:
        timer.target = static_cast<u16>(value);
        break;
    default:
        break;
    }
}
