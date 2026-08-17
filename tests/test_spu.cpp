#include <memory>

#include <doctest/doctest.h>

#include "console.h"
#include "spu.h"

namespace {

// The SPU reached through the bus, the way software does it, so the
// sixteen-bit register decode and the widening of a word access are
// part of what is tested.
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
};

constexpr u32 KEY_ON = 0x1F801D88;
constexpr u32 KEY_OFF = 0x1F801D8C;
constexpr u32 ENDX = 0x1F801D9C;
constexpr u32 TRANSFER_ADDRESS = 0x1F801DA6;
constexpr u32 TRANSFER_FIFO = 0x1F801DA8;
constexpr u32 CONTROL = 0x1F801DAA;
constexpr u32 STATUS = 0x1F801DAE;

// A voice's volume register, which is only here to be written and read
// back again.
constexpr u32 VOICE0_VOLUME_LEFT = 0x1F801C00;

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
    sound.write32(KEY_ON, 0x00FF0001);

    CHECK(sound.spu().key_on == 0x00FF0001);
    CHECK(sound.read32(ENDX) == 0x00FF0001);
}

TEST_CASE("a voice reports at once that it has finished")
{
    // There is no decoder, so a started voice is treated as having
    // already reached the end of its sample. Software that waits for a
    // sound to finish then carries on rather than stopping dead.
    const Sound sound;
    sound.write32(KEY_ON, 0x00000005);
    CHECK(sound.read32(ENDX) == 0x00000005);

    // Keying off clears the voice from the started set.
    sound.write32(KEY_OFF, 0x00000001);
    CHECK(sound.spu().key_on == 0x00000004);
    CHECK(sound.spu().key_off == 0x00000001);
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
