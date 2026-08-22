#pragma once

#include <array>

#include "types.h"

struct State;

// The sound processor: twenty-four voices playing compressed samples
// out of half a megabyte of memory of its own, mixed and sent to the
// output.
//
// A voice is a pointer into that memory and an envelope. It reads
// sixteen-byte ADPCM blocks, decodes each into twenty-eight samples,
// steps through them at whatever pitch it was given, and multiplies
// what comes out by a shape software describes rather than draws: four
// rates and a sustain level, which the voice then walks through on its
// own until it is keyed off. The blocks carry their own loop points, so
// where a sound ends and what it does there is in the data and not in
// any register.
//
// All twenty-four are summed into a stereo pair once every 768 master
// cycles — 44100 times a second, the rate every other clock on the
// console is derived from. `take_output` is where those samples leave
// for a host that has a sound card to put them on; nothing here needs
// one, and a machine whose output nobody drains sounds the same to the
// software running on it.
//
// The voices are not the only thing summed there. The CD-ROM drive
// decodes the compressed audio written between a movie's pictures and
// hands it over one sample at a time, to be mixed in beside them under
// a volume of its own and a bit in the control register that switches
// it off. That path is the drive's, not a voice's: neither the SPU's
// enable nor its mute reaches it.
//
// What is missing is the wet path and the corners: reverb, the noise
// generator, pitch modulation, and the volume sweeps. Each is noted
// where it would have gone.
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

    // Sample data is compressed sixteen bytes at a time: two bytes of
    // header and twenty-eight four-bit differences.
    static constexpr u32 BLOCK_SIZE = 16;
    static constexpr u32 SAMPLES_PER_BLOCK = 28;

    // One stereo sample every 768 master cycles. That is not a chosen
    // ratio: the console's 33.8688 MHz is 44100 * 768, because the
    // whole machine is clocked from the sound chip's crystal.
    static constexpr u32 SAMPLE_RATE = 44100;
    static constexpr u64 TICK_CYCLES = 768;

    // One of the twenty-four voices, as far as anything outside needs
    // to see it: where its decoder has got to, and where its envelope
    // has got to.
    struct Voice {
        // The envelope generator walks these in order, one phase per
        // rate software gave it. A voice that is Off costs nothing:
        // it is not summed and its decoder does not run.
        enum class Phase : u32 { Off, Attack, Decay, Sustain, Release };

        Phase phase = Phase::Off;

        // Envelope level, 0 to 0x7FFF, and the samples left before it
        // next moves. A slow rate moves once in a hundred thousand
        // samples, so the wait is counted rather than recomputed.
        s32 level = 0;
        u32 wait = 0;

        // The block being decoded and the one to jump back to at the
        // end, both byte addresses into sample memory.
        u32 address = 0;
        u32 repeat_address = 0;

        // The header byte that says what to do at the end of the
        // current block — loop, and whether to keep playing after it.
        u8 flags = 0;

        // Where in the block the voice is: which sample is next, and
        // the fraction of a sample past it that the pitch has carried
        // the voice to. The fraction is twelve bits, so a pitch of
        // 0x1000 is the sample memory's own rate.
        u32 index = 0;
        u32 counter = 0;

        std::array<s16, SAMPLES_PER_BLOCK> block{};

        // The last two decoded samples, which the ADPCM filter needs
        // and which carry across a block boundary — a block cannot be
        // decoded on its own.
        s32 adpcm_old = 0;
        s32 adpcm_older = 0;

        // The two samples the output is interpolated between.
        s16 sample = 0;
        s16 previous_sample = 0;
    };

    // A stereo sample on its way out of the machine.
    struct Frame {
        s16 left;
        s16 right;
    };

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

    // The drive's sound for the sample about to be produced. Set once
    // per tick by whoever is pacing the machine, because the SPU has
    // no way to ask the drive for it: the two are wired together and
    // the sample is simply there on the input when the mixer looks.
    void set_cd_input(s16 left, s16 right);

    // Produces one stereo sample: every voice decoded, enveloped and
    // summed. Returns whether a voice has just read the address
    // software armed, which is the only interrupt the SPU raises on
    // its own — the one a transfer raises is caught by the store that
    // caused it.
    bool tick();

    // Whether the SPU is asking for the interrupt line. Checked after
    // any access that could have moved the transfer address past the
    // address software armed.
    bool interrupt_pending() const;

    // Voices with an envelope still running. Nothing on the console
    // reports this; it is here because "is anything playing" is the
    // first question asked of silence.
    u32 active_voices() const;

    // SPUCNT, which says whether the SPU is enabled and unmuted at
    // all — the other first question asked of silence.
    u16 control() const;

    // Takes up to `count` finished frames, oldest first, and reports
    // how many there were. A host calls this as often as its sound
    // card needs feeding.
    u32 take_output(Frame* frames, u32 count);

    // Frames produced but not yet taken.
    u32 output_ready() const;

    std::array<u16, REGISTER_COUNT> registers{};
    std::array<u8, RAM_SIZE> ram{};
    std::array<Voice, VOICE_COUNT> voices{};

    // Where the next transferred halfword goes. Software sets it in
    // units of eight bytes and it advances by two as data moves.
    u32 transfer_address = 0;

    // Which voices have been started, and which have reached the end
    // of what they were playing.
    u32 key_on = 0;
    u32 key_off = 0;
    u32 ended = 0;

    // Set when a transfer or a voice touches the address software
    // armed, and cleared by writing the control register with the
    // enable off.
    bool irq_flag = false;

    // The drive's sound, as it stands on the input. Not a queue: the
    // drive holds what it has decoded and the mixer takes one frame of
    // it per sample, so what is here is only ever the frame being
    // mixed.
    s16 cd_left = 0;
    s16 cd_right = 0;

private:
    // The register at `phys`, as an index into the file.
    static u32 index_of(u32 phys) { return (phys - BASE) / 2; }

    u16 voice_register(u32 voice, u32 offset) const;
    u16 status() const;

    // Moves `value` into sample memory at the transfer address and
    // steps past it, raising the interrupt if it was armed there.
    void transfer(u16 value);

    // A voice starting from its start address, or being let go. Both
    // happen inside the store that asked for them rather than at the
    // next sample, which is a fraction of a millisecond early.
    void start_voice(u32 voice);
    void stop_voice(u32 voice);

    // The decoder: a block at a time, then a sample at a time through
    // it, then on to whatever block the flags point at.
    void decode_block(u32 voice);
    void next_block(u32 voice);
    void advance_voice(u32 voice);

    // This voice's contribution, before its own volume is applied.
    s32 voice_output(u32 voice);

    void step_envelope(u32 voice);

    void push_output(s32 left, s32 right);

    // The mixed output waiting to be taken. Not machine state — the
    // console has no such buffer — so it is neither saved nor
    // restored, and a host that never drains it loses the oldest
    // frames rather than stalling the machine that is filling it.
    // It has to hold the longest run the host can make in one go, or
    // a window presented a few times a second — where each pass runs
    // a quarter of a second of console at once — would lose the sound
    // it made past the end of it.
    static constexpr u32 OUTPUT_FRAMES = 16384;  // 372 ms at 44.1 kHz
    std::array<Frame, OUTPUT_FRAMES> output{};
    u64 produced = 0;
    u64 taken = 0;
};
