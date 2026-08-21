#pragma once

#include <array>

#include "types.h"

struct State;

// The memory-control registers, 0x1F801000 to 0x1F801020. Nine words
// that say where the two expansion windows sit, and — for each of the
// six devices that share the CPU's bus — how long that device holds
// the bus for and how wide its data lines are:
//
//   0x1F801000  expansion 1 base address
//   0x1F801004  expansion 2 base address
//   0x1F801008  expansion 1 delay/size
//   0x1F80100C  expansion 3 delay/size
//   0x1F801010  BIOS ROM delay/size
//   0x1F801014  SPU delay/size
//   0x1F801018  CD-ROM delay/size
//   0x1F80101C  expansion 2 delay/size
//   0x1F801020  the common delays the six share
//
// A delay register holds an access time in cycles, a data bus width,
// and four bits saying which of the common delays that device needs;
// the common register holds those four delays. Together they are what
// makes a load from the CD-ROM cost eight cycles and the same load
// from RAM cost five. The BIOS programmes all of it at boot and a game
// may reprogramme it — several do, to make their own cartridge or the
// CD-ROM answer faster — so the cost of an access is worked out from
// these registers rather than written down as a constant.
//
// The window base and size fields are stored and read back but not
// obeyed: nothing here moves the expansion windows, which sit where
// the BIOS puts them on every retail machine.
struct MemControl {
    static constexpr u32 BASE = 0x1F801000;
    static constexpr u32 END = 0x1F801024;
    static constexpr u32 REGISTER_COUNT = (END - BASE) / 4;

    // The six devices a delay register speaks for, in the order those
    // registers are mapped.
    enum class Device : u32 {
        Expansion1,
        Expansion3,
        Bios,
        Spu,
        CdRom,
        Expansion2,
        Count,
    };

    MemControl() { recalculate(); }

    u32 read_register(u32 phys) const;
    void write_register(u32 phys, u32 value);

    // What a load of `bytes` bytes from `device` costs in CPU cycles,
    // counting the one cycle the load instruction takes anyway. A
    // device narrower than the load answers it in several accesses,
    // which is where most of the difference between one cycle and
    // fifty-six comes from.
    u32 access_cycles(Device device, u32 bytes) const;

    void visit_state(State& state);

    // What the BIOS leaves them at, so that a machine whose registers
    // have not been written yet is timed as the console would be
    // rather than as an infinitely fast bus.
    static constexpr std::array<u32, REGISTER_COUNT> DEFAULTS = {
        0x1F000000,  // expansion 1 base
        0x1F802000,  // expansion 2 base
        0x0013243F,  // expansion 1: 8-bit, slow
        0x00003022,  // expansion 3
        0x0013243F,  // BIOS: 8-bit, four cycles an access
        0x200931E1,  // SPU: 16-bit, and the slowest of them
        0x00020843,  // CD-ROM: 8-bit
        0x00070777,  // expansion 2
        0x00031125,  // the common delays
    };

    // The registers as software wrote them, indexed by their position
    // in the map above.
    std::array<u32, REGISTER_COUNT> registers = DEFAULTS;

private:
    // What one device costs, worked out from its delay register. This
    // is on the load path of every instruction that touches one of
    // them, so it is derived once per write to the registers rather
    // than once per access.
    struct Timing {
        u32 first;       // cycles for the first access of a transfer
        u32 sequential;  // and for each one after it
        u32 bus_shift;   // 0 for an 8-bit bus, 1 for a 16-bit one
    };

    void recalculate();

    std::array<Timing, static_cast<u32>(Device::Count)> timing{};
};
