#include "dma.h"

#include <format>

#include "bus.h"
#include "log.h"
#include "savestate.h"

namespace {

// CHCR bits.
constexpr u32 CHCR_TO_DEVICE = 1 << 0;
constexpr u32 CHCR_DECREASING = 1 << 1;
constexpr u32 CHCR_SYNC_MODE = 0x600;  // bits 10..9
constexpr u32 CHCR_ENABLE = 1 << 24;
constexpr u32 CHCR_TRIGGER = 1 << 28;

// DICR bits. Everything else is either unused or the computed top bit.
constexpr u32 DICR_WRITABLE = 0x00FF803F;
constexpr u32 DICR_FORCE = 1 << 15;
constexpr u32 DICR_MASTER_ENABLE = 1 << 23;
constexpr u32 DICR_ENABLE_SHIFT = 16;
constexpr u32 DICR_FLAG_SHIFT = 24;
constexpr u32 DICR_CHANNEL_MASK = 0x7F;
constexpr u32 DICR_ACTIVE = 1u << 31;

// Register offsets within the controller's range.
constexpr u32 CHANNEL_STRIDE = 0x10;
constexpr u32 CHANNEL_REGION_END = 0x70;
constexpr u32 DPCR_OFFSET = 0x70;
constexpr u32 DICR_OFFSET = 0x74;

// A DMA address is a RAM address: the top bits are ignored and the low
// two are dropped, so a transfer always stays word-aligned and inside
// the 2 MB whatever software put in MADR.
constexpr u32 RAM_ADDRESS_MASK = 0x1FFFFC;

// A linked list ends at the first node whose "next" field has this bit
// set. The conventional terminator is 0xFFFFFF, but only the one bit
// is actually tested.
constexpr u32 LIST_END = 0x800000;

// Channel 6 does one thing and has no settings. Its control register
// is mostly not a register at all: the bits that would say which way
// the transfer goes, which way the address moves and how it is paced
// are not wired to anything, and read back as they were built rather
// than as they were written. All that is left is the enable, the
// trigger, and one spare bit that remembers what it is told.
constexpr u32 OTC_CHANNEL = 6;
constexpr u32 OTC_WRITABLE = CHCR_ENABLE | CHCR_TRIGGER | (1u << 30);

// The size field of a linked-list node header: how many words of
// payload follow it.
constexpr u32 LIST_HEADER_WORDS_SHIFT = 24;

// How many nodes a list may have before it is not a list. A node is
// four bytes at least, and they live in the two megabytes of RAM, so a
// walk that visits more nodes than that many has been somewhere twice
// and is going round. Software does write such a chain — by accident,
// or to hold the channel open — and the console just keeps following
// it, transferring nothing and never finishing, while the CPU carries
// on beside it. That cannot be done here, where a transfer runs inside
// the store that started it, so the walk stops instead and leaves the
// channel where the console leaves it: still busy, and with no
// interrupt to say it is done.
constexpr u32 LIST_NODE_LIMIT = 0x200000 / 4;

}  // namespace

Dma::SyncMode Dma::Channel::sync_mode() const
{
    return static_cast<SyncMode>((control & CHCR_SYNC_MODE) >> 9);
}

bool Dma::Channel::to_device() const { return (control & CHCR_TO_DEVICE) != 0; }

bool Dma::Channel::address_decreasing() const
{
    return (control & CHCR_DECREASING) != 0;
}

u32 Dma::Channel::transfer_words() const
{
    const u32 size = block & 0xFFFF;
    if (sync_mode() == SyncMode::Request) {
        const u32 blocks = block >> 16;
        return size * blocks;
    }
    // A count of zero means the largest a 16-bit field can ask for.
    return size == 0 ? 0x10000 : size;
}

bool Dma::Channel::started(bool device_asking) const
{
    if ((control & CHCR_ENABLE) == 0) {
        return false;
    }
    // A Manual transfer goes when software triggers it by hand, or
    // when the device behind the channel asks for the bus on its own
    // — the trigger is how software starts one that nothing is
    // asking for. The other sync modes are paced by the device
    // throughout and go as soon as they are enabled.
    if (sync_mode() == SyncMode::Manual && !device_asking) {
        return (control & CHCR_TRIGGER) != 0;
    }
    return true;
}

void Dma::Channel::finish() { control &= ~(CHCR_ENABLE | CHCR_TRIGGER); }

void Dma::visit_state(State& state)
{
    for (Channel& channel : channels) {
        state(channel.base);
        state(channel.block);
        state(channel.control);
    }
    state(control);
    state(interrupt);
}

void Dma::reset()
{
    channels = {};
    channels[OTC_CHANNEL].control = CHCR_DECREASING;
    control = 0x07654321;
    interrupt = 0;
}

bool Dma::interrupt_active() const
{
    if ((interrupt & DICR_FORCE) != 0) {
        return true;
    }
    if ((interrupt & DICR_MASTER_ENABLE) == 0) {
        return false;
    }
    const u32 enabled = (interrupt >> DICR_ENABLE_SHIFT) & DICR_CHANNEL_MASK;
    const u32 flagged = (interrupt >> DICR_FLAG_SHIFT) & DICR_CHANNEL_MASK;
    return (enabled & flagged) != 0;
}

bool Dma::channel_ready(u32 channel) const
{
    // DPCR gives each channel a nibble, of which bit 3 is its enable.
    const u32 enable = 1u << (channel * 4 + 3);
    if ((control & enable) == 0) {
        return false;
    }
    // Every device here answers the moment it is asked, so the only
    // channel nothing is asking on behalf of is the ordering table's,
    // which has no device behind it at all.
    return channels[channel].started(channel != OTC_CHANNEL);
}

bool Dma::complete(u32 channel)
{
    // The enable bit gates the flag itself, so a transfer that
    // finishes while its channel is switched off leaves nothing
    // behind. Otherwise a game that enables the interrupt afterwards
    // would find the line already up and never see it rise again.
    const u32 enabled = (interrupt >> DICR_ENABLE_SHIFT) & DICR_CHANNEL_MASK;
    if ((enabled & (1u << channel)) == 0) {
        return false;
    }
    const bool was_active = interrupt_active();
    interrupt |= 1u << (DICR_FLAG_SHIFT + channel);
    return !was_active && interrupt_active();
}

u32 Dma::read_register(u32 phys) const
{
    // A read of part of a register is the whole one, moved down to
    // where the bytes asked for start; the caller keeps as many of
    // them as it wanted.
    return whole_register((phys - BASE) & ~3u) >> lane_shift(phys);
}

u32 Dma::whole_register(u32 offset) const
{
    if (offset < CHANNEL_REGION_END) {
        const Channel& channel = channels[offset / CHANNEL_STRIDE];
        switch (offset % CHANNEL_STRIDE) {
        case 0x0:
            return channel.base;
        case 0x4:
            return channel.block;
        case 0x8:
            return channel.control;
        default:
            return 0;
        }
    }

    if (offset == DPCR_OFFSET) {
        return control;
    }
    if (offset == DICR_OFFSET) {
        const u32 active = static_cast<u32>(interrupt_active());
        return (interrupt & ~DICR_ACTIVE) | (active << 31);
    }
    return 0;
}

u32 Dma::lane_shift(u32 phys) { return (phys & 3) * 8; }

Dma::Written Dma::write_register(u32 phys, u32 value)
{
    // A narrower store still drives the whole bus and these registers
    // take all of it — the byte enables go unread — so what lands is
    // the value moved up into the lane its address named, and the
    // bytes beside it are replaced rather than kept.
    const u32 word = value << lane_shift(phys);
    const u32 offset = (phys - BASE) & ~3u;

    if (offset < CHANNEL_REGION_END) {
        const u32 index = offset / CHANNEL_STRIDE;
        Channel& channel = channels[index];
        switch (offset % CHANNEL_STRIDE) {
        case 0x0:
            channel.base = word;
            break;
        case 0x4:
            channel.block = word;
            break;
        case 0x8:
            channel.control = word;
            if (index == OTC_CHANNEL) {
                // Built to walk backwards through a block of RAM, and
                // nothing software writes changes that.
                channel.control = (word & OTC_WRITABLE) | CHCR_DECREASING;
            }
            break;
        default:
            break;
        }
        return {channel_ready(index) ? index : NO_CHANNEL, false};
    }

    if (offset == DPCR_OFFSET) {
        control = word;
        // Switching a channel on can start one that was already
        // waiting with its CHCR bits set.
        for (u32 index = 0; index < CHANNEL_COUNT; index++) {
            if (channel_ready(index)) {
                return {index, false};
            }
        }
        return {};
    }

    if (offset == DICR_OFFSET) {
        const bool was_active = interrupt_active();
        // The per-channel flags are acknowledged by writing a one to
        // them, the opposite of the interrupt controller's I_STAT. Any
        // flag not named in the write stays raised.
        const u32 acknowledged = (word >> DICR_FLAG_SHIFT) & DICR_CHANNEL_MASK;
        u32 flags = (interrupt >> DICR_FLAG_SHIFT) & DICR_CHANNEL_MASK;
        flags &= ~acknowledged;
        interrupt = (word & DICR_WRITABLE) | (flags << DICR_FLAG_SHIFT);
        // Forcing the top bit, or enabling a channel that has already
        // flagged, raises the line as much as a transfer finishing
        // does.
        return {NO_CHANNEL, !was_active && interrupt_active()};
    }
    return {};
}

namespace {

// The word a device hands over when RAM is the destination.
u32 read_from_device(Bus& bus, u32 channel, u32 address, u32 remaining)
{
    switch (static_cast<Dma::Port>(channel)) {
    case Dma::Port::MdecOut:
        return bus.mdec.read_data();
    case Dma::Port::Otc:
        // Channel 6 has no device behind it. The controller builds an
        // ordering table: a chain running backwards through RAM, each
        // entry holding the address of the one before it, ending at a
        // marker. It exists because clearing the table is the one
        // thing every frame starts with.
        if (remaining == 1) {
            return 0xFFFFFF;
        }
        return (address - 4) & RAM_ADDRESS_MASK;
    case Dma::Port::Gpu:
        return bus.gpu.read();
    case Dma::Port::Spu:
        return bus.spu.read_dma();
    case Dma::Port::CdRom: {
        // The same FIFO the CPU would read a byte at a time, taken
        // four bytes to the word, least significant first. The drive
        // is what limits how much is there: a channel asked for more
        // than the sector holds reads zeroes off the end of it.
        u32 word = 0;
        for (u32 i = 0; i < 4; i++) {
            word |= u32{bus.cdrom.read_data()} << (i * 8);
        }
        return word;
    }
    default:
        return 0;
    }
}

void write_to_device(Bus& bus, u32 channel, u32 word)
{
    switch (static_cast<Dma::Port>(channel)) {
    case Dma::Port::MdecIn:
        bus.mdec.write_data(word);
        break;
    case Dma::Port::Gpu:
        bus.gpu.write_gp0(word);
        break;
    case Dma::Port::Spu:
        bus.spu.write_dma(word);
        break;
    default:
        // The rest do not exist yet, so their words go nowhere. The
        // transfer still completes, which keeps software from waiting
        // on a channel that would never finish.
        break;
    }
}

void report_unserved(Bus& bus, u32 channel)
{
    const auto port = static_cast<Dma::Port>(channel);
    if (port == Dma::Port::Gpu || port == Dma::Port::Otc ||
        port == Dma::Port::CdRom || port == Dma::Port::Spu ||
        port == Dma::Port::MdecIn || port == Dma::Port::MdecOut) {
        return;
    }
    if (bus.note_unhandled(Dma::BASE + channel)) {
        log_message(std::format("dma: channel {} has no device", channel));
    }
}

u32 read_ram(const Bus& bus, u32 address)
{
    const u32 offset = address & RAM_ADDRESS_MASK;
    return static_cast<u32>(bus.ram[offset]) |
        (static_cast<u32>(bus.ram[offset + 1]) << 8) |
        (static_cast<u32>(bus.ram[offset + 2]) << 16) |
        (static_cast<u32>(bus.ram[offset + 3]) << 24);
}

void write_ram(Bus& bus, u32 address, u32 value)
{
    const u32 offset = address & RAM_ADDRESS_MASK;
    bus.ram[offset] = static_cast<u8>(value);
    bus.ram[offset + 1] = static_cast<u8>(value >> 8);
    bus.ram[offset + 2] = static_cast<u8>(value >> 16);
    bus.ram[offset + 3] = static_cast<u8>(value >> 24);
}

// A Manual or Request transfer: a fixed number of words, one step at a
// time, in whichever direction the channel was set up for.
void run_block(Bus& bus, u32 channel)
{
    const Dma::Channel& settings = bus.dma.channels[channel];
    const u32 step = settings.address_decreasing() ? 0u - 4u : 4u;

    u32 address = settings.base & RAM_ADDRESS_MASK;
    u32 remaining = settings.transfer_words();

    while (remaining > 0) {
        if (settings.to_device()) {
            write_to_device(bus, channel, read_ram(bus, address));
        } else {
            const u32 word = read_from_device(bus, channel, address, remaining);
            write_ram(bus, address, word);
        }
        address = (address + step) & RAM_ADDRESS_MASK;
        remaining--;
    }
}

// A linked list: RAM holds a chain of packets, each a header word
// giving its length and the address of the next, followed by that many
// words for the device. It is how a frame's worth of GPU commands is
// handed over in one go, assembled in whatever order suits the game
// and chained into the order the GPU should see.
//
// Reports whether it reached the end of the chain. A chain with no end
// is the one case where it does not.
bool run_linked_list(Bus& bus, u32 channel)
{
    const Dma::Channel& settings = bus.dma.channels[channel];
    u32 address = settings.base & RAM_ADDRESS_MASK;

    for (u32 node = 0; node < LIST_NODE_LIMIT; node++) {
        const u32 header = read_ram(bus, address);
        u32 words = header >> LIST_HEADER_WORDS_SHIFT;
        while (words > 0) {
            address = (address + 4) & RAM_ADDRESS_MASK;
            write_to_device(bus, channel, read_ram(bus, address));
            words--;
        }
        if ((header & LIST_END) != 0) {
            return true;
        }
        address = header & RAM_ADDRESS_MASK;
    }
    return false;
}

}  // namespace

void run_dma(Bus& bus, u32 channel)
{
    report_unserved(bus, channel);

    if (bus.dma.channels[channel].sync_mode() == Dma::SyncMode::LinkedList) {
        if (!run_linked_list(bus, channel)) {
            // Still going, as far as software can see. Nothing clears
            // the enable bit and nothing raises the interrupt; the
            // channel is left running until something writes it off.
            return;
        }
    } else {
        run_block(bus, channel);
    }

    bus.dma.channels[channel].finish();
    if (bus.dma.complete(channel)) {
        bus.irq.raise(Interrupt::Dma);
    }
}
