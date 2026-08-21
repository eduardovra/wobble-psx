#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_set>

#include "cdrom.h"
#include "dma.h"
#include "gpu.h"
#include "irq.h"
#include "mdec.h"
#include "memctrl.h"
#include "scheduler.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include "types.h"

struct Debugger;
struct State;

// Everything the CPU can address. There is no MMU on the R3000A: an
// address is decoded straight to a device by its numeric range, which
// makes this the map of the whole machine.
//
//   0x00000000  2 MB   RAM
//   0x1F000000  512 KB expansion 1 (cartridge port, usually empty)
//   0x1F800000  1 KB   scratchpad (the data cache used as fast RAM)
//   0x1F801000  8 KB   hardware registers — GPU, SPU, DMA, timers…
//   0x1FC00000  512 KB BIOS ROM
//
// So far RAM, the scratchpad, the BIOS, the interrupt controller, the
// GPU's ports, the DMA controller, the CD-ROM, the controller port, the
// SPU and the MDEC are real; the rest of the register range reads as
// zero and swallows writes.
struct Bus {
    // The clock comes in from outside because the bus does not own it:
    // the CPU drives it forward, and the devices behind here are timed
    // against it. A counter software reads has to answer for the
    // instant of the read, which means the read path needs the time.
    explicit Bus(Scheduler& scheduler) : scheduler(scheduler) { }

    static constexpr u32 RAM_SIZE = 2 * 1024 * 1024;
    static constexpr u32 BIOS_SIZE = 512 * 1024;

    // The scratchpad: the data cache the R3000A would have had, wired
    // up as a kilobyte of directly addressed fast memory instead.
    // Games use it for whatever they touch most in an inner loop, so a
    // machine without it does not run slowly — it runs wrong.
    static constexpr u32 SCRATCHPAD_START = 0x1F800000;
    static constexpr u32 SCRATCHPAD_SIZE = 1024;

    // What a load costs, by region, in CPU cycles — the figures
    // ps1-tests' cpu/access-time reads back off a console, which is
    // also what grades any change to them. Each includes the one cycle
    // the instruction takes to issue, so the stall charged on top of
    // that is the figure minus one.
    //
    // There is no data cache on the PSX (the SRAM that would be one is
    // the scratchpad), so every load pays the full price and the
    // cached and uncached windows onto RAM cost the same. That makes a
    // load several times more expensive than an ALU instruction, which
    // is most of the difference between this machine's speed and the
    // one instruction per cycle it used to run at.
    static constexpr u32 IO_LOAD_CYCLES = 3;   // one shared decoder
    static constexpr u32 RAM_LOAD_CYCLES = 5;  // plus DRAM refresh

    // The BIOS, the CD-ROM, the SPU and the three expansion windows
    // are not here: what they cost is worked out from the
    // memory-control registers, which say how long each of them holds
    // the bus for and how wide its data lines are. See memctrl.h.

    // Reads the whole 512 KB image; false if it is missing or short.
    bool load_bios(const std::string& path);

    void visit_state(State& state);

    // Reads and writes take virtual addresses, as the CPU sees them.
    //
    // Reads are not const: reading a hardware register is an event the
    // device sees, and some of them answer differently next time
    // because of it. GPUREAD hands back the next pixel of a transfer
    // and steps past it, which a const read could not do.
    //
    // A write takes the whole register the store came from rather than
    // the byte or halfword its width names. Memory keeps the part that
    // fits, but a hardware register is on the far side of a 32-bit bus
    // that carries all of it whatever the width was, and most devices
    // do not read the byte enables that say which part was meant: a
    // byte written to the DMA controller replaces the register it
    // lands in, all four bytes of it. So the width is passed down
    // beside the value and each device decides how much to take.
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);

    // The same word back as read32, billed for `bytes` of it: what an
    // unaligned load's two halves each read.
    u32 read32_partial(u32 addr, u32 bytes);
    void write8(u32 addr, u32 value);
    void write16(u32 addr, u32 value);
    void write32(u32 addr, u32 value);

    // Reads the instruction at `addr`, or nothing when there is no
    // instruction there to fetch. Hardware answers a fetch from a
    // region no memory is mapped to — the gaps between the hardware
    // registers included — with a bus error, and the scratchpad is
    // refused whatever is in it: it is the data cache wired up as
    // memory, and code cannot be run from it at all. Saying so is the
    // point of this being separate from read32, which has to answer
    // something.
    std::optional<u32> fetch(u32 addr);

    // Side-effect-free accessors for a debugger looking at memory.
    // Reading a hardware register is an event the device reacts to,
    // so a debugger must never do it by accident — these answer only
    // for RAM and the BIOS, and refuse everything else rather than
    // guess. Writes reach RAM alone; the BIOS is ROM.
    std::optional<u8> peek8(u32 addr) const;
    bool poke8(u32 addr, u8 value);

    // Decode of the hardware register range, shared by all three
    // access widths. Returns whether a device claimed the address,
    // leaving `value` untouched when none did.
    //
    // Only the exact register address matches, so a narrow access to
    // the middle of a register falls through to the unimplemented
    // default instead of returning a shifted value. Nothing does that
    // to the registers implemented so far.
    //
    // `width` is in bytes, and matters to one device: the SPU's
    // registers are sixteen bits each, so a word access to it is two
    // registers rather than one wide one, and the pair that starts a
    // voice is written both halves at once.
    bool read_io(u32 phys, u32& value, u32 width);
    bool write_io(u32 phys, u32 value, u32 width);

    u32 read_word(u32 addr, u32 billed_bytes);

    // How long a load of `bytes` bytes from `phys` stalls the CPU,
    // over and above the one cycle the instruction itself costs.
    u32 load_stall(u32 phys, u32 bytes) const;

    // Cycles the accesses made so far have stalled the CPU for, over
    // and above the one cycle the instruction itself costs. The CPU
    // clears it each step and bills whatever is left at the end, which
    // keeps the region timings here, next to the decode that knows
    // which region an address is in.
    u32 stall_cycles = 0;

    // The clock through which the DMA controller has the bus to
    // itself. There is one bus and one set of address lines: while a
    // channel is reading or writing RAM the CPU cannot fetch or load,
    // so it waits. That waiting is what makes a transfer cost the
    // program that started it anything at all — without it the words
    // would move for free beside a CPU that never noticed.
    u64 dma_hold_until = 0;

    // How long the CPU must wait before its next instruction, because
    // the controller still has the bus. Nought once the hold has
    // passed, and nought again the moment it has been charged, since
    // charging it is what moves the clock past it.
    u32 dma_stall() const;

    // Records an address that no device claimed, and reports whether
    // it had not been seen before. An unimplemented register is
    // usually polled in a loop, so logging every access buries the
    // information and costs a formatted string and a write each time
    // round; saying it once turns the log into a list of what is
    // still missing.
    //
    // Keyed on the address alone: a read and a write to the same
    // register are one missing device, not two.
    bool note_unhandled(u32 addr);

    // Watches how hard a hardware register is being read, and says so
    // when one of them is being read to the exclusion of all other
    // work. Software on this machine waits by reading a register until
    // a bit flips, so a register read half a million times in a second
    // is the shape a stalled guest has: no fault, no unimplemented
    // anything, just a program that is never going to be let go.
    //
    // It is what a hang looks like from outside, and without it a hang
    // looks like nothing at all — the emulator runs on and reports a
    // clean run while the program inside it has stopped.
    void note_poll(u32 addr, u32 value);

    std::array<u8, RAM_SIZE> ram{};
    std::array<u8, BIOS_SIZE> bios{};
    std::array<u8, SCRATCHPAD_SIZE> scratchpad{};

    Scheduler& scheduler;

    Irq irq;
    Gpu gpu;
    Dma dma;
    CdRom cdrom;
    Sio sio;
    Spu spu;
    Timers timers;
    Mdec mdec;
    MemControl memctrl;

    // Bounded by the number of distinct unhandled addresses a game
    // actually touches, which is small.
    std::unordered_set<u32> reported_addresses;

    // The register note_poll is counting. One slot rather than a count
    // per address: a wait loop reads one register, and the handful of
    // reads an interrupt makes in between are a different address and
    // leave the count where it was.
    struct Poll {
        u32 address = 0;
        u32 value = 0;  // its last answer, for the report
        u64 since = 0;  // the cycle this window opened at
        u64 reads = 0;
        bool reported = false;
    };
    Poll poll;

    // Set while a debugger wants to see memory accesses, and null the
    // rest of the time. A null check on the access paths is a branch
    // the predictor gets right every time, so the cost when nothing is
    // attached is nil — which matters, because this sits in the
    // hottest code the emulator has.
    Debugger* debug = nullptr;
};
