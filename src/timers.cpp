#include "timers.h"

#include "gpu.h"
#include "irq.h"
#include "savestate.h"

namespace {

// Mode bits.
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

// The GPU's clock is 11/7 of the CPU's, so a count of its cycles is
// worth this many of ours. Dividing rather than scaling up keeps the
// arithmetic in whole cycles at the cost of being a fraction of a
// percent slow, which is the same trade the scanline period makes.
u64 gpu_cycles_to_cpu(u64 cycles) { return cycles * 7 / 11; }

// How many CPU cycles one count of a timer takes. Two of the four
// source values mean the CPU clock for every timer; the other two mean
// something different for each.
u64 divider(u32 index, u16 mode, const Gpu& gpu)
{
    const u16 source = (mode & MODE_SOURCE) >> 8;
    switch (index) {
    case 0:
        // Pixels. Which is how long depends on the width the display
        // is running at, so this changes when software changes mode.
        return (source & 1) != 0 ? gpu_cycles_to_cpu(gpu.dot_cycles()) : 1;
    case 1:
        return (source & 1) != 0 ? Gpu::CYCLES_PER_SCANLINE : 1;
    default:
        return (source & 2) != 0 ? 8 : 1;
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
        state(timer.leftover);
        state(timer.fired);
    }
}

void Timers::advance(u64 now, const Gpu& gpu, Irq& irq)
{
    for (u32 index = 0; index < COUNT; index++) {
        Timer& timer = timers[index];
        if (now <= timer.updated) {
            continue;
        }

        const u64 period = divider(index, timer.mode, gpu);
        const u64 available = (now - timer.updated) + timer.leftover;
        timer.updated = now;
        timer.leftover = available % period;

        const u64 counted = available / period;
        if (counted == 0) {
            continue;
        }

        // Where the counter would get to with nothing in the way, and
        // then which of the two events it passed on the journey. Both
        // are worked out from the whole span rather than step by step,
        // so a timer left alone for a second costs the same as one
        // read every instruction.
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
        const bool wanted_wrap =
            wrapped && (timer.mode & MODE_IRQ_ON_WRAP) != 0;
        if (!wanted_target && !wanted_wrap) {
            continue;
        }
        // A one-shot interrupt fires once and then stays quiet until
        // the mode is written again, which is what the mode register's
        // repeat bit selects between.
        if (timer.fired && (timer.mode & MODE_IRQ_REPEATS) == 0) {
            continue;
        }
        timer.fired = true;
        timer.mode &= static_cast<u16>(~MODE_NO_INTERRUPT);
        irq.raise(static_cast<Interrupt>(static_cast<u32>(Interrupt::Timer0) +
                                         index));
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
        timer.leftover = 0;
        break;
    case MODE_OFFSET:
        // Writing the mode restarts the counter. Software relies on
        // that: setting up a timer and zeroing it are the same act.
        timer.mode =
            static_cast<u16>((value & MODE_WRITABLE) | MODE_NO_INTERRUPT);
        timer.value = 0;
        timer.leftover = 0;
        timer.fired = false;
        break;
    case TARGET_OFFSET:
        timer.target = static_cast<u16>(value);
        break;
    default:
        break;
    }
}
