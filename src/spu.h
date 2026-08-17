#pragma once

#include <array>

#include "types.h"

struct State;

// The sound processor: twenty-four voices playing compressed samples
// out of half a megabyte of memory of its own, mixed and sent to the
// output.
//
// None of that happens here. What this is, is the half of the SPU
// software can see and wait on: its register file, its RAM, and the
// handshakes that say a transfer has finished and a voice has stopped.
// Nothing is decoded and nothing is heard.
//
// That is worth having on its own, because the SPU's absence does not
// make a game quiet — it makes it hang. The sound library initialises
// the device before anything else, writes its wave data across, and
// spins until the status register agrees with the control register it
// just wrote. A machine that never agrees never gets past the loading
// screen, which is why the sound hardware is on the critical path to a
// game drawing its first frame.
struct Spu {
    static constexpr u32 BASE = 0x1F801C00;
    static constexpr u32 END = 0x1F802000;

    // The registers are sixteen bits each, back to back, and this
    // covers the lot: the voices, the control block, and the reverb
    // configuration behind it.
    static constexpr u32 REGISTER_COUNT = (END - BASE) / 2;

    // Sample memory. Not addressable by the CPU at all — everything
    // reaches it through the transfer registers below, or through the
    // DMA channel that does the same thing in bulk.
    static constexpr u32 RAM_SIZE = 512 * 1024;

    static constexpr u32 VOICE_COUNT = 24;

    void reset();
    void visit_state(State& state);

    // Reads are not const: the status register answers for the moment
    // it is asked, and reading the IRQ flag is part of how software
    // clears it.
    u16 read_register(u32 phys);
    void write_register(u32 phys, u16 value);

    // DMA channel 4, which moves sample data the same way the transfer
    // register does, two halfwords to the word.
    void write_dma(u32 word);
    u32 read_dma();

    // Whether the SPU is asking for the interrupt line. Checked after
    // any access that could have moved the transfer address past the
    // address software armed.
    bool interrupt_pending() const;

    std::array<u16, REGISTER_COUNT> registers{};
    std::array<u8, RAM_SIZE> ram{};

    // Where the next transferred halfword goes. Software sets it in
    // units of eight bytes and it advances by two as data moves.
    u32 transfer_address = 0;

    // Which voices have been started, and which have reached the end
    // of what they were playing.
    u32 key_on = 0;
    u32 key_off = 0;
    u32 ended = 0;

    // Set when a transfer touches the address software armed, and
    // cleared by writing the control register with the enable off.
    bool irq_flag = false;

private:
    // The register at `phys`, as an index into the file.
    static u32 index_of(u32 phys) { return (phys - BASE) / 2; }

    u16 control() const;
    u16 status() const;

    // Moves `value` into sample memory at the transfer address and
    // steps past it, raising the interrupt if it was armed there.
    void transfer(u16 value);
};
