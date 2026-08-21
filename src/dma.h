#pragma once

#include <array>

#include "types.h"

struct Bus;
struct State;

// The DMA controller: seven channels that move words between RAM and a
// device without the CPU copying them one at a time.
//
// Each channel is three registers — where in RAM, how much, and how —
// followed by two registers shared by all of them:
//
//   0x1F801080 + n*0x10  MADR  address in RAM to start at
//                 + 0x4  BCR   how much to move
//                 + 0x8  CHCR  direction, step, sync mode, and the
//                              bits that start it
//   0x1F8010F0           DPCR  per-channel enable and priority
//   0x1F8010F4           DICR  per-channel interrupt enable and flags
//
// Writing CHCR's start bits arms a channel; the controller then runs
// on the scheduler, a block to an event, until the channel is done.
// That is what makes a transfer take time: the store that starts one
// returns immediately, and software watching CHCR's busy bit sees it
// stay up for as long as the words take to move.
//
// Two clocks come out of that, and they are not the same. A block's
// words are read or written back to back and the CPU cannot touch RAM
// while they are — that is the *hold*, and it stalls the CPU. What
// comes after is the *gap*: the controller waiting for a slow device
// to want the next block, or handing the CPU the window that
// chopping promised it. The bus is the CPU's again during a gap, and
// a transfer that is mostly gap is one a game can work through.
struct Dma {
    static constexpr u32 BASE = 0x1F801080;
    static constexpr u32 END = 0x1F801100;
    static constexpr u32 CHANNEL_COUNT = 7;

    // Which device sits on each channel. The numbering is the
    // hardware's, and it is also the order DPCR gives priorities in.
    enum class Port : u32 {
        MdecIn = 0,
        MdecOut = 1,
        Gpu = 2,
        CdRom = 3,
        Spu = 4,
        Pio = 5,
        Otc = 6,  // no device: the controller itself writes the list
    };

    // How a channel decides when it is finished.
    enum class SyncMode : u32 {
        Manual = 0,      // a single burst of BCR words
        Request = 1,     // BCR counts blocks, paced by the device
        LinkedList = 2,  // RAM holds a chain of packets to send
    };

    struct Channel {
        // MADR, BCR and CHCR. The first two are not only settings: a
        // running transfer walks MADR along RAM and counts BCR's
        // block half down, and software reading either of them while
        // it runs sees how far it has got.
        u32 base = 0;
        u32 block = 0;
        u32 control = 0;

        // Set once the controller has taken the channel up and not
        // yet finished with it. `remaining` is what is left to do:
        // words for a Manual transfer, blocks for a Request one, and
        // nothing for a linked list, which knows it is done only when
        // it reaches the end marker.
        bool running = false;
        u32 remaining = 0;

        SyncMode sync_mode() const;

        // False means the device is the source and RAM the
        // destination.
        bool to_device() const;

        // Some transfers walk backwards through RAM, which is how the
        // ordering table is built.
        bool address_decreasing() const;

        // How many words a Manual transfer moves, and how a Request
        // one is divided. Undefined for a linked list, which is
        // bounded by its end marker.
        u32 transfer_words() const;
        u32 block_words() const;
        u32 block_count() const;

        // Chopping: a Manual transfer that would otherwise hold the
        // bus from beginning to end, told to let go of it every so
        // often. `chop_words` is how many words it moves before it
        // does, `chop_cpu_cycles` how long it leaves the CPU alone
        // afterwards — both powers of two, both written as the
        // exponent.
        bool chopping() const;
        u32 chop_words() const;
        u32 chop_cpu_cycles() const;

        // Whether the channel has been told to go. Manual transfers
        // need an explicit trigger as well as being enabled; the
        // others start as soon as they are enabled.
        // Whether the channel is set up to run. `device_asking` is
        // whether the thing behind it wants the bus, which is what
        // starts a Manual transfer that software has not triggered.
        bool started(bool device_asking) const;

        // Clears the bits that started it, which is how software sees
        // that the transfer is over.
        void finish();
    };

    void reset();

    void visit_state(State& state);

    // Every register here is a word, and software need not read or
    // write it as one — a game switches one channel's interrupt on by
    // writing the byte its enable lives in. What a narrower access
    // names is the byte lane it starts at, since the bus carries the
    // whole word either way and these registers ignore the byte
    // enables that would narrow it.
    u32 read_register(u32 phys) const;

    static constexpr u32 NO_CHANNEL = CHANNEL_COUNT;

    // Reports whether the write brought the controller's interrupt
    // line up, which is the one thing it cannot do for itself. What
    // it may also have done — armed a channel — is left to the caller
    // to notice, because starting one needs the rest of the machine.
    bool write_register(u32 phys, u32 value);

    // Whether a channel is both switched on in DPCR and started.
    bool channel_ready(u32 channel) const;

    // Which channel gets the bus next, or NO_CHANNEL when none can
    // take it. DPCR gives each a priority, nought being the highest;
    // where two are equal the higher-numbered channel wins, which is
    // the order hardware settles it in. `asking` is a bit per channel
    // whose device wants the bus — a channel the register bits have
    // armed still waits its turn behind one whose device is ready.
    u32 arbitrate(u32 asking) const;

    // Records that a channel finished, and reports whether that has
    // brought the controller's interrupt line up. A channel whose
    // interrupt DICR has not enabled does not even raise its flag:
    // the enable is what lets the flag be set, not what lets a flag
    // already set through.
    bool complete(u32 channel);

    std::array<Channel, CHANNEL_COUNT> channels{};

    // DPCR powers up with every channel disabled but priorities set.
    u32 control = 0x07654321;
    u32 interrupt = 0;  // DICR, minus its computed top bit

private:
    // DICR bit 31: set when any enabled channel has flagged, or when
    // software forces it. Read-only, so it is computed rather than
    // stored — which means acknowledging the last channel flag lowers
    // it with no write of its own.
    //
    // It is the moment it goes up that reaches the interrupt
    // controller, not the level, so a write that raises it counts as
    // much as a transfer finishing does.
    bool interrupt_active() const;

    // A register as a whole, by its offset from BASE.
    u32 whole_register(u32 offset) const;

    // How far into its word the byte an address names sits.
    static u32 lane_shift(u32 phys);
};

// One turn of the controller: gives the bus to whichever channel wants
// it most, moves a block, and asks the scheduler to come back when
// that block has been paid for. Lives outside Dma because it needs RAM
// and the device at the other end, which is to say the whole machine.
//
// `deadline` is the cycle the turn was due on, which is a little
// behind the clock — the CPU can only stop between instructions. The
// next turn is timed from it rather than from now, or every overshoot
// is added to the transfer and a long one finishes late by the sum of
// them all.
void dma_event(Bus& bus, u64 deadline);

// Looks again at what the channels are asking for, and wakes the
// controller when one of them wants the bus and nothing has it. Every
// write to a DMA register goes through here, because any of them can
// be the one that arms a channel.
void dma_wake(Bus& bus);
