#include "console.h"

#include <algorithm>

#include "cdrom.h"
#include "gpu.h"
#include "irq.h"
#include "savestate.h"
#include "spu.h"
#include "timers.h"

void Console::reset()
{
    cpu.reset();
    bus.irq.reset();
    bus.gpu.reset();
    bus.dma.reset();
    bus.cdrom.reset();
    bus.sio.reset();
    bus.spu.reset();
    bus.timers.reset();
    bus.mdec.reset();
    scheduler.reset();
    frames = 0;
    scheduler.schedule_in(EventKind::Hblank, Gpu::CYCLES_PER_SCANLINE);
    scheduler.schedule_in(EventKind::CdRom, CdRom::TICK_CYCLES);
    scheduler.schedule_in(EventKind::Timers, Timers::TICK_CYCLES);
    scheduler.schedule_in(EventKind::Spu, Spu::TICK_CYCLES);
}

void Console::dispatch_due_events()
{
    while (const std::optional<DueEvent> event = scheduler.next_due()) {
        switch (event->kind) {
        case EventKind::Hblank:
            // Vertical blank falls out of the video signal rather than
            // being scheduled: the GPU says when the scanline it just
            // finished was the last visible one. That is the only path
            // from a scheduled event to the CPU — the device raises
            // its line, the controller decides whether it is unmasked,
            // and the CPU notices on its next step.
            if (bus.gpu.next_scanline()) {
                frames++;
                bus.irq.raise(Interrupt::VBlank);
            }
            // Nothing is periodic on its own; a repeating event asks
            // for its next occurrence as it fires. Counting from the
            // deadline rather than from now keeps it exactly on rate.
            scheduler.schedule_at(EventKind::Hblank,
                                  event->deadline + Gpu::CYCLES_PER_SCANLINE);
            break;
        case EventKind::CdRom:
            if (bus.cdrom.tick(CdRom::TICK_CYCLES)) {
                bus.irq.raise(Interrupt::CdRom);
            }
            scheduler.schedule_at(EventKind::CdRom,
                                  event->deadline + CdRom::TICK_CYCLES);
            break;
        case EventKind::Spu:
            // The one sample the SPU owes the output, whether or not
            // anything is listening: a machine whose host has no sound
            // card must still take the same time to play a sound, or
            // everything a game paces against its own music drifts.
            if (bus.spu.tick()) {
                bus.irq.raise(Interrupt::Spu);
            }
            scheduler.schedule_at(EventKind::Spu,
                                  event->deadline + Spu::TICK_CYCLES);
            break;
        case EventKind::Sio:
            if (bus.sio.deliver_acknowledge()) {
                bus.irq.raise(Interrupt::Controller);
            }
            break;
        case EventKind::Timers:
            bus.timers.advance(scheduler.now, bus.gpu, bus.irq);
            scheduler.schedule_at(EventKind::Timers,
                                  event->deadline + Timers::TICK_CYCLES);
            break;
        case EventKind::Count:
            break;  // sentinel, never returned
        }
    }
}

u32 Console::step()
{
    const u32 cycles = cpu.step();
    scheduler.advance(cycles);
    dispatch_due_events();
    return cycles;
}

void Console::run_cycles(u64 cycles)
{
    const u64 end = scheduler.now + cycles;
    while (scheduler.now < end && !cpu.halted) {
        const u64 deadline = std::min(end, scheduler.next_deadline());
        while (scheduler.now < deadline && !cpu.halted) {
            scheduler.advance(cpu.step());
        }
        dispatch_due_events();
    }
}

void Console::visit_state(State& state)
{
    cpu.visit_state(state);
    bus.visit_state(state);
    scheduler.visit_state(state);
    state(frames);
}

std::vector<u8> Console::save_state()
{
    State state;
    state.saving = true;

    u32 magic = State::MAGIC;
    u32 version = State::VERSION;
    state(magic);
    state(version);
    visit_state(state);

    return std::move(state.bytes);
}

bool Console::load_state(const std::vector<u8>& bytes)
{
    State state;
    state.saving = false;
    state.bytes = bytes;

    u32 magic = 0;
    u32 version = 0;
    state(magic);
    state(version);
    if (!state.ok || magic != State::MAGIC || version != State::VERSION) {
        return false;
    }

    // Restoring into a live machine: if the traversal runs short the
    // console is left half-loaded, which is why the header is checked
    // before any of it is touched.
    visit_state(state);
    return state.ok;
}
