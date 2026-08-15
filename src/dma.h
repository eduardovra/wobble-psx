#pragma once

#include <array>

#include "types.h"

struct Bus;

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
// Writing CHCR's start bits is what runs a transfer, so the controller
// has no step of its own: run_dma() moves the whole thing at once,
// inside the store instruction that started it. Real hardware
// interleaves with the CPU and takes time doing it, which matters for
// timing but not yet for correctness — nothing here can observe the
// difference, because the CPU is stopped for the duration either way.
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
        u32 base = 0;     // MADR
        u32 block = 0;    // BCR
        u32 control = 0;  // CHCR

        SyncMode sync_mode() const;

        // False means the device is the source and RAM the
        // destination.
        bool to_device() const;

        // Some transfers walk backwards through RAM, which is how the
        // ordering table is built.
        bool address_decreasing() const;

        // How many words a Manual or Request transfer moves. Undefined
        // for a linked list, which is bounded by its end marker.
        u32 transfer_words() const;

        // Whether the channel has been told to go. Manual transfers
        // need an explicit trigger as well as being enabled; the
        // others start as soon as they are enabled.
        bool started() const;

        // Clears the bits that started it, which is how software sees
        // that the transfer is over.
        void finish();
    };

    void reset();

    u32 read_register(u32 phys) const;

    // Returns the channel this write may have started, or NO_CHANNEL.
    // The caller runs it, because moving the words needs the rest of
    // the machine and this holds only the registers.
    u32 write_register(u32 phys, u32 value);

    static constexpr u32 NO_CHANNEL = CHANNEL_COUNT;

    // Whether a channel is both switched on in DPCR and started.
    bool channel_ready(u32 channel) const;

    // Records that a channel finished, and reports whether that has
    // brought the controller's interrupt line up.
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
    bool interrupt_active() const;
};

// Runs a channel's transfer to completion. Lives outside Dma because
// it needs RAM and the device at the other end, which is to say the
// whole machine.
void run_dma(Bus& bus, u32 channel);
