#pragma once

#include <array>
#include <string>
#include <unordered_set>

#include "dma.h"
#include "gpu.h"
#include "irq.h"
#include "types.h"

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
// So far only RAM, the BIOS and the interrupt controller are real; the
// rest of the register range reads as zero and swallows writes, which
// is enough to get the BIOS booting.
struct Bus {
    static constexpr u32 RAM_SIZE = 2 * 1024 * 1024;
    static constexpr u32 BIOS_SIZE = 512 * 1024;

    // Reads the whole 512 KB image; false if it is missing or short.
    bool load_bios(const std::string& path);

    // Reads and writes take virtual addresses, as the CPU sees them.
    //
    // Reads are not const: reading a hardware register is an event the
    // device sees, and some of them answer differently next time
    // because of it. GPUREAD hands back the next pixel of a transfer
    // and steps past it, which a const read could not do.
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // Decode of the hardware register range, shared by all three
    // access widths. Returns whether a device claimed the address,
    // leaving `value` untouched when none did.
    //
    // Only the exact register address matches, so a narrow access to
    // the middle of a register falls through to the unimplemented
    // default instead of returning a shifted value. Nothing does that
    // to the registers implemented so far.
    bool read_io(u32 phys, u32& value);
    bool write_io(u32 phys, u32 value);

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

    std::array<u8, RAM_SIZE> ram{};
    std::array<u8, BIOS_SIZE> bios{};

    Irq irq;
    Gpu gpu;
    Dma dma;

    // Bounded by the number of distinct unhandled addresses a game
    // actually touches, which is small.
    std::unordered_set<u32> reported_addresses;
};
