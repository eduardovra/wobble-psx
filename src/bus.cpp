#include "bus.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <fstream>

namespace {

// Top three address bits select the memory region; masking them off
// yields the physical address (KSEG0/KSEG1 mirror the same memory).
constexpr u32 REGION_MASK[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,  // KUSEG
    0x7FFFFFFF,                                      // KSEG0
    0x1FFFFFFF,                                      // KSEG1
    0xFFFFFFFF, 0xFFFFFFFF,                          // KSEG2
};

constexpr u32 BIOS_START = 0x1FC00000;
constexpr u32 IO_START = 0x1F801000;
constexpr u32 IO_END = 0x1F803000;
constexpr u32 CACHE_CONTROL = 0xFFFE0130;

u32 to_physical(u32 addr)
{
    return addr & REGION_MASK[addr >> 29];
}

template <typename T, std::size_t N>
T read_from(const std::array<u8, N>& mem, u32 offset)
{
    T value;
    std::memcpy(&value, mem.data() + offset, sizeof(T));
    return value;
}

template <typename T, std::size_t N>
void write_to(std::array<u8, N>& mem, u32 offset, T value)
{
    std::memcpy(mem.data() + offset, &value, sizeof(T));
}

}  // namespace

bool Bus::load_bios(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(bios.data()), bios.size());
    return file.gcount() == static_cast<std::streamsize>(bios.size());
}

u32 Bus::read32(u32 addr) const
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        return read_from<u32>(ram, phys);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u32>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        return 0;  // I/O not implemented yet
    }
    SDL_Log("bus: unhandled read32 at %08X", addr);
    return 0;
}

u16 Bus::read16(u32 addr) const
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        return read_from<u16>(ram, phys);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u16>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        return 0;
    }
    SDL_Log("bus: unhandled read16 at %08X", addr);
    return 0;
}

u8 Bus::read8(u32 addr) const
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        return ram[phys];
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return bios[phys - BIOS_START];
    }
    if (phys >= IO_START && phys < IO_END) {
        return 0;
    }
    SDL_Log("bus: unhandled read8 at %08X", addr);
    return 0;
}

void Bus::write32(u32 addr, u32 value)
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        write_to(ram, phys, value);
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        return;  // I/O not implemented yet
    }
    if (addr == CACHE_CONTROL) {
        return;
    }
    SDL_Log("bus: unhandled write32 at %08X = %08X", addr, value);
}

void Bus::write16(u32 addr, u16 value)
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        write_to(ram, phys, value);
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        return;
    }
    SDL_Log("bus: unhandled write16 at %08X = %04X", addr, value);
}

void Bus::write8(u32 addr, u8 value)
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        ram[phys] = value;
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        return;
    }
    SDL_Log("bus: unhandled write8 at %08X = %02X", addr, value);
}
