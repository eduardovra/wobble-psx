#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "spu.h"
#include "types.h"

struct Console;

// Instrumentation for working out what the machine is doing.
//
// The commands are text in and text out, which is the whole point: a
// question about the machine becomes a line of input rather than a
// throwaway program compiled against the emulator. Output is plain
// aligned columns, meant to be read directly and to survive being
// piped through grep and diff.
//
// Nothing here changes what the machine does. Breakpoints and traces
// observe between instructions, and the memory hooks only record —
// so a run under the debugger reaches the same state as one without,
// which is what makes any of it worth trusting.
struct Debugger {
    // Why a run came back.
    enum class Stop : u32 {
        Budget,      // did as much as it was asked to
        Breakpoint,  // reached an address with a breakpoint on it
        Watchpoint,  // touched watched memory
        Target,      // arrived at the address `until` was given
        Halted,      // the CPU stopped on something it cannot do
    };

    // A range of memory to report accesses to. Kept as a range rather
    // than an address because the interesting thing is usually a
    // structure, not a word.
    struct Watch {
        u32 address = 0;
        u32 length = 4;
        bool on_read = true;
        bool on_write = true;
    };

    struct TraceEntry {
        u32 pc = 0;
        u32 instr = 0;
    };

    // Executes one command line and returns what it printed. An
    // unknown command comes back as an error string rather than
    // throwing, so a script runs to its end and reports everything
    // wrong with it in one go.
    std::string execute(Console& console, const std::string& line);

    // Runs instructions until something stops it. `limit` bounds the
    // work; `target` stops on arrival at an address.
    Stop run(Console& console, u64 limit, std::optional<u32> target);

    // Moves whatever the SPU has finished into `recorded`, so a long
    // run is captured rather than only its last fifth of a second.
    void take_recording(Spu& spu);

    // Called by the bus on every access while a debugger is attached.
    // Records the address for the data profile and notes whether it
    // fell in a watched range.
    void note_access(u32 address, u32 length, bool write);

    // The instruction about to run, and whether its fetch has gone
    // past yet. The CPU fetches through the same bus as everything
    // else, so without this the data profile is just the instruction
    // profile again — every address in it would be the code that read
    // it. Only the first read at the current pc is treated as the
    // fetch, so code that genuinely loads from itself still shows up.
    u32 executing_pc = 0;
    bool fetch_pending = false;

    std::vector<u32> breakpoints;
    std::vector<Watch> watchpoints;

    // The last instructions retired, oldest first once dumped. A ring
    // so that leaving it on costs a fixed amount however long a run
    // goes.
    static constexpr std::size_t TRACE_CAPACITY = 4096;
    std::vector<TraceEntry> trace;
    std::size_t trace_next = 0;
    bool trace_enabled = false;

    // Where time went, and what memory it went to. Only collected
    // during `profile`, since counting every instruction is the one
    // thing here that costs enough to notice.
    bool profiling = false;
    std::unordered_map<u32, u64> pc_counts;
    std::unordered_map<u32, u64> data_counts;

    // What the SPU has produced, kept because there is no sound card
    // here to send it to and the machine drops whatever nobody takes.
    // This is to the sound what `screen` is to the picture: the only
    // way to hear a headless run. Off by default — a second of it is
    // 176 KB, and a run nobody is listening to should not pay for it.
    bool recording = false;
    std::vector<Spu::Frame> recorded;

    // Set by note_access when a watched range is touched; the run loop
    // picks it up after the instruction finishes, so the access has
    // completed and its effect can be seen.
    bool watch_hit = false;
    u32 watch_address = 0;
    bool watch_was_write = false;

    u64 instructions_run = 0;
};
