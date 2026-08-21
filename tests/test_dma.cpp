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
    LooseBus bus;
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 4);

    SUBCASE("DPCR still disabled leaves the channel armed but idle")
    {
        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
        bus.settle();
        CHECK(bus->read32(SCRATCH) == 0);
        // Still armed: enabling it in DPCR is what sets it going.
        CHECK((bus->read32(chcr(OTC_CHANNEL)) & (1u << 24)) != 0);

        bus->write32(DPCR, ALL_CHANNELS_ENABLED);
        bus.settle();
        CHECK(bus->read32(SCRATCH) != 0);
    }

    SUBCASE("a manual transfer waits for its trigger as well")
    {
        bus->write32(DPCR, ALL_CHANNELS_ENABLED);
        bus.settle();
        bus->write32(chcr(OTC_CHANNEL), (1u << 24) | DECREASING);
        bus.settle();
        CHECK(bus->read32(SCRATCH) == 0);

        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
        bus.settle();
        CHECK(bus->read32(SCRATCH) != 0);
    }
}

TEST_CASE("the ordering table channel chains backwards to a terminator")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    // Four entries, built downwards from SCRATCH.
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 4);
    bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    bus.settle();

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
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    // An image transfer into VRAM: three words of command, then one
    // word of pixels.
    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);               // to 0,0
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);  // a 2x1 rectangle
    write_ram(*bus, SCRATCH + 12, 0xBBBBAAAA);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), 4);
    bus->write32(chcr(GPU_CHANNEL), START | TO_DEVICE);
    bus.settle();

    CHECK(bus->gpu.vram[0] == 0xAAAA);
    CHECK(bus->gpu.vram[1] == 0xBBBB);
}

TEST_CASE("a linked list follows its chain until the end marker")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

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
    bus.settle();

    CHECK(bus->gpu.vram[0] == 0xCCCC);
    CHECK(bus->gpu.vram[1] == 0xDDDD);
}

TEST_CASE("an empty linked-list packet still advances the chain")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    const u32 second = SCRATCH + 0x100;
    write_ram(*bus, SCRATCH, (0u << 24) | second);  // no payload
    write_ram(*bus, second + 0, (4u << 24) | 0xFFFFFF);
    write_ram(*bus, second + 4, 0xA0000000);
    write_ram(*bus, second + 8, 0);
    write_ram(*bus, second + 12, (1u << 16) | 2);
    write_ram(*bus, second + 16, 0x22221111);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE | SYNC_LINKED_LIST);
    bus.settle();

    CHECK(bus->gpu.vram[0] == 0x1111);
    CHECK(bus->gpu.vram[1] == 0x2222);
}

TEST_CASE("a request transfer moves blocksize times block count")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);
    write_ram(*bus, SCRATCH + 12, 0x44443333);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), (2u << 16) | 2);  // 2 blocks of 2
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE | (1u << 9));
    bus.settle();

    CHECK(bus->gpu.vram[0] == 0x3333);
    CHECK(bus->gpu.vram[1] == 0x4444);
}

TEST_CASE("a completed channel interrupts only when it is allowed to")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();
    bus->irq.mask = 1u << static_cast<u32>(Interrupt::Dma);

    const u32 flag = 1u << (24 + OTC_CHANNEL);
    const u32 enable = 1u << (16 + OTC_CHANNEL);
    constexpr u32 MASTER_ENABLE = 1u << 23;

    auto run_otc = [&] {
        bus->write32(madr(OTC_CHANNEL), SCRATCH);
        bus->write32(bcr(OTC_CHANNEL), 2);
        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
        bus.settle();
    };

    SUBCASE("with the channel's interrupt disabled, nothing is recorded")
    {
        run_otc();
        // The enable is what lets the flag be set at all, so a
        // transfer that finishes without one leaves nothing behind for
        // a later enable to find.
        CHECK((bus->read32(DICR) & flag) == 0);
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
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();
    constexpr u32 MASTER_ENABLE = 1u << 23;
    const u32 flag = 1u << (24 + OTC_CHANNEL);
    const u32 enable = 1u << (16 + OTC_CHANNEL);

    bus->write32(DICR, MASTER_ENABLE | enable);
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 2);
    bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    bus.settle();
    REQUIRE((bus->read32(DICR) & flag) != 0);

    // Writing zeros leaves it raised — the opposite of I_STAT.
    bus->write32(DICR, MASTER_ENABLE | enable);
    CHECK((bus->read32(DICR) & flag) != 0);

    bus->write32(DICR, MASTER_ENABLE | enable | flag);
    CHECK((bus->read32(DICR) & flag) == 0);
    // And the computed master flag follows it down.
    CHECK((bus->read32(DICR) & (1u << 31)) == 0);
}

TEST_CASE("a write that raises the master flag interrupts on its own")
{
    LooseBus bus;
    constexpr u32 MASTER_ENABLE = 1u << 23;
    bus->irq.mask = 1u << static_cast<u32>(Interrupt::Dma);

    // A flag with its channel enabled but the master switched off: the
    // line is down until the write that brings it up.
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();
    bus->write32(DICR, 1u << (16 + OTC_CHANNEL));
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 2);
    bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    bus.settle();
    REQUIRE_FALSE(bus->irq.active());

    bus->write32(DICR, MASTER_ENABLE | (1u << (16 + OTC_CHANNEL)));
    CHECK(bus->irq.active());
}

TEST_CASE("a register is written a byte at a time as well as whole")
{
    LooseBus bus;
    constexpr u32 MASTER_ENABLE = 1u << 23;
    const u32 flag = 1u << (24 + OTC_CHANNEL);

    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 2);

    // DICR's enables live in its third byte, which is how a game
    // switches one channel's interrupt on and off around a transfer it
    // wants to hear about.
    const u32 enables = Dma::BASE + 0x74 + 2;
    bus->write8(enables, (MASTER_ENABLE | (1u << (16 + OTC_CHANNEL))) >> 16);
    CHECK(bus->read8(enables) == (0x80 | (1u << OTC_CHANNEL)));

    bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    bus.settle();
    REQUIRE((bus->read32(DICR) & flag) != 0);

    // And a byte write that names none of the flags leaves them alone,
    // rather than acknowledging every one it did not mean to touch.
    bus->write8(enables, 0x80);
    CHECK((bus->read32(DICR) & flag) != 0);

    // What it does not do is keep the bytes it passed over. The
    // register takes the whole word the bus carried, so the enable it
    // just cleared is cleared and not merged back in.
    CHECK((bus->read32(DICR) & (1u << (16 + OTC_CHANNEL))) == 0);
}

TEST_CASE("forcing the interrupt needs neither a flag nor an enable")
{
    LooseBus bus;
    constexpr u32 FORCE = 1u << 15;

    CHECK((bus->read32(DICR) & (1u << 31)) == 0);
    bus->write32(DICR, FORCE);
    CHECK((bus->read32(DICR) & (1u << 31)) != 0);
}

TEST_CASE("the controller registers read back what was written")
{
    LooseBus bus;

    // DPCR powers up with priorities set and every channel disabled.
    CHECK(bus->read32(DPCR) == 0x07654321);

    bus->write32(madr(GPU_CHANNEL), 0x00001234);
    bus->write32(bcr(GPU_CHANNEL), 0x00420010);
    CHECK(bus->read32(madr(GPU_CHANNEL)) == 0x00001234);
    CHECK(bus->read32(bcr(GPU_CHANNEL)) == 0x00420010);

    // Each channel has its own set, three registers apart.
    CHECK(bus->read32(madr(OTC_CHANNEL)) == 0);
}

TEST_CASE("the ordering table channel ignores everything it is told")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    // Channel 6 is built to walk backwards through RAM writing a
    // chain, and has no settings. Software asking for the opposite of
    // each of them in turn gets the same table either way.
    const u32 contrary = START | TO_DEVICE | SYNC_LINKED_LIST;
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 4);
    bus->write32(chcr(OTC_CHANNEL), contrary);
    bus.settle();

    CHECK(bus->read32(SCRATCH) == SCRATCH - 4);
    CHECK(bus->read32(SCRATCH - 4) == SCRATCH - 8);
    CHECK(bus->read32(SCRATCH - 12) == 0xFFFFFF);

    // And the register does not remember being asked: the bits that
    // are not wired to anything read back as the channel was built,
    // which is backwards and nothing else.
    CHECK(bus->read32(chcr(OTC_CHANNEL)) == DECREASING);
}

TEST_CASE("a chain with no end leaves the channel running")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();
    bus->write32(DICR, (1u << 23) | (1u << (16 + GPU_CHANNEL)));

    // A packet with no payload whose next pointer is itself. The
    // console follows it for ever, transferring nothing, while the
    // CPU carries on beside it.
    write_ram(*bus, SCRATCH, SCRATCH);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(chcr(GPU_CHANNEL), START | TO_DEVICE | SYNC_LINKED_LIST);
    bus.settle();

    // So as far as software can see it never finished: the enable bit
    // is still up, and no interrupt was raised to say otherwise.
    CHECK((bus->read32(chcr(GPU_CHANNEL)) & (1u << 24)) != 0);
    CHECK((bus->read32(DICR) & (1u << (24 + GPU_CHANNEL))) == 0);
}

TEST_CASE("a device asking for the bus starts a transfer software did not")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);
    write_ram(*bus, SCRATCH + 12, 0xBBBBAAAA);

    // Enabled, in the sync mode that waits for a trigger, but never
    // triggered. The GPU is asking for the bus, and that is the other
    // thing that starts one.
    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), 4);
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE);
    bus.settle();

    CHECK(bus->gpu.vram[0] == 0xAAAA);
    CHECK(bus->gpu.vram[1] == 0xBBBB);

    // The ordering table's channel has no device behind it to ask, so
    // there it really is the trigger that starts one.
    bus->write32(madr(OTC_CHANNEL), SCRATCH);
    bus->write32(bcr(OTC_CHANNEL), 4);
    bus->write32(chcr(OTC_CHANNEL), 1u << 24);
    bus.settle();
    CHECK(bus->read32(SCRATCH) == 0xA0000000);
}

TEST_CASE("a chopped transfer gives the bus back between its windows")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    // Eight words to the GPU in one Manual burst, chopped into windows
    // of two words with sixty-four clocks of CPU between them.
    constexpr u32 CHOPPING = 1u << 8;
    constexpr u32 WINDOW_TWO_WORDS = 1u << 16;   // 1 SHL 1
    constexpr u32 WINDOW_SIXTY_FOUR = 6u << 20;  // 1 SHL 6
    const u32 chopped =
        START | TO_DEVICE | CHOPPING | WINDOW_TWO_WORDS | WINDOW_SIXTY_FOUR;

    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 4);
    write_ram(*bus, SCRATCH + 12, 0xBBBBAAAA);
    write_ram(*bus, SCRATCH + 16, 0xDDDDCCCC);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), 5);
    bus->write32(chcr(GPU_CHANNEL), chopped);

    // One turn of the controller moves one window and no more: two
    // words of the command have gone and no pixel has yet.
    bus.settle(1);
    CHECK(bus->read32(madr(GPU_CHANNEL)) == SCRATCH + 8);
    CHECK((bus->read32(chcr(GPU_CHANNEL)) & (1u << 24)) != 0);
    CHECK(bus->gpu.vram[0] == 0);

    // The window it promised the CPU is real time and not nothing:
    // the controller is not due back until the far side of it.
    const u64 due = bus.scheduler.next_deadline_for(EventKind::Dma);
    CHECK(due - bus->dma_hold_until >= 64);

    // Left to run, the rest arrives as it would have without.
    bus.settle();
    CHECK(bus->gpu.vram[0] == 0xAAAA);
    CHECK(bus->gpu.vram[3] == 0xDDDD);
    CHECK((bus->read32(chcr(GPU_CHANNEL)) & (1u << 24)) == 0);
}

TEST_CASE("the channel with the highest priority takes the bus first")
{
    LooseBus bus;

    // Two channels armed at once, both with something to move. Which
    // goes first is DPCR's to say, and nought is its highest. Each
    // nibble is a channel: bit 3 switches it on, the rest are its
    // priority.
    auto arm_both = [&] {
        write_ram(*bus, SCRATCH + 0, 0xA0000000);
        write_ram(*bus, SCRATCH + 4, 0);
        write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);
        write_ram(*bus, SCRATCH + 12, 0x22221111);

        bus->write32(madr(GPU_CHANNEL), SCRATCH);
        bus->write32(bcr(GPU_CHANNEL), 4);
        bus->write32(chcr(GPU_CHANNEL), START | TO_DEVICE);

        bus->write32(madr(OTC_CHANNEL), SCRATCH + 0x100);
        bus->write32(bcr(OTC_CHANNEL), 4);
        bus->write32(chcr(OTC_CHANNEL), START | DECREASING);
    };

    SUBCASE("the ordering table wins when it is given the better one")
    {
        bus->write32(DPCR, 0x08888F88);  // channel 6 at 0, channel 2 at 7
        arm_both();
        bus.settle(1);

        CHECK(bus->read32(SCRATCH + 0x100) == SCRATCH + 0xFC);
        CHECK(bus->gpu.vram[0] == 0);
    }

    SUBCASE("and loses when the GPU is given it instead")
    {
        bus->write32(DPCR, 0x0F888888);  // channel 2 at 0, channel 6 at 7
        arm_both();
        bus.settle(1);

        CHECK(bus->gpu.vram[0] == 0x1111);
        CHECK(bus->read32(SCRATCH + 0x100) == 0);
    }

    SUBCASE("a tie goes to the higher-numbered channel")
    {
        bus->write32(DPCR, ALL_CHANNELS_ENABLED);  // every one at 0
        arm_both();
        bus.settle(1);

        CHECK(bus->read32(SCRATCH + 0x100) == SCRATCH + 0xFC);
        CHECK(bus->gpu.vram[0] == 0);
    }
}

TEST_CASE("a request transfer counts its blocks down where software sees")
{
    LooseBus bus;
    bus->write32(DPCR, ALL_CHANNELS_ENABLED);
    bus.settle();

    write_ram(*bus, SCRATCH + 0, 0xA0000000);
    write_ram(*bus, SCRATCH + 4, 0);
    write_ram(*bus, SCRATCH + 8, (1u << 16) | 2);
    write_ram(*bus, SCRATCH + 12, 0x44443333);

    bus->write32(madr(GPU_CHANNEL), SCRATCH);
    bus->write32(bcr(GPU_CHANNEL), (2u << 16) | 2);  // 2 blocks of 2
    bus->write32(chcr(GPU_CHANNEL), (1u << 24) | TO_DEVICE | (1u << 9));

    // One block gone: BCR has a block left in it and MADR has moved
    // past the words that went.
    bus.settle(1);
    CHECK((bus->read32(bcr(GPU_CHANNEL)) >> 16) == 1);
    CHECK(bus->read32(madr(GPU_CHANNEL)) == SCRATCH + 8);

    bus.settle();
    CHECK((bus->read32(bcr(GPU_CHANNEL)) >> 16) == 0);
    CHECK(bus->gpu.vram[0] == 0x3333);
    CHECK(bus->gpu.vram[1] == 0x4444);
}
