#include "bus.h"

#include <cstring>
#include <format>
#include <fstream>
#include <utility>

#include "debugger.h"
#include "log.h"
#include "savestate.h"

namespace {

// MIPS splits the 4 GB address space into fixed regions, and the PSX
// reaches the same hardware through three of them:
//
//   KUSEG  0x00000000  2 GB  user space, cached
//   KSEG0  0x80000000  512 MB  kernel, cached
//   KSEG1  0xA0000000  512 MB  kernel, uncached
//   KSEG2  0xC0000000  1 GB  kernel, only the cache control register
//
// KSEG0 and KSEG1 are two windows onto the same physical memory, one
// through the cache and one around it — which is why the BIOS runs
// from 0xBFC00000 while its ROM sits at 0x1FC00000. Clearing the top
// bits of the address strips the window and leaves the physical
// address, so the region only decides caching, never what is hit.
//
// Top three address bits select the memory region; masking them off
// yields the physical address (KSEG0/KSEG1 mirror the same memory).
// clang-format off
constexpr u32 REGION_MASK[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,  // KUSEG
    0x7FFFFFFF,                                      // KSEG0
    0x1FFFFFFF,                                      // KSEG1
    0xFFFFFFFF, 0xFFFFFFFF,                          // KSEG2
};
// clang-format on

constexpr u32 BIOS_START = 0x1FC00000;
constexpr u32 IO_START = 0x1F801000;
constexpr u32 IO_END = 0x1F803000;
constexpr u32 EXPANSION1_START = 0x1F000000;
constexpr u32 EXPANSION1_END = 0x1F080000;
// Sits in KSEG2 and so is matched on the virtual address, unmasked.
constexpr u32 CACHE_CONTROL = 0xFFFE0130;

bool in_expansion1(u32 phys)
{
    return phys >= EXPANSION1_START && phys < EXPANSION1_END;
}

// Shifting an address right by 29 leaves its top 3 bits: the region
// index. Since this emulator has no caches, translating to a physical
// address is all the region distinction amounts to here.
u32 to_physical(u32 addr) { return addr & REGION_MASK[addr >> 29]; }

// How long a load from this address stalls the CPU, beyond the one
// cycle the instruction costs anyway. Repeating the range checks the
// read paths make is a few comparisons against keeping the timing
// spread over all nine of their branches.
//
// Everything unclaimed reads back as zero without a bus access, so it
// stalls for nothing. The BIOS figure is for a full word: its ROM is
// on an 8-bit bus, so a byte load is really a quarter of it, which is
// not worth splitting for a region nothing reads data from in anger.
u32 load_stall(u32 phys)
{
    if (phys < Bus::RAM_SIZE) {
        return Bus::RAM_LOAD_CYCLES - 1;
    }
    if (phys >= IO_START && phys < IO_END) {
        return Bus::IO_LOAD_CYCLES - 1;
    }
    if (phys >= BIOS_START && phys < BIOS_START + Bus::BIOS_SIZE) {
        return Bus::BIOS_LOAD_CYCLES - 1;
    }
    return 0;
}

// Memory is kept as raw bytes, so multi-byte accesses go through
// memcpy rather than a reinterpreted pointer, which would break
// strict aliasing. The PSX is little-endian, and so is every host
// this targets, so no byte swapping is needed.
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

void Bus::visit_state(State& state)
{
    // The BIOS image is not saved: it is read-only and comes from the
    // same file either way, so a state carries only what the machine
    // could have changed. reported_addresses is left out for the same
    // reason in reverse — it is the emulator's own bookkeeping, not
    // anything the console has.
    state(ram);
    irq.visit_state(state);
    gpu.visit_state(state);
    dma.visit_state(state);
}

bool Bus::read_io(u32 phys, u32& value)
{
    switch (phys) {
    case Irq::STATUS:
        value = irq.status;
        return true;
    case Irq::MASK:
        value = irq.mask;
        return true;
    case Gpu::GP0:
        value = gpu.read();
        return true;
    case Gpu::GP1:
        value = gpu.status();
        return true;
    default:
        break;
    }

    if (phys >= Dma::BASE && phys < Dma::END) {
        value = dma.read_register(phys);
        return true;
    }
    return false;
}

bool Bus::write_io(u32 phys, u32 value)
{
    switch (phys) {
    case Irq::STATUS:
        irq.acknowledge(static_cast<u16>(value));
        return true;
    case Irq::MASK:
        irq.mask = static_cast<u16>(value);
        return true;
    case Gpu::GP0:
        gpu.write_gp0(value);
        return true;
    case Gpu::GP1:
        gpu.write_gp1(value);
        return true;
    default:
        break;
    }

    if (phys >= Dma::BASE && phys < Dma::END) {
        // A write to CHCR is what starts a transfer, so it happens
        // here, inside the store instruction that asked for it.
        const u32 channel = dma.write_register(phys, value);
        if (channel != Dma::NO_CHANNEL) {
            // The whole transfer happens inside this store, and costs
            // it nothing: run_dma reaches RAM directly rather than
            // through the read paths above, so none of its accesses
            // are billed. DMA does take the bus away from the CPU on
            // hardware, but charging that means running the transfer
            // on the scheduler rather than all at once here.
            run_dma(*this, channel);
        }
        return true;
    }
    return false;
}

bool Bus::note_unhandled(u32 addr)
{
    return reported_addresses.insert(addr).second;
}

bool Bus::load_bios(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(bios.data()),
              static_cast<std::streamsize>(bios.size()));
    return std::cmp_equal(file.gcount(), bios.size());
}

u32 Bus::read32(u32 addr)
{
    if (debug != nullptr) {
        debug->note_access(addr, 4, false);
    }
    const u32 phys = to_physical(addr);
    stall_cycles += load_stall(phys);
    if (phys < RAM_SIZE) {
        return read_from<u32>(ram, phys);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u32>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        u32 value = 0;
        read_io(phys, value);  // 0 for a device that does not exist yet
        return value;
    }
    if (note_unhandled(addr)) {
        log_message(std::format("bus: unhandled read32 at {:08X}", addr));
    }
    return 0;
}

u16 Bus::read16(u32 addr)
{
    if (debug != nullptr) {
        debug->note_access(addr, 2, false);
    }
    const u32 phys = to_physical(addr);
    stall_cycles += load_stall(phys);
    if (phys < RAM_SIZE) {
        return read_from<u16>(ram, phys);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u16>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        u32 value = 0;
        read_io(phys, value);
        return static_cast<u16>(value);
    }
    if (note_unhandled(addr)) {
        log_message(std::format("bus: unhandled read16 at {:08X}", addr));
    }
    return 0;
}

u8 Bus::read8(u32 addr)
{
    if (debug != nullptr) {
        debug->note_access(addr, 1, false);
    }
    const u32 phys = to_physical(addr);
    stall_cycles += load_stall(phys);
    if (phys < RAM_SIZE) {
        return ram[phys];
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return bios[phys - BIOS_START];
    }
    if (phys >= IO_START && phys < IO_END) {
        u32 value = 0;
        read_io(phys, value);
        return static_cast<u8>(value);
    }
    if (in_expansion1(phys)) {
        return 0xFF;  // no expansion device present
    }
    if (note_unhandled(addr)) {
        log_message(std::format("bus: unhandled read8 at {:08X}", addr));
    }
    return 0;
}

std::optional<u8> Bus::peek8(u32 addr) const
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        return ram[phys];
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return bios[phys - BIOS_START];
    }
    return std::nullopt;
}

bool Bus::poke8(u32 addr, u8 value)
{
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        ram[phys] = value;
        return true;
    }
    return false;
}

void Bus::write32(u32 addr, u32 value)
{
    if (debug != nullptr) {
        debug->note_access(addr, 4, true);
    }
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        write_to(ram, phys, value);
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        write_io(phys, value);  // dropped if no device claims it yet
        return;
    }
    if (addr == CACHE_CONTROL) {
        return;
    }
    if (note_unhandled(addr)) {
        log_message(std::format(
            "bus: unhandled write32 at {:08X} = {:08X}", addr, value));
    }
}

void Bus::write16(u32 addr, u16 value)
{
    if (debug != nullptr) {
        debug->note_access(addr, 2, true);
    }
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        write_to(ram, phys, value);
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        write_io(phys, value);
        return;
    }
    if (note_unhandled(addr)) {
        log_message(std::format(
            "bus: unhandled write16 at {:08X} = {:04X}", addr, value));
    }
}

void Bus::write8(u32 addr, u8 value)
{
    if (debug != nullptr) {
        debug->note_access(addr, 1, true);
    }
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        ram[phys] = value;
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        write_io(phys, value);
        return;
    }
    if (note_unhandled(addr)) {
        log_message(std::format(
            "bus: unhandled write8 at {:08X} = {:02X}", addr, value));
    }
}
