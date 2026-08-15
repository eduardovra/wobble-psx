#include <memory>

#include <doctest/doctest.h>

#include "bus.h"

namespace {

// An address in the expansion region, which no device here claims.
constexpr u32 UNMAPPED = 0x1F900000;

}  // namespace

TEST_CASE("an unhandled address is reported once, however often it is hit")
{
    const auto bus = std::make_unique<Bus>();

    CHECK(bus->note_unhandled(UNMAPPED));
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));

    // A different missing device still gets its own report.
    CHECK(bus->note_unhandled(UNMAPPED + 4));
}

TEST_CASE("a read and a write to one register are one missing device")
{
    const auto bus = std::make_unique<Bus>();

    bus->read32(UNMAPPED);
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));

    bus->write32(UNMAPPED, 0);
    CHECK_FALSE(bus->note_unhandled(UNMAPPED));
}

TEST_CASE("KUSEG, KSEG0 and KSEG1 are windows onto the same RAM")
{
    const auto bus = std::make_unique<Bus>();
    bus->write32(0x00001000, 0xDEADBEEF);

    CHECK(bus->read32(0x00001000) == 0xDEADBEEF);  // KUSEG
    CHECK(bus->read32(0x80001000) == 0xDEADBEEF);  // KSEG0, cached
    CHECK(bus->read32(0xA0001000) == 0xDEADBEEF);  // KSEG1, uncached

    // Writing through one window is visible from the others.
    bus->write32(0xA0001000, 0x12345678);
    CHECK(bus->read32(0x00001000) == 0x12345678);
}

TEST_CASE("memory is little-endian")
{
    const auto bus = std::make_unique<Bus>();
    bus->write32(0x00001000, 0x12345678);

    CHECK(bus->read8(0x00001000) == 0x78);
    CHECK(bus->read8(0x00001003) == 0x12);
    CHECK(bus->read16(0x00001000) == 0x5678);
}

TEST_CASE("an absent expansion device reads as all ones")
{
    const auto bus = std::make_unique<Bus>();

    CHECK(bus->read8(0x1F000000) == 0xFF);
}
