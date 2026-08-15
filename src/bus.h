#pragma once

#include <array>
#include <string>

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
// So far only RAM and the BIOS are real; the register range reads as
// zero and swallows writes, which is enough to get the BIOS booting.
struct Bus {
    static constexpr u32 RAM_SIZE = 2 * 1024 * 1024;
    static constexpr u32 BIOS_SIZE = 512 * 1024;

    // Reads the whole 512 KB image; false if it is missing or short.
    bool load_bios(const std::string& path);

    // Reads and writes take virtual addresses, as the CPU sees them.
    u8 read8(u32 addr) const;
    u16 read16(u32 addr) const;
    u32 read32(u32 addr) const;
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    std::array<u8, RAM_SIZE> ram{};
    std::array<u8, BIOS_SIZE> bios{};
};
