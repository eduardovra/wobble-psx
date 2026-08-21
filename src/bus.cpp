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
constexpr u32 IO_END = 0x1F802000;
constexpr u32 EXPANSION1_START = 0x1F000000;
constexpr u32 EXPANSION1_END = 0x1F080000;
// Expansion region 2: the debug port, where a development board's
// serial lines and the POST register live. Nothing is plugged in
// here, so the bus floats and every read comes back all ones.
constexpr u32 EXPANSION2_START = 0x1F802000;
constexpr u32 EXPANSION2_END = 0x1F803000;
// Expansion region 3: the multitap and boot-ROM port. Empty here too,
// and on every retail machine.
constexpr u32 EXPANSION3_START = 0x1FA00000;
constexpr u32 EXPANSION3_END = 0x1FC00000;
// Sits in KSEG2 and so is matched on the virtual address, unmasked.
constexpr u32 CACHE_CONTROL = 0xFFFE0130;

bool in_scratchpad(u32 phys)
{
    return phys >= Bus::SCRATCHPAD_START &&
        phys < Bus::SCRATCHPAD_START + Bus::SCRATCHPAD_SIZE;
}

bool in_expansion1(u32 phys)
{
    return phys >= EXPANSION1_START && phys < EXPANSION1_END;
}

bool in_expansion2(u32 phys)
{
    return phys >= EXPANSION2_START && phys < EXPANSION2_END;
}

bool in_expansion3(u32 phys)
{
    return phys >= EXPANSION3_START && phys < EXPANSION3_END;
}

// All three expansion windows are empty on a retail machine, and an
// empty window is not silence: nothing drives the data lines, so they
// stay at the pull-ups and a read comes back all ones.
bool in_expansion(u32 phys)
{
    return in_expansion1(phys) || in_expansion2(phys) || in_expansion3(phys);
}

// Shifting an address right by 29 leaves its top 3 bits: the region
// index. Since this emulator has no caches, translating to a physical
// address is all the region distinction amounts to here.
u32 to_physical(u32 addr) { return addr & REGION_MASK[addr >> 29]; }

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
    // could have changed. reported_addresses and the poll count are
    // left out for the same reason in reverse — they are the
    // emulator's own bookkeeping, not anything the console has.
    state(ram);
    state(scratchpad);
    irq.visit_state(state);
    gpu.visit_state(state);
    dma.visit_state(state);
    cdrom.visit_state(state);
    sio.visit_state(state);
    spu.visit_state(state);
    timers.visit_state(state);
    mdec.visit_state(state);
    memctrl.visit_state(state);
}

bool Bus::read_io(u32 phys, u32& value, u32 width)
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
    // The CD-ROM controller's four registers are bytes rather than
    // words: each address in its range is its own register, and the
    // controller drives that one byte onto every lane of the bus. So a
    // wider read finds it repeated rather than finding the registers
    // beside it — a word read of the status register answers 1A1A1A1Ah.
    if (phys >= CdRom::BASE && phys < CdRom::END) {
        const u32 byte = cdrom.read_register(phys) & 0xFF;
        value = 0;
        for (u32 lane = 0; lane < width; lane++) {
            value |= byte << (lane * 8);
        }
        return true;
    }
    if (phys >= Sio::BASE && phys < Sio::END) {
        value = sio.read_register(phys);
        return true;
    }
    if (phys >= Timers::BASE && phys < Timers::END) {
        value = timers.read_register(phys, scheduler.now, gpu, irq);
        return true;
    }
    if (phys >= Spu::BASE && phys < Spu::END) {
        value = spu.read_register(phys);
        if (width == 4) {
            value |= u32{spu.read_register(phys + 2)} << 16;
        }
        return true;
    }
    if (phys >= Mdec::BASE && phys < Mdec::END) {
        value = mdec.read_register(phys);
        return true;
    }
    if (phys >= MemControl::BASE && phys < MemControl::END) {
        value = memctrl.read_register(phys);
        return true;
    }
    return false;
}

bool Bus::write_io(u32 phys, u32 value, u32 width)
{
    switch (phys) {
    case Irq::STATUS:
        irq.acknowledge(static_cast<u16>(value));
        return true;
    case Irq::MASK:
        irq.mask = static_cast<u16>(value) & Irq::LINES;
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
        const Dma::Written written = dma.write_register(phys, value);
        if (written.interrupt) {
            irq.raise(Interrupt::Dma);
        }
        if (written.channel != Dma::NO_CHANNEL) {
            // The whole transfer happens inside this store, and costs
            // it nothing: run_dma reaches RAM directly rather than
            // through the read paths above, so none of its accesses
            // are billed. DMA does take the bus away from the CPU on
            // hardware, but charging that means running the transfer
            // on the scheduler rather than all at once here.
            run_dma(*this, written.channel);
        }
        return true;
    }
    if (phys >= CdRom::BASE && phys < CdRom::END) {
        // The other side of that byte-wide bus: a wider write reaches
        // the register one byte at a time, so what stays behind is the
        // byte the access ends with rather than the one at the bottom.
        const u32 last_lane = (width - 1) * 8;
        cdrom.write_register(phys, static_cast<u8>(value >> last_lane));
        return true;
    }
    if (phys >= Sio::BASE && phys < Sio::END) {
        // A byte written to the port is shifted out and the device's
        // answer shifted back inside the same store; the acknowledge
        // that follows is a separate event, and has to be, because the
        // driver clears the last one in between.
        if (sio.write_register(phys, value)) {
            scheduler.schedule_at(EventKind::Sio,
                                  scheduler.now + sio.acknowledge_delay());
        }
        return true;
    }
    if (phys >= Timers::BASE && phys < Timers::END) {
        timers.write_register(phys, value, scheduler.now, gpu, irq);
        return true;
    }
    if (phys >= Spu::BASE && phys < Spu::END) {
        spu.write_register(phys, static_cast<u16>(value));
        if (width == 4) {
            spu.write_register(phys + 2, static_cast<u16>(value >> 16));
        }
        if (spu.interrupt_pending()) {
            irq.raise(Interrupt::Spu);
        }
        return true;
    }
    if (phys >= Mdec::BASE && phys < Mdec::END) {
        mdec.write_register(phys, value);
        return true;
    }
    if (phys >= MemControl::BASE && phys < MemControl::END) {
        memctrl.write_register(phys, value);
        return true;
    }
    return false;
}

std::optional<u32> Bus::fetch(u32 addr)
{
    if (debug != nullptr) {
        debug->note_access(addr, 4, false);
    }
    // The fetch is not billed. There is an instruction cache on the
    // R3000A and none here, so code running from RAM — which is nearly
    // all of it — fetches at one cycle on hardware and would cost seven
    // if this counted it. Charging nothing is the closer of the two
    // answers until the cache is modelled; the price is that the BIOS's
    // own uncached run out of ROM, which nothing times, comes out
    // faster than it really is.
    const u32 phys = to_physical(addr);
    if (phys < RAM_SIZE) {
        return read_from<u32>(ram, phys);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u32>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        // A hardware register is fetched from as readily as it is read
        // from, and the instruction that comes back is whatever the
        // register holds. Software does this on purpose: two words
        // written into a pair of DMA registers and jumped to are a
        // function like any other. The gaps between the registers are
        // not, and fall through to the refusal below.
        u32 value = 0;
        if (read_io(phys, value, 4)) {
            return value;
        }
    }
    return std::nullopt;
}

bool Bus::note_unhandled(u32 addr)
{
    return reported_addresses.insert(addr).second;
}

void Bus::note_poll(u32 addr, u32 value)
{
    // Half a million reads of one register inside a second of console
    // time is a program doing nothing else whatsoever: at four or five
    // instructions to a read there is no room left in the second for
    // anything but the loop. Real waits are shorter than that by
    // orders of magnitude — a DMA channel finishes in microseconds,
    // and a program waiting a whole second for the drive spends it
    // drawing rather than spinning.
    //
    // Counting against a window rather than since the last read of
    // some other register is what makes it hold: a wait loop is
    // interrupted sixty times a second by a handler that reads the
    // interrupt controller, and that must not read as the loop having
    // moved on.
    constexpr u64 STUCK_READS = 500'000;
    constexpr u64 WINDOW = CPU_CLOCK_HZ;

    const bool window_over = scheduler.now - poll.since > WINDOW;
    if (addr != poll.address) {
        // Another register takes the slot only once the one in it has
        // stopped being read, so that the handler's reads above do not
        // evict the loop they interrupted.
        if (poll.reads > 0 && !window_over) {
            return;
        }
        poll = Poll{addr, value, scheduler.now, 0, false};
    } else if (window_over) {
        poll.since = scheduler.now;
        poll.reads = 0;
    }

    poll.value = value;
    poll.reads++;
    if (poll.reads < STUCK_READS || poll.reported) {
        return;
    }
    // Once per stall, not once per read and not once per window: a
    // program that is stuck stays stuck, and the line saying so is
    // worth nothing repeated.
    poll.reported = true;
    log_message(std::format("bus: {:08X} read {} times in a second, "
                            "answering {:08X} — the guest is waiting for "
                            "something that is not coming",
                            poll.address,
                            poll.reads,
                            poll.value));
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

// How long a load from this address stalls the CPU, beyond the one
// cycle the instruction costs anyway. Repeating the range checks the
// read paths make is a few comparisons against keeping the timing
// spread over all nine of their branches.
//
// The six devices on the far side of a chip select are timed by what
// the memory-control registers say they cost, which is where the width
// of the load starts to matter: a byte and a word from RAM cost the
// same, but a word from the CD-ROM is four accesses to an eight-bit
// device and costs four times what a byte does. Everything unclaimed
// reads back as zero without a bus access, so it stalls for nothing.
u32 Bus::load_stall(u32 phys, u32 bytes) const
{
    if (phys < RAM_SIZE) {
        return RAM_LOAD_CYCLES - 1;
    }
    if (phys >= IO_START && phys < IO_END) {
        // The CD-ROM and the SPU sit inside the register range but not
        // on the bus the rest of it is on: they are separate chips
        // with their own chip selects, and cost several times what a
        // register on the main bus does.
        if (phys >= CdRom::BASE && phys < CdRom::END) {
            return memctrl.access_cycles(MemControl::Device::CdRom, bytes) - 1;
        }
        if (phys >= Spu::BASE && phys < Spu::END) {
            return memctrl.access_cycles(MemControl::Device::Spu, bytes) - 1;
        }
        return IO_LOAD_CYCLES - 1;
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return memctrl.access_cycles(MemControl::Device::Bios, bytes) - 1;
    }
    if (in_expansion1(phys)) {
        return memctrl.access_cycles(MemControl::Device::Expansion1, bytes) - 1;
    }
    if (in_expansion2(phys)) {
        return memctrl.access_cycles(MemControl::Device::Expansion2, bytes) - 1;
    }
    if (in_expansion3(phys)) {
        return memctrl.access_cycles(MemControl::Device::Expansion3, bytes) - 1;
    }
    // The cache control register is not on the bus at all — it is
    // inside the CPU — and answers in one cycle for a byte and two for
    // anything wider.
    if (phys == CACHE_CONTROL) {
        return bytes > 1 ? 1 : 0;
    }
    return 0;
}

u32 Bus::read32(u32 addr) { return read_word(addr, 4); }

// LWL and LWR each read the aligned word around their address and keep
// one end of it: two, three or four of its bytes, and between them
// exactly the four of the unaligned word they were written to move. So
// each is billed for the bytes it keeps rather than for a whole word,
// which on a narrow device is most of the price — the pair that reads
// an unaligned word out of the SPU costs what one aligned word costs,
// not twice that.
u32 Bus::read32_partial(u32 addr, u32 bytes) { return read_word(addr, bytes); }

u32 Bus::read_word(u32 addr, u32 billed_bytes)
{
    if (debug != nullptr) {
        debug->note_access(addr, 4, false);
    }
    const u32 phys = to_physical(addr);
    stall_cycles += load_stall(phys, billed_bytes);
    if (phys < RAM_SIZE) {
        return read_from<u32>(ram, phys);
    }
    if (in_scratchpad(phys)) {
        return read_from<u32>(scratchpad, phys - SCRATCHPAD_START);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u32>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        u32 value = 0;
        read_io(phys, value, 4);  // 0 for a device that does not exist yet
        note_poll(phys, value);
        return value;
    }
    if (in_expansion(phys)) {
        return 0xFFFFFFFF;  // nothing on the port: the lines float high
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
    stall_cycles += load_stall(phys, 2);
    if (phys < RAM_SIZE) {
        return read_from<u16>(ram, phys);
    }
    if (in_scratchpad(phys)) {
        return read_from<u16>(scratchpad, phys - SCRATCHPAD_START);
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return read_from<u16>(bios, phys - BIOS_START);
    }
    if (phys >= IO_START && phys < IO_END) {
        u32 value = 0;
        read_io(phys, value, 2);
        note_poll(phys, value);
        return static_cast<u16>(value);
    }
    if (in_expansion(phys)) {
        return 0xFFFF;  // nothing on the port: the lines float high
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
    stall_cycles += load_stall(phys, 1);
    if (phys < RAM_SIZE) {
        return ram[phys];
    }
    if (in_scratchpad(phys)) {
        return scratchpad[phys - SCRATCHPAD_START];
    }
    if (phys >= BIOS_START && phys < BIOS_START + BIOS_SIZE) {
        return bios[phys - BIOS_START];
    }
    if (phys >= IO_START && phys < IO_END) {
        u32 value = 0;
        read_io(phys, value, 1);
        note_poll(phys, value);
        return static_cast<u8>(value);
    }
    if (in_expansion(phys)) {
        return 0xFF;  // nothing on the port: the lines float high
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
    if (in_scratchpad(phys)) {
        return scratchpad[phys - SCRATCHPAD_START];
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
    if (in_scratchpad(phys)) {
        scratchpad[phys - SCRATCHPAD_START] = value;
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
    if (in_scratchpad(phys)) {
        write_to(scratchpad, phys - SCRATCHPAD_START, value);
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        write_io(phys, value, 4);  // dropped if no device claims it yet
        return;
    }
    if (in_expansion2(phys)) {
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

void Bus::write16(u32 addr, u32 value)
{
    if (debug != nullptr) {
        debug->note_access(addr, 2, true);
    }
    const u32 phys = to_physical(addr);
    const u16 half = static_cast<u16>(value);
    if (phys < RAM_SIZE) {
        write_to(ram, phys, half);
        return;
    }
    if (in_scratchpad(phys)) {
        write_to(scratchpad, phys - SCRATCHPAD_START, half);
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        write_io(phys, value, 2);
        return;
    }
    if (in_expansion2(phys)) {
        return;
    }
    if (note_unhandled(addr)) {
        log_message(std::format(
            "bus: unhandled write16 at {:08X} = {:04X}", addr, half));
    }
}

void Bus::write8(u32 addr, u32 value)
{
    if (debug != nullptr) {
        debug->note_access(addr, 1, true);
    }
    const u32 phys = to_physical(addr);
    const u8 byte = static_cast<u8>(value);
    if (phys < RAM_SIZE) {
        ram[phys] = byte;
        return;
    }
    if (in_scratchpad(phys)) {
        scratchpad[phys - SCRATCHPAD_START] = byte;
        return;
    }
    if (phys >= IO_START && phys < IO_END) {
        write_io(phys, value, 1);
        return;
    }
    if (in_expansion2(phys)) {
        return;  // the debug port's POST register lands here
    }
    if (note_unhandled(addr)) {
        log_message(std::format(
            "bus: unhandled write8 at {:08X} = {:02X}", addr, byte));
    }
}
