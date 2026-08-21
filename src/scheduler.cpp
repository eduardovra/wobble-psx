#include "scheduler.h"

#include <algorithm>

#include "savestate.h"

void Scheduler::reset()
{
    now = 0;
    deadlines.fill(NEVER);
}

void Scheduler::schedule_at(EventKind kind, u64 timestamp)
{
    deadlines[static_cast<std::size_t>(kind)] = timestamp;
}

void Scheduler::schedule_in(EventKind kind, u64 delay)
{
    schedule_at(kind, now + delay);
}

void Scheduler::cancel(EventKind kind)
{
    deadlines[static_cast<std::size_t>(kind)] = NEVER;
}

u64 Scheduler::next_deadline() const
{
    u64 earliest = NEVER;
    for (const u64 deadline : deadlines) {
        earliest = std::min(earliest, deadline);
    }
    return earliest;
}

u64 Scheduler::next_deadline_for(EventKind kind) const
{
    return deadlines[static_cast<std::size_t>(kind)];
}

std::optional<DueEvent> Scheduler::next_due()
{
    std::size_t due = EVENT_COUNT;
    for (std::size_t i = 0; i < deadlines.size(); i++) {
        if (deadlines[i] > now) {
            continue;
        }
        // Fire in chronological order, not enum order, so devices see
        // the same sequence they would on hardware.
        if (due == EVENT_COUNT || deadlines[i] < deadlines[due]) {
            due = i;
        }
    }

    if (due == EVENT_COUNT) {
        return std::nullopt;
    }

    const u64 deadline = deadlines[due];
    deadlines[due] = NEVER;
    return DueEvent{.kind = static_cast<EventKind>(due), .deadline = deadline};
}

void Scheduler::visit_state(State& state)
{
    state(now);
    state(deadlines);
}
