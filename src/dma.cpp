#include "dma.h"

#include <algorithm>
#include <array>
#include <format>

#include "bus.h"
#include "log.h"
#include "savestate.h"

namespace {

// CHCR bits.
constexpr u32 CHCR_TO_DEVICE = 1 << 0;
constexpr u32 CHCR_DECREASING = 1 << 1;
constexpr u32 CHCR_CHOPPING = 1 << 8;
constexpr u32 CHCR_SYNC_MODE = 0x600;     // bits 10..9
constexpr u32 CHCR_CHOP_DMA_SHIFT = 16;   // bits 18..16
constexpr u32 CHCR_CHOP_CPU_SHIFT = 20;   // bits 22..20
constexpr u32 CHCR_CHOP_WINDOW_MASK = 7;  // both are three bits wide
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

// MADR is 24 bits wide: it addresses RAM, and two megabytes of it
// need no more. The byte above them does not exist, so an address
// written with one set reads back without it.
constexpr u32 MADR_MASK = 0x00FFFFFF;

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

// What the DMA controller pays for `words` words of RAM. It reads DRAM
// in hyper-page mode — a word a clock, with a row address loaded every
// sixteenth — which is where nocash's "around 17 clks per 16 words"
// comes from, and why DMA reaches RAM several times faster than the
// CPU does.
u32 ram_cycles(u32 words) { return words + (words + 15) / 16; }

// How long the thing on the far side of each channel takes over a
// word, in master clocks. nocash's table, with one correction: it puts
// the SPU at four clocks a word, and spu/memory-transfer measures a
// console taking far longer than that — the note beside the assertion
// says so outright. Forty-eight is a sample period divided by the
// sixteen words of the SPU's transfer FIFO, which is the mechanism the
// measurement is of.
constexpr std::array<u32, Dma::CHANNEL_COUNT> DEVICE_CYCLES_PER_WORD = {
    1,   // MDEC in
    1,   // MDEC out
    1,   // GPU
    24,  // CD-ROM
    48,  // SPU
    20,  // expansion port
    1,   // ordering table: no device, so RAM is the whole cost
};

// What the controller spends between one chop window and the next,
// over and above the CPU window software asked for. dma/chopping puts
// it a shade over five clocks, and steadily so: the figure comes out
// the same across every window size and every CPU window in the log.
constexpr u32 CHOP_OVERHEAD_CYCLES = 5;

// The same, between the blocks of a Request transfer, where the
// channel has to see the device ask again rather than simply carry on.
// The same log puts that nearer ten.
constexpr u32 BLOCK_GAP_CYCLES = 10;

// A linked list is walked a run of nodes at a time rather than one to
// an event, and these are how long a run is and what it costs.
//
// The run matters more than it looks. Between one event and the next
// the CPU always gets an instruction in, because the stall it is
// charged ends exactly where the next event begins — so a node to an
// event would hand a thousand-node ordering table a thousand
// instructions, which is not what a console does with it. The
// controller does not stop between nodes, and neither does this: it
// walks until it has spent LIST_SLICE_CYCLES and only then lets go.
//
// It is not a detail. The BIOS shell sends one frame's table while
// building the next into the same primitives, and a walk that leaves
// the CPU running through it reads a primitive halfway through being
// relinked — length written, pointer not — follows the empty pointer
// to address zero and never comes back. The console wins that race by
// keeping the bus; so does this.
constexpr u32 LIST_FOLLOW_CYCLES = 1;
constexpr u32 LIST_SLICE_CYCLES = 4096;

// What the controller gives back after each run. dma/chain-looping
// measures a console leaving the CPU some five-eighths of the bus
// while a chain that points at itself is walked; a gap of four thirds
// of the run is the closest this comes to it, and lands nine per cent
// the generous side.
constexpr u32 LIST_GAP_NUMERATOR = 4;
constexpr u32 LIST_GAP_DENOMINATOR = 3;

// How long to wait before looking again at a channel that is armed and
// enabled but whose device is not asking for anything. Short enough
// that the transfer starts promptly once the device is ready, long
// enough that waiting costs nothing.
constexpr u64 RETRY_CYCLES = 64;

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
    // A count of zero means the largest a 16-bit field can ask for.
    return size == 0 ? 0x10000 : size;
}

u32 Dma::Channel::block_words() const
{
    const u32 size = block & 0xFFFF;
    return size == 0 ? 0x10000 : size;
}

u32 Dma::Channel::block_count() const { return block >> 16; }

bool Dma::Channel::chopping() const { return (control & CHCR_CHOPPING) != 0; }

u32 Dma::Channel::chop_words() const
{
    return 1u << ((control >> CHCR_CHOP_DMA_SHIFT) & CHCR_CHOP_WINDOW_MASK);
}

u32 Dma::Channel::chop_cpu_cycles() const
{
    return 1u << ((control >> CHCR_CHOP_CPU_SHIFT) & CHCR_CHOP_WINDOW_MASK);
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

void Dma::Channel::finish()
{
    control &= ~(CHCR_ENABLE | CHCR_TRIGGER);
    running = false;
    remaining = 0;
}

void Dma::visit_state(State& state)
{
    for (Channel& channel : channels) {
        state(channel.base);
        state(channel.block);
        state(channel.control);
        state(channel.running);
        state(channel.remaining);
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

u32 Dma::arbitrate(u32 asking) const
{
    u32 winner = NO_CHANNEL;
    u32 winning_priority = 0;
    for (u32 index = 0; index < CHANNEL_COUNT; index++) {
        if (!channel_ready(index) || (asking & (1u << index)) == 0) {
            continue;
        }
        // Nought is the highest priority, and the scan runs upwards,
        // so taking every channel at least as good as the one held
        // leaves the highest-numbered of any tied set — which is the
        // order hardware breaks ties in.
        const u32 priority = (control >> (index * 4)) & 7;
        if (winner == NO_CHANNEL || priority <= winning_priority) {
            winner = index;
            winning_priority = priority;
        }
    }
    return winner;
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

bool Dma::write_register(u32 phys, u32 value)
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
            channel.base = word & MADR_MASK;
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
            // Clearing the enable bit halfway through is how software
            // calls a transfer off, and the channel stops where it
            // stands rather than finishing what it had left.
            if ((channel.control & CHCR_ENABLE) == 0) {
                channel.running = false;
                channel.remaining = 0;
            }
            break;
        default:
            break;
        }
        return false;
    }

    if (offset == DPCR_OFFSET) {
        control = word;
        return false;
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
        return !was_active && interrupt_active();
    }
    return false;
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

// Whether the device behind a channel wants the bus this instant. It
// is the request half of the handshake the controller runs with each
// of them: the channel is armed by software, but what paces it is the
// device saying it has room for another block, or another block to
// give. Everything here answers the moment it is asked except the
// drive, which has a sector or has not.
bool device_asking(Bus& bus, u32 channel)
{
    switch (static_cast<Dma::Port>(channel)) {
    case Dma::Port::CdRom:
        return bus.cdrom.has_data();
    default:
        return true;
    }
}

// How much of a block the controller reads or writes RAM for, and how
// long it then waits. The hold is the bus taken away from the CPU; the
// gap is the device being slow, which it does with the bus let go.
struct BlockCost {
    u32 hold = 0;
    u32 gap = 0;
};

BlockCost block_cost(u32 channel, u32 words)
{
    const u32 hold = ram_cycles(words);
    const u32 device = words * DEVICE_CYCLES_PER_WORD[channel];
    return {hold, device > hold ? device - hold : 0};
}

// Moves `words` words between RAM and the device, from `address`
// onwards, leaving the address after the last of them behind. That
// walking address is MADR itself: software reading it mid-transfer
// sees how far the channel has got.
void move_words(Bus& bus, u32 channel, u32 words)
{
    Dma::Channel& settings = bus.dma.channels[channel];
    const u32 step = settings.address_decreasing() ? 0u - 4u : 4u;
    u32 address = settings.base & RAM_ADDRESS_MASK;
    u32 remaining = words;

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
    settings.base = address & MADR_MASK;
}

// A Manual transfer: one burst of BCR words. Without chopping the
// controller keeps the bus until the last of them, which is why a game
// that wants to get anything done during a long one asks for chopping
// instead.
BlockCost step_manual(Bus& bus, u32 channel)
{
    Dma::Channel& settings = bus.dma.channels[channel];
    const bool chopped = settings.chopping();
    u32 words = settings.remaining;
    if (chopped) {
        words = std::min(words, settings.chop_words());
    }

    move_words(bus, channel, words);
    settings.remaining -= words;

    BlockCost cost = block_cost(channel, words);
    cost.gap += CHOP_OVERHEAD_CYCLES;
    if (chopped && settings.remaining > 0) {
        // The window software asked for, which is the whole point of
        // chopping: a run of clocks it knows the bus is its own.
        cost.gap += settings.chop_cpu_cycles();
    }
    return cost;
}

// A Request transfer: BCR's low half is the size of a block and its
// high half how many, and the device is asked for each one in turn.
// The count is BCR's own, counted down where software can watch it.
BlockCost step_request(Bus& bus, u32 channel)
{
    Dma::Channel& settings = bus.dma.channels[channel];
    // A block count of nought asks for nothing at all. Saying so here
    // is what keeps the channel from counting down from zero and
    // holding the bus for the rest of time.
    if (settings.remaining == 0) {
        return {};
    }

    const u32 words = settings.block_words();

    move_words(bus, channel, words);
    settings.remaining--;
    settings.block = (settings.remaining << 16) | (settings.block & 0xFFFF);

    BlockCost cost = block_cost(channel, words);
    cost.gap += BLOCK_GAP_CYCLES;
    return cost;
}

// One node of a linked list: a header word saying how many words of
// payload follow it and where the next node is, then the payload. The
// header is read from RAM like the payload is, so it is paid for too.
//
// A chain that points back into itself has no end, and the controller
// follows it for as long as software leaves it running — the CPU
// carries on beside it, and nothing here has to notice.
BlockCost step_linked_list(Bus& bus, u32 channel)
{
    Dma::Channel& settings = bus.dma.channels[channel];
    u32 hold = 0;

    while (hold < LIST_SLICE_CYCLES) {
        const u32 address = settings.base & RAM_ADDRESS_MASK;
        const u32 header = read_ram(bus, address);
        const u32 words = header >> LIST_HEADER_WORDS_SHIFT;

        for (u32 index = 1; index <= words; index++) {
            const u32 payload = (address + index * 4) & RAM_ADDRESS_MASK;
            write_to_device(bus, channel, read_ram(bus, payload));
        }
        hold += ram_cycles(words + 1) + LIST_FOLLOW_CYCLES;

        if ((header & LIST_END) != 0) {
            // What hardware leaves in MADR at the end of a chain: the
            // terminator itself, not the last node's address.
            settings.base = 0xFFFFFF;
            settings.remaining = 0;
            return {hold, 0};
        }
        settings.base = header & MADR_MASK;
    }

    settings.remaining = 1;  // still something to follow
    const u32 gap = hold * LIST_GAP_NUMERATOR / LIST_GAP_DENOMINATOR;
    return {hold, gap};
}

// Takes a channel up: works out what there is to do before the first
// block of it moves. MADR and BCR are left where software put them,
// since the transfer walks them from there.
void begin(Dma::Channel& settings)
{
    settings.running = true;
    switch (settings.sync_mode()) {
    case Dma::SyncMode::Manual:
        settings.remaining = settings.transfer_words();
        break;
    case Dma::SyncMode::Request:
        settings.remaining = settings.block_count();
        break;
    default:
        settings.remaining = 1;  // a list is done when it says so
        break;
    }
}

}  // namespace

void dma_wake(Bus& bus)
{
    // Only ever brought forward, never put off: a channel already
    // waiting on a slow device keeps the earlier appointment, and one
    // just armed does not have to wait for it.
    if (bus.scheduler.next_deadline_for(EventKind::Dma) <= bus.scheduler.now) {
        return;
    }
    for (u32 channel = 0; channel < Dma::CHANNEL_COUNT; channel++) {
        if (bus.dma.channel_ready(channel)) {
            bus.scheduler.schedule_at(EventKind::Dma, bus.scheduler.now);
            return;
        }
    }
}

void dma_event(Bus& bus, u64 deadline)
{
    u32 asking = 0;
    bool armed = false;
    for (u32 channel = 0; channel < Dma::CHANNEL_COUNT; channel++) {
        if (!bus.dma.channel_ready(channel)) {
            continue;
        }
        armed = true;
        report_unserved(bus, channel);
        if (device_asking(bus, channel)) {
            asking |= 1u << channel;
        }
    }

    const u32 channel = bus.dma.arbitrate(asking);
    if (channel == Dma::NO_CHANNEL) {
        // Nothing to move. If a channel is armed all the same, its
        // device has yet to ask; look again shortly. Otherwise the
        // controller has nothing to wake for at all.
        if (armed) {
            bus.scheduler.schedule_at(EventKind::Dma, deadline + RETRY_CYCLES);
        }
        return;
    }

    Dma::Channel& settings = bus.dma.channels[channel];
    if (!settings.running) {
        begin(settings);
    }

    BlockCost cost;
    switch (settings.sync_mode()) {
    case Dma::SyncMode::Manual:
        cost = step_manual(bus, channel);
        break;
    case Dma::SyncMode::Request:
        cost = step_request(bus, channel);
        break;
    default:
        cost = step_linked_list(bus, channel);
        break;
    }

    // The bus is the controller's for as long as the words took, and
    // the CPU stalls on its next access until then.
    bus.dma_hold_until = deadline + cost.hold;

    if (settings.remaining == 0) {
        settings.finish();
        if (bus.dma.complete(channel)) {
            bus.irq.raise(Interrupt::Dma);
        }
    }

    // Always strictly later, so a channel that transfers nothing at
    // all — an empty linked-list packet — still lets the clock move.
    const u64 spent = std::max<u64>(cost.hold + cost.gap, 1);
    bus.scheduler.schedule_at(EventKind::Dma, deadline + spent);
}
