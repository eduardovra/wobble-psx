#include "dma.h"

#include <format>

#include "bus.h"
#include "log.h"

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

// The size field of a linked-list node header: how many words of
// payload follow it.
constexpr u32 LIST_HEADER_WORDS_SHIFT = 24;

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

bool Dma::Channel::started() const
{
    if ((control & CHCR_ENABLE) == 0) {
        return false;
    }
    // Only a Manual transfer waits for its trigger; the others are
    // paced by the device and go as soon as they are enabled.
    if (sync_mode() == SyncMode::Manual) {
        return (control & CHCR_TRIGGER) != 0;
    }
    return true;
}

void Dma::Channel::finish() { control &= ~(CHCR_ENABLE | CHCR_TRIGGER); }

void Dma::reset()
{
    channels = {};
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
    return channels[channel].started();
}

bool Dma::complete(u32 channel)
{
    const bool was_active = interrupt_active();
    interrupt |= 1u << (DICR_FLAG_SHIFT + channel);
    return !was_active && interrupt_active();
}

u32 Dma::read_register(u32 phys) const
{
    const u32 offset = phys - BASE;

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

u32 Dma::write_register(u32 phys, u32 value)
{
    const u32 offset = phys - BASE;

    if (offset < CHANNEL_REGION_END) {
        const u32 index = offset / CHANNEL_STRIDE;
        Channel& channel = channels[index];
        switch (offset % CHANNEL_STRIDE) {
        case 0x0:
            channel.base = value;
            break;
        case 0x4:
            channel.block = value;
            break;
        case 0x8:
            channel.control = value;
            break;
        default:
            break;
        }
        return channel_ready(index) ? index : NO_CHANNEL;
    }

    if (offset == DPCR_OFFSET) {
        control = value;
        // Switching a channel on can start one that was already
        // waiting with its CHCR bits set.
        for (u32 index = 0; index < CHANNEL_COUNT; index++) {
            if (channel_ready(index)) {
                return index;
            }
        }
        return NO_CHANNEL;
    }

    if (offset == DICR_OFFSET) {
        // The per-channel flags are acknowledged by writing a one to
        // them, the opposite of the interrupt controller's I_STAT. Any
        // flag not named in the write stays raised.
        const u32 acknowledged = (value >> DICR_FLAG_SHIFT) & DICR_CHANNEL_MASK;
        u32 flags = (interrupt >> DICR_FLAG_SHIFT) & DICR_CHANNEL_MASK;
        flags &= ~acknowledged;
        interrupt = (value & DICR_WRITABLE) | (flags << DICR_FLAG_SHIFT);
    }
    return NO_CHANNEL;
}

namespace {

// The word a device hands over when RAM is the destination.
u32 read_from_device(Bus& bus, u32 channel, u32 address, u32 remaining)
{
    switch (static_cast<Dma::Port>(channel)) {
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
    default:
        return 0;
    }
}

void write_to_device(Bus& bus, u32 channel, u32 word)
{
    if (static_cast<Dma::Port>(channel) == Dma::Port::Gpu) {
        bus.gpu.write_gp0(word);
    }
    // Other devices do not exist yet, so their words go nowhere. The
    // transfer still completes, which keeps software from waiting on a
    // channel that would never finish.
}

void report_unserved(Bus& bus, u32 channel)
{
    const auto port = static_cast<Dma::Port>(channel);
    if (port == Dma::Port::Gpu || port == Dma::Port::Otc) {
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
void run_linked_list(Bus& bus, u32 channel)
{
    const Dma::Channel& settings = bus.dma.channels[channel];
    u32 address = settings.base & RAM_ADDRESS_MASK;

    while (true) {
        const u32 header = read_ram(bus, address);
        u32 words = header >> LIST_HEADER_WORDS_SHIFT;
        while (words > 0) {
            address = (address + 4) & RAM_ADDRESS_MASK;
            write_to_device(bus, channel, read_ram(bus, address));
            words--;
        }
        if ((header & LIST_END) != 0) {
            return;
        }
        address = header & RAM_ADDRESS_MASK;
    }
}

}  // namespace

void run_dma(Bus& bus, u32 channel)
{
    report_unserved(bus, channel);

    if (bus.dma.channels[channel].sync_mode() == Dma::SyncMode::LinkedList) {
        run_linked_list(bus, channel);
    } else {
        run_block(bus, channel);
    }

    bus.dma.channels[channel].finish();
    if (bus.dma.complete(channel)) {
        bus.irq.raise(Interrupt::Dma);
    }
}
