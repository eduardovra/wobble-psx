#include <memory>

#include <doctest/doctest.h>

#include "bus.h"
#include "dma.h"
#include "machine.h"

namespace {

constexpr u32 GPU_CHANNEL = 2;
constexpr u32 OTC_CHANNEL = 6;

// A channel's three registers, by channel number.
constexpr u32 madr(u32 channel) { return Dma::BASE + channel * 0x10; }
constexpr u32 bcr(u32 channel) { return madr(channel) + 4; }
constexpr u32 chcr(u32 channel) { return madr(channel) + 8; }

constexpr u32 DPCR = Dma::BASE + 0x70;
constexpr u32 DICR = Dma::BASE + 0x74;

// CHCR values for the shapes of transfer used below.
constexpr u32 START = (1u << 24) | (1u << 28);  // enable and trigger
constexpr u32 TO_DEVICE = 1u << 0;
constexpr u32 DECREASING = 1u << 1;
constexpr u32 SYNC_LINKED_LIST = 2u << 9;

// DPCR with every channel switched on.
constexpr u32 ALL_CHANNELS_ENABLED = 0x88888888;

// Somewhere in RAM to work in, clear of anything else the tests use.
constexpr u32 SCRATCH = 0x00010000;

void write_ram(Bus& bus, u32 address, u32 value)
{
    bus.write32(address, value);
}

}  // namespace

TEST_CASE("a channel runs only when both it and DPCR say so")
{
    const LooseBus bus;
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 4);

    SUBCASE("DPCR still disabled leaves the channel armed but idle")
    {
        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
        CHECK(bus->read32(SCRATCH) == 0);
        // Still armed: enabling it in DPCR is what sets it going.
        CHECK((bus->read32(chcr(OTC_CHANNEL)) & (1u << 24)) != 0);

        bus->write32(DPCR, ALL_CHANNELS_ENABLED);
        CHECK(bus->read32(SCRATCH) != 0);
    }

    SUBCASE("a manual transfer waits for its trigger as well")
    {
        bus->write32(DPCR, ALL_CHANNELS_ENABLED);
        bus->write32(chcr(OTC_CHANNEL), (1u << 24) | DECREASING);
        CHECK(bus->read32(SCRATCH) == 0);

        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
        CHECK(bus->read32(SCRATCH) != 0);
    }
}

TEST_CASE("the ordering table channel chains backwards to a terminator")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);

    // Four entries, built downwards from SCRATCH.
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 4);
    bus->write32(chcr(OTC_CHANNEL), START | DECREASING);

    CHECK(bus->read32(SCRATCH) == SCRATCH - 4);
    CHECK(bus->read32(SCRATCH - 4) == SCRATCH - 8);
    CHECK(bus->read32(SCRATCH - 8) == SCRATCH - 12);
    // The last entry written is the end of the list.
    CHECK(bus->read32(SCRATCH - 12) == 0xFFFFFF);

    // And the channel reports itself finished.
    CHECK((bus->read32(chcr(OTC_CHANNEL)) & (1u << 24)) == 0);
}

TEST_CASE("a block transfer hands every word to the GPU")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);

    // An image transfer into VRAM: three words of command, then one
    // word of pixels.
    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);               // to 0,0
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);  // a 2x1 rectangle
    write_ram(*bus, SCRATCH + 12, 0xBBBBAAAA);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), 4);
    bus->write32(chcr(GPU_CHANNEL), START | TO_DEVICE);

    CHECK(bus->gpu.vram[0] == 0xAAAA);
    CHECK(bus->gpu.vram[1] == 0xBBBB);
}

TEST_CASE("a linked list follows its chain until the end marker")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);

    // Two packets, the second not adjacent to the first, so only the
    // chain can find it. Each carries one word of a two-word image
    // transfer split across them.
    const u32 second = SCRATCH + 0x100;
    write_ram(*bus, SCRATCH + 0, (2u << 24) | second);  // 2 words, then
    write_ram(*bus, SCRATCH + 4, 0xA0000000);
    write_ram(*bus, SCRATCH + 8, 0);

    write_ram(*bus, second + 0, (2u << 24) | 0xFFFFFF);  // 2 words, end
    write_ram(*bus, second + 4, (1u << 16) | 2);
    write_ram(*bus, second + 8, 0xDDDDCCCC);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE | SYNC_LINKED_LIST);

    CHECK(bus->gpu.vram[0] == 0xCCCC);
    CHECK(bus->gpu.vram[1] == 0xDDDD);
}

TEST_CASE("an empty linked-list packet still advances the chain")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);

    const u32 second = SCRATCH + 0x100;
    write_ram(*bus, SCRATCH, (0u << 24) | second);  // no payload
    write_ram(*bus, second + 0, (4u << 24) | 0xFFFFFF);
    write_ram(*bus, second + 4, 0xA0000000);
    write_ram(*bus, second + 8, 0);
    write_ram(*bus, second + 12, (1u << 16) | 2);
    write_ram(*bus, second + 16, 0x22221111);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE | SYNC_LINKED_LIST);

    CHECK(bus->gpu.vram[0] == 0x1111);
    CHECK(bus->gpu.vram[1] == 0x2222);
}

TEST_CASE("a request transfer moves blocksize times block count")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);

    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);
    write_ram(*bus, SCRATCH + 12, 0x44443333);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), (2u << 16) | 2);  // 2 blocks of 2
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE | (1u << 9));

    CHECK(bus->gpu.vram[0] == 0x3333);
    CHECK(bus->gpu.vram[1] == 0x4444);
}

TEST_CASE("a completed channel interrupts only when it is allowed to")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus->irq.mask = 1u << static_cast<u32>(Interrupt::Dma);

    const u32 flag = 1u << (24 + OTC_CHANNEL);
    const u32 enable = 1u << (16 + OTC_CHANNEL);
    constexpr u32 MASTER_ENABLE = 1u << 23;

    auto run_otc = [&] {
        bus->write32(madr(OTC_CHANNEL), SCRATCH);
        bus->write32(bcr(OTC_CHANNEL), 2);
        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    };

    SUBCASE("with the channel's interrupt disabled, nothing is raised")
    {
        run_otc();
        // The flag is still recorded; it just cannot reach the CPU.
        CHECK((bus->read32(DICR) & flag) != 0);
        CHECK((bus->read32(DICR) & (1u << 31)) == 0);
        CHECK_FALSE(bus->irq.active());
    }

    SUBCASE("enabled, it raises the DMA line")
    {
        bus->write32(DICR, MASTER_ENABLE | enable);
        run_otc();
        CHECK((bus->read32(DICR) & (1u << 31)) != 0);
        CHECK(bus->irq.active());
    }

    SUBCASE("the master enable alone gates it")
    {
        bus->write32(DICR, enable);  // no master enable
        run_otc();
        CHECK((bus->read32(DICR) & (1u << 31)) == 0);
        CHECK_FALSE(bus->irq.active());
    }
}

TEST_CASE("a DICR flag is acknowledged by writing a one to it")
{
    const LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    constexpr u32 MASTER_ENABLE = 1u << 23;
    const u32 flag = 1u << (24 + OTC_CHANNEL);
    const u32 enable = 1u << (16 + OTC_CHANNEL);

    bus->write32(DICR, MASTER_ENABLE | enable);
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 2);
    bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    REQUIRE((bus->read32(DICR) & flag) != 0);

    // Writing zeros leaves it raised — the opposite of I_STAT.
    bus->write32(DICR, MASTER_ENABLE | enable);
    CHECK((bus->read32(DICR) & flag) != 0);

    bus->write32(DICR, MASTER_ENABLE | enable | flag);
    CHECK((bus->read32(DICR) & flag) == 0);
    // And the computed master flag follows it down.
    CHECK((bus->read32(DICR) & (1u << 31)) == 0);
}

TEST_CASE("forcing the interrupt needs neither a flag nor an enable")
{
    const LooseBus bus;
    constexpr u32 FORCE = 1u << 15;

    CHECK((bus->read32(DICR) & (1u << 31)) == 0);
    bus->write32(DICR, FORCE);
    CHECK((bus->read32(DICR) & (1u << 31)) != 0);
}

TEST_CASE("the controller registers read back what was written")
{
    const LooseBus bus;

    // DPCR powers up with priorities set and every channel disabled.
    CHECK(bus->read32(DPCR) == 0x07654321);

    bus->write32(madr(GPU_CHANNEL), 0x00001234);
    bus->write32(bcr(GPU_CHANNEL), 0x00420010);
    CHECK(bus->read32(madr(GPU_CHANNEL)) == 0x00001234);
    CHECK(bus->read32(bcr(GPU_CHANNEL)) == 0x00420010);

    // Each channel has its own set, three registers apart.
    CHECK(bus->read32(madr(OTC_CHANNEL)) == 0);
}
