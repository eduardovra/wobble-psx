#pragma once

#include <array>
#include <string>

#include "types.h"

struct Bus {
    static constexpr u32 RAM_SIZE = 2 * 1024 * 1024;
    static constexpr u32 BIOS_SIZE = 512 * 1024;

    bool load_bios(const std::string& path);

    u8 read8(u32 addr) const;
    u16 read16(u32 addr) const;
    u32 read32(u32 addr) const;
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    std::array<u8, RAM_SIZE> ram{};
    std::array<u8, BIOS_SIZE> bios{};
};
