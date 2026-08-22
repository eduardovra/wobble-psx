#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

#include <doctest/doctest.h>

#include "console.h"
#include "spu.h"

namespace {

constexpr u32 KEY_ON = 0x1F801D88;
constexpr u32 KEY_OFF = 0x1F801D8C;
constexpr u32 NOISE_MODE = 0x1F801D94;
constexpr u32 ENDX = 0x1F801D9C;
constexpr u32 IRQ_ADDRESS = 0x1F801DA4;
constexpr u32 TRANSFER_ADDRESS = 0x1F801DA6;
constexpr u32 TRANSFER_FIFO = 0x1F801DA8;
constexpr u32 CONTROL = 0x1F801DAA;
constexpr u32 STATUS = 0x1F801DAE;
constexpr u32 MAIN_VOLUME_LEFT = 0x1F801D80;
constexpr u32 MAIN_VOLUME_RIGHT = 0x1F801D82;
constexpr u32 CD_VOLUME_LEFT = 0x1F801DB0;
constexpr u32 CD_VOLUME_RIGHT = 0x1F801DB2;

// The drive's volume is not written the way a voice's is: there is no
// sweep behind it, so all sixteen bits are the volume and this is as
// loud as the drive goes.
constexpr u16 FULL_CD_VOLUME = 0x7FFF;

// Voice 0's registers, which is the voice everything here plays on.
constexpr u32 VOICE0_VOLUME_LEFT = 0x1F801C00;
constexpr u32 VOICE0_VOLUME_RIGHT = 0x1F801C02;
constexpr u32 VOICE0_SAMPLE_RATE = 0x1F801C04;
constexpr u32 VOICE0_START_ADDRESS = 0x1F801C06;
constexpr u32 VOICE0_ADSR_LO = 0x1F801C08;
constexpr u32 VOICE0_ADSR_HI = 0x1F801C0A;
constexpr u32 VOICE0_ADSR_VOLUME = 0x1F801C0C;

// The volume registers hold half the volume they mean, so this is as
// loud as a voice goes.
constexpr u16 FULL_VOLUME = 0x3FFF;

// Sample memory is addressed in units of eight bytes, and this is
// where every sample in this file is put.
constexpr u32 SAMPLE_BASE = 0x1000;

// One sample of the data per sample of output: the rate a sound was
// recorded at, played back at the rate it was recorded.
constexpr u16 UNITY_PITCH = 0x1000;

// The flag byte of an ADPCM block.
constexpr u8 LOOP_END = 1 << 0;
constexpr u8 LOOP_REPEAT = 1 << 1;

using Block = std::array<u8, Spu::BLOCK_SIZE>;

// An ADPCM block: a header saying how to decode it, a flag byte saying
// what to do when it ends, and twenty-eight four-bit differences packed
// two to a byte, low nibble first.
Block make_block(u8 header,
                 u8 flags,
                 const std::array<u8, Spu::SAMPLES_PER_BLOCK>& nibbles)
{
    Block block{};
    block[0] = header;
    block[1] = flags;
    for (u32 i = 0; i < nibbles.size(); i++) {
        const u8 shift = (i % 2 == 0) ? 0 : 4;
        block[2 + i / 2] |= static_cast<u8>((nibbles[i] & 0x0F) << shift);
    }
    return block;
}

// The SPU reached through the bus, the way software does it, so the
// sixteen-bit register decode and the widening of a word access are
// part of what is tested. Time is moved by hand: what matters is how
// many samples have gone by, not what the CPU was doing meanwhile.
struct Sound {
    std::unique_ptr<Console> console = std::make_unique<Console>();

    Sound() { console->reset(); }

    Spu& spu() const { return console->bus.spu; }

    void write16(u32 address, u16 value) const
    {
        console->bus.write16(address, value);
    }

    void write32(u32 address, u32 value) const
    {
        console->bus.write32(address, value);
    }

    u16 read16(u32 address) const { return console->bus.read16(address); }
    u32 read32(u32 address) const { return console->bus.read32(address); }

    // Sample data on its way into sample memory, through the transfer
    // register rather than by reaching into the array.
    void upload(u32 address, const Block& block) const
    {
        write16(TRANSFER_ADDRESS, static_cast<u16>(address / 8));
        for (u32 i = 0; i < block.size(); i += 2) {
            const u16 half =
                static_cast<u16>(block[i] | (u32{block[i + 1]} << 8));
            write16(TRANSFER_FIFO, half);
        }
    }

    // A voice set up to play what is at SAMPLE_BASE, at full volume and
    // at the rate the data was recorded at.
    void arm_voice0() const
    {
        write16(CONTROL, 0xC000);  // enabled and unmuted
        write16(MAIN_VOLUME_LEFT, FULL_VOLUME);
        write16(MAIN_VOLUME_RIGHT, FULL_VOLUME);
        write16(VOICE0_VOLUME_LEFT, FULL_VOLUME);
        write16(VOICE0_VOLUME_RIGHT, FULL_VOLUME);
        write16(VOICE0_SAMPLE_RATE, UNITY_PITCH);
        write16(VOICE0_START_ADDRESS, SAMPLE_BASE / 8);
        // Sustain at the top, so the envelope stays where it is put
        // and the test hears the samples rather than the shape.
        write16(VOICE0_ADSR_LO, 0x000F);
        write16(VOICE0_ADSR_HI, 0x0000);
    }

    void key_on(u32 voices) const { write32(KEY_ON, voices); }
    void key_off(u32 voices) const { write32(KEY_OFF, voices); }

    void ticks(u32 count) const
    {
        console->scheduler.advance(count * Spu::TICK_CYCLES);
        console->dispatch_due_events();
    }

    // The same, with the drive's sound standing on the input. The SPU
    // is ticked directly rather than through the clock, because what
    // paces the machine sets this input every sample from the drive —
    // and the drive in these tests is empty.
    void cd_ticks(u32 count, s16 left, s16 right) const
    {
        for (u32 i = 0; i < count; i++) {
            spu().set_cd_input(left, right);
            spu().tick();
        }
    }
};

}  // namespace

TEST_CASE("the status register echoes the mode the control register asked for")
{
    // This is the handshake the sound library spins on before it will
    // go on, so it is the one thing the SPU has to get right even
    // before it makes a sound.
    const Sound sound;
    sound.write16(CONTROL, 0x8001);
    CHECK((sound.read16(STATUS) & 0x3F) == 0x01);

    sound.write16(CONTROL, 0xC020);  // transfer mode 2: DMA write
    CHECK((sound.read16(STATUS) & 0x3F) == 0x20);

    // And says which way it wants the DMA controller to move data.
    CHECK((sound.read16(STATUS) & (1 << 8)) != 0);   // write request
    CHECK((sound.read16(STATUS) & (1 << 9)) == 0);   // not a read
    CHECK((sound.read16(STATUS) & (1 << 10)) == 0);  // never busy
}

TEST_CASE("a voice register reads back what was written to it")
{
    const Sound sound;
    sound.write16(VOICE0_VOLUME_LEFT, 0x3FFF);
    CHECK(sound.read16(VOICE0_VOLUME_LEFT) == 0x3FFF);
}

TEST_CASE("sample data written through the transfer register lands in RAM")
{
    const Sound sound;

    // The address is set in units of eight bytes.
    sound.write16(TRANSFER_ADDRESS, 0x0100);
    sound.write16(CONTROL, 0x0010);  // manual transfer
    sound.write16(TRANSFER_FIFO, 0x1234);
    sound.write16(TRANSFER_FIFO, 0x5678);

    const u32 base = 0x0100 * 8;
    CHECK(sound.spu().ram[base] == 0x34);
    CHECK(sound.spu().ram[base + 1] == 0x12);
    CHECK(sound.spu().ram[base + 2] == 0x78);
    CHECK(sound.spu().ram[base + 3] == 0x56);

    // The address has moved on with the data, so a transfer is a
    // stream rather than a repeated write to one place.
    CHECK(sound.spu().transfer_address == base + 4);
}

TEST_CASE("a word access reaches two registers, not one wide one")
{
    // The SPU is a sixteen-bit device, and the pair that starts a
    // voice is written as a single word — so a word write that only
    // reached the low half would never start voices 16 to 23.
    const Sound sound;
    sound.key_on(0x00FF0001);

    CHECK(sound.spu().key_on == 0x00FF0001);
    CHECK(sound.read32(KEY_ON) == 0x00FF0001);
}

TEST_CASE("the DMA channel moves sample data the same way")
{
    const Sound sound;
    sound.write16(TRANSFER_ADDRESS, 0x0020);
    sound.spu().write_dma(0xAABBCCDD);

    const u32 base = 0x0020 * 8;
    CHECK(sound.spu().ram[base] == 0xDD);
    CHECK(sound.spu().ram[base + 1] == 0xCC);
    CHECK(sound.spu().ram[base + 2] == 0xBB);
    CHECK(sound.spu().ram[base + 3] == 0xAA);

    // And reads it back out again from where it was put.
    sound.write16(TRANSFER_ADDRESS, 0x0020);
    CHECK(sound.spu().read_dma() == 0xAABBCCDD);
}

TEST_CASE("a block of differences decodes to the samples it describes")
{
    // Shift 0 and filter 0: no prediction and no scaling, so each
    // four-bit difference is the sample, sign-extended from the top of
    // a sixteen-bit word. That makes what the decoder should produce
    // something the test can state rather than compute.
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, 0, {1, 2, 0xF, 0xE, 7, 8, 0}));
    sound.arm_voice0();
    sound.key_on(1);

    const Spu::Voice& voice = sound.spu().voices[0];
    CHECK(voice.block[0] == 0x1000);
    CHECK(voice.block[1] == 0x2000);
    CHECK(voice.block[2] == -0x1000);  // 0xF is -1, not 15
    CHECK(voice.block[3] == -0x2000);
    CHECK(voice.block[4] == 0x7000);
    CHECK(voice.block[5] == -0x8000);
    CHECK(voice.block[6] == 0);
}

TEST_CASE("the shift in the header scales the whole block")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x04, 0, {1, 0xF}));
    sound.arm_voice0();
    sound.key_on(1);

    // Four bits of shift is a sixteenth of the step.
    CHECK(sound.spu().voices[0].block[0] == 0x0100);
    CHECK(sound.spu().voices[0].block[1] == -0x0100);
}

TEST_CASE("a sample is predicted from the two decoded before it")
{
    // Filter 1 adds 60/64 of the previous sample, which is what makes
    // ADPCM out of what would otherwise be four bits of resolution: a
    // run of zero differences holds the waveform rather than silencing
    // it.
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x10, 0, {4, 0, 0}));
    sound.arm_voice0();
    sound.key_on(1);

    const Spu::Voice& voice = sound.spu().voices[0];
    CHECK(voice.block[0] == 0x4000);
    CHECK(voice.block[1] == (0x4000 * 60 + 32) / 64);
    CHECK(voice.block[2] == (voice.block[1] * 60 + 32) / 64);
}

TEST_CASE("what a voice decodes comes out of the mixer")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, 0, {4, 4, 4, 4, 4, 4, 4, 4}));
    sound.arm_voice0();
    sound.key_on(1);
    // Straight to full envelope: the shape a sound is given is tested
    // on its own below, and here it would only scale the answer.
    sound.write16(VOICE0_ADSR_VOLUME, 0x7FFF);

    sound.ticks(8);

    std::array<Spu::Frame, 8> frames{};
    CHECK(sound.spu().take_output(frames.data(), frames.size()) == 8);

    // The voice, its own volume and the main volume are each a
    // fraction just short of one, so the sample arrives a count or two
    // under the 0x4000 it was decoded as. The first frame is the ramp
    // up from silence that interpolating between two samples costs.
    CHECK(frames[0].left == 0);
    for (u32 i = 1; i < frames.size(); i++) {
        CHECK(frames[i].left == doctest::Approx(0x4000).epsilon(0.001));
        CHECK(frames[i].right == frames[i].left);
    }
}

TEST_CASE("a voice resampled between two samples stays between them")
{
    // A pitch that is not a whole number of samples puts the output
    // between two of them, and a falling waveform is where that goes
    // wrong: interpolating downwards has to produce a value below the
    // one before it, not an enormous one. Nothing in the mix is
    // louder than the loudest sample that went into it.
    const Sound sound;
    sound.upload(
        SAMPLE_BASE,
        make_block(0x00,
                   LOOP_END | LOOP_REPEAT,
                   {7, 5, 3, 1, 0xF, 0xD, 0xB, 9, 0xB, 0xD, 0xF, 1, 3, 5}));
    sound.arm_voice0();
    sound.write16(VOICE0_SAMPLE_RATE, 0x0555);  // a third of unity
    sound.key_on(1);
    sound.write16(VOICE0_ADSR_VOLUME, 0x7FFF);

    sound.ticks(256);

    std::array<Spu::Frame, 256> frames{};
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 256);

    // The loudest sample in the block is 0x7000, and the volumes it
    // passes through are each just under one.
    s32 peak = 0;
    for (const Spu::Frame& frame : frames) {
        CHECK(std::abs(static_cast<s32>(frame.left)) <= 0x7000);
        peak = std::max(peak, std::abs(static_cast<s32>(frame.left)));
    }
    // And the waveform did reach for it, so the bound above is not
    // being met by silence.
    CHECK(peak > 0x6000);
}

TEST_CASE("the mute bit silences the output without stopping the voices")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, 0, {4, 4, 4, 4}));
    sound.arm_voice0();
    sound.key_on(1);
    sound.write16(VOICE0_ADSR_VOLUME, 0x7FFF);
    sound.write16(CONTROL, 0x8000);  // enabled, muted

    sound.ticks(4);

    std::array<Spu::Frame, 4> frames{};
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 4);
    for (const Spu::Frame& frame : frames) {
        CHECK(frame.left == 0);
        CHECK(frame.right == 0);
    }

    // The voice went on playing behind the mute, which is why
    // unmuting does not restart a sound that should be half over.
    CHECK(sound.spu().voices[0].index > 1);
}

TEST_CASE("a sample that ends without looping stops the voice")
{
    // The end of a sound is in the data, not in a register: the last
    // block says so, and a voice that reaches it stops on its own.
    // Software waits on exactly this to know a sound has finished.
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, LOOP_END, {1, 1, 1, 1}));
    sound.arm_voice0();
    sound.key_on(1);

    CHECK(sound.read32(ENDX) == 0);

    sound.ticks(Spu::SAMPLES_PER_BLOCK + 2);

    CHECK(sound.read32(ENDX) == 1);
    CHECK(sound.spu().voices[0].phase == Spu::Voice::Phase::Off);
    CHECK(sound.spu().active_voices() == 0);
}

TEST_CASE("a sample that says to repeat plays on from its loop point")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE,
                 make_block(0x00, LOOP_END | LOOP_REPEAT, {1, 1, 1, 1}));
    sound.arm_voice0();
    sound.key_on(1);

    sound.ticks(Spu::SAMPLES_PER_BLOCK * 3);

    // The end was reached and reported, and the voice is still going.
    CHECK(sound.read32(ENDX) == 1);
    CHECK(sound.spu().voices[0].phase != Spu::Voice::Phase::Off);
    CHECK(sound.spu().voices[0].address == SAMPLE_BASE);
}

TEST_CASE("keying a voice on clears the end it reported last time")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, LOOP_END, {1}));
    sound.arm_voice0();
    sound.key_on(1);
    sound.ticks(Spu::SAMPLES_PER_BLOCK + 2);
    REQUIRE(sound.read32(ENDX) == 1);

    sound.key_on(1);
    CHECK(sound.read32(ENDX) == 0);
    CHECK(sound.spu().voices[0].phase == Spu::Voice::Phase::Attack);
}

TEST_CASE("the envelope rises when a voice starts and falls when it stops")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, 0, {1, 1, 1, 1}));
    sound.arm_voice0();

    // The fastest attack there is: a whole step of 7 shifted up
    // eleven places, every sample.
    sound.write16(VOICE0_ADSR_LO, 0x000F);
    sound.key_on(1);
    CHECK(sound.read16(VOICE0_ADSR_VOLUME) == 0);

    sound.ticks(1);
    CHECK(sound.read16(VOICE0_ADSR_VOLUME) == 7 << 11);

    // And it stops at the top rather than wrapping round.
    sound.ticks(8);
    CHECK(sound.read16(VOICE0_ADSR_VOLUME) == 0x7FFF);

    // Keying off does not silence a voice: it releases it, and the
    // sound dies away at the rate software asked for.
    sound.key_off(1);
    CHECK(sound.spu().voices[0].phase == Spu::Voice::Phase::Release);
    CHECK(sound.read16(VOICE0_ADSR_VOLUME) == 0x7FFF);

    sound.ticks(3);
    CHECK(sound.read16(VOICE0_ADSR_VOLUME) == 0);
    CHECK(sound.spu().voices[0].phase == Spu::Voice::Phase::Off);
}

TEST_CASE("a voice falls to the sustain level and stays there")
{
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, 0, {1, 1, 1, 1}));
    sound.arm_voice0();

    // Sustain level 7, which is 8 * 0x800, with the fastest decay and
    // a sustain slow enough to be standing still.
    sound.write16(VOICE0_ADSR_LO, 0x0007);
    sound.write16(VOICE0_ADSR_HI, 0x7F << 6);
    sound.key_on(1);
    sound.ticks(64);

    CHECK(sound.spu().voices[0].phase == Spu::Voice::Phase::Sustain);
    const u16 settled = sound.read16(VOICE0_ADSR_VOLUME);
    CHECK(settled == doctest::Approx(8 * 0x800).epsilon(0.001));

    // And stays there for as long as the note is held, which is the
    // whole point of the phase: nothing but a key off ends it.
    sound.ticks(1024);
    CHECK(sound.read16(VOICE0_ADSR_VOLUME) == settled);
    CHECK(sound.spu().voices[0].phase == Spu::Voice::Phase::Sustain);
}

TEST_CASE("a voice reading the armed address interrupts the CPU")
{
    // Sample memory is where a game watches its own playback from: it
    // arms an address partway through a long sound and is told when
    // the voice gets there.
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, 0, {1, 1, 1, 1}));
    sound.upload(SAMPLE_BASE + Spu::BLOCK_SIZE,
                 make_block(0x00, LOOP_END, {1, 1, 1, 1}));

    sound.arm_voice0();
    sound.write16(IRQ_ADDRESS, (SAMPLE_BASE + Spu::BLOCK_SIZE) / 8);
    sound.write16(CONTROL, 0xC040);  // enabled, unmuted, IRQ on
    sound.key_on(1);

    CHECK_FALSE(sound.spu().interrupt_pending());
    CHECK((sound.console->bus.irq.status & (1 << 9)) == 0);

    // Far enough for the voice to reach the second block.
    sound.ticks(Spu::SAMPLES_PER_BLOCK + 2);

    CHECK(sound.spu().interrupt_pending());
    CHECK((sound.console->bus.irq.status & (1 << 9)) != 0);

    // Turning the enable off is how the handler acknowledges it.
    sound.write16(CONTROL, 0xC000);
    CHECK_FALSE(sound.spu().interrupt_pending());
}

TEST_CASE("a voice in noise mode is silent rather than wrong")
{
    // The noise generator is not modelled. The voice still runs, so
    // the flags software waits on stay honest, but nothing of it is
    // heard — which is a missing hi-hat rather than a burst of the
    // sample data played as if it were noise.
    const Sound sound;
    sound.upload(SAMPLE_BASE, make_block(0x00, LOOP_END, {4, 4, 4, 4}));
    sound.arm_voice0();
    sound.write32(NOISE_MODE, 1);
    sound.key_on(1);
    sound.write16(VOICE0_ADSR_VOLUME, 0x7FFF);

    sound.ticks(Spu::SAMPLES_PER_BLOCK + 2);

    std::array<Spu::Frame, 8> frames{};
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 8);
    for (const Spu::Frame& frame : frames) {
        CHECK(frame.left == 0);
    }
    CHECK(sound.read32(ENDX) == 1);
}

TEST_CASE("output nobody takes is dropped rather than backing up")
{
    // The machine cannot wait for a host to want its sound, so the
    // buffer between them is finite and the oldest frames go first.
    const Sound sound;
    sound.ticks(Spu::SAMPLE_RATE / 2);  // half a second of silence

    const u32 ready = sound.spu().output_ready();
    CHECK(ready > 0);
    CHECK(ready < Spu::SAMPLE_RATE / 2);

    std::array<Spu::Frame, 16> frames{};
    CHECK(sound.spu().take_output(frames.data(), frames.size()) == 16);
    CHECK(sound.spu().output_ready() == ready - 16);
}

TEST_CASE("the drive's sound is mixed in beside the voices")
{
    const Sound sound;
    sound.write16(CONTROL, 0xC001);  // enabled, unmuted, CD audio on
    sound.write16(MAIN_VOLUME_LEFT, FULL_VOLUME);
    sound.write16(MAIN_VOLUME_RIGHT, FULL_VOLUME);
    sound.write16(CD_VOLUME_LEFT, FULL_CD_VOLUME);
    sound.write16(CD_VOLUME_RIGHT, FULL_CD_VOLUME);

    // No voice is playing, so whatever comes out is the drive's.
    sound.cd_ticks(4, 0x4000, -0x2000);

    std::array<Spu::Frame, 4> frames{};
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 4);
    for (const Spu::Frame& frame : frames) {
        CHECK(frame.left == doctest::Approx(0x4000).epsilon(0.01));
        CHECK(frame.right == doctest::Approx(-0x2000).epsilon(0.01));
    }
}

TEST_CASE("the drive's sound has its own volume and its own switch")
{
    const Sound sound;
    sound.write16(CONTROL, 0xC001);
    sound.write16(MAIN_VOLUME_LEFT, FULL_VOLUME);
    sound.write16(MAIN_VOLUME_RIGHT, FULL_VOLUME);

    // Half as loud on the left as on the right, which is the volume
    // register doing the only thing it does.
    sound.write16(CD_VOLUME_LEFT, FULL_CD_VOLUME / 2);
    sound.write16(CD_VOLUME_RIGHT, FULL_CD_VOLUME);
    sound.cd_ticks(1, 0x4000, 0x4000);

    std::array<Spu::Frame, 1> frames{};
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 1);
    CHECK(frames[0].left == doctest::Approx(0x2000).epsilon(0.01));
    CHECK(frames[0].right == doctest::Approx(0x4000).epsilon(0.01));

    // With the bit down the drive is not heard at all, however loud
    // its volume is.
    sound.write16(CONTROL, 0xC000);
    sound.cd_ticks(1, 0x4000, 0x4000);
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 1);
    CHECK(frames[0].left == 0);
    CHECK(frames[0].right == 0);
}

TEST_CASE("a muted sound processor still plays the drive")
{
    // The enable and the mute are the voices'. A game that has
    // switched its own sound off while a movie plays still hears the
    // movie, which is what those two bits not reaching this path
    // means.
    const Sound sound;
    sound.write16(CONTROL, 0x0001);  // CD audio on, everything else off
    sound.write16(MAIN_VOLUME_LEFT, FULL_VOLUME);
    sound.write16(MAIN_VOLUME_RIGHT, FULL_VOLUME);
    sound.write16(CD_VOLUME_LEFT, FULL_CD_VOLUME);
    sound.write16(CD_VOLUME_RIGHT, FULL_CD_VOLUME);

    sound.cd_ticks(1, 0x4000, 0x4000);

    std::array<Spu::Frame, 1> frames{};
    REQUIRE(sound.spu().take_output(frames.data(), frames.size()) == 1);
    CHECK(frames[0].left == doctest::Approx(0x4000).epsilon(0.01));
    CHECK(frames[0].right == doctest::Approx(0x4000).epsilon(0.01));
}
