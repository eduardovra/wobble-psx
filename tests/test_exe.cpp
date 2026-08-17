#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "console.h"
#include "exe.h"

namespace {

// The header offsets, written out again rather than shared with the
// loader: what is being pinned here is the file format, and a test
// that read the same constants as the code could not tell whether
// either of them had it right.
constexpr u32 HEADER_INITIAL_PC = 0x10;
constexpr u32 HEADER_INITIAL_GP = 0x14;
constexpr u32 HEADER_LOAD_ADDRESS = 0x18;
constexpr u32 HEADER_BODY_SIZE = 0x1C;
constexpr u32 HEADER_BSS_ADDRESS = 0x28;
constexpr u32 HEADER_BSS_SIZE = 0x2C;
constexpr u32 HEADER_STACK_BASE = 0x30;
constexpr u32 HEADER_STACK_OFFSET = 0x34;

// A PS-EXE built by hand. Nothing is assembled into the body: what is
// under test is where the bytes land and what the registers hold
// afterwards, not anything the program would do once it ran.
struct ExeFile {
    std::vector<u8> bytes = std::vector<u8>(Exe::HEADER_SIZE, 0);

    ExeFile()
    {
        constexpr std::string_view MAGIC = "PS-X EXE";
        std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin());
    }

    void set_word(u32 offset, u32 value)
    {
        for (u32 i = 0; i < 4; i++) {
            bytes[offset + i] = static_cast<u8>(value >> (i * 8));
        }
    }

    // Appends a body and declares it in the header, which is what the
    // loader reads to know how much to copy.
    void set_body(const std::vector<u8>& body)
    {
        bytes.resize(Exe::HEADER_SIZE);
        bytes.insert(bytes.end(), body.begin(), body.end());
        set_word(HEADER_BODY_SIZE, static_cast<u32>(body.size()));
    }
};

// The console is megabytes of arrays, too big for the stack a test
// runs on. No BIOS is loaded into it: start_exe places a program and
// points the CPU at it without running anything, which is the half of
// the loader that needs no ROM image.
struct Loaded {
    std::unique_ptr<Console> console = std::make_unique<Console>();

    Loaded() { console->reset(); }

    u8 ram(u32 address) const { return console->bus.ram[address & 0x1FFFFF]; }
};

}  // namespace

TEST_CASE("a header is read into its fields")
{
    ExeFile file;
    file.set_word(HEADER_INITIAL_PC, 0x80011BF8);
    file.set_word(HEADER_INITIAL_GP, 0x80012345);
    file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
    file.set_word(HEADER_STACK_BASE, 0x801FFFF0);
    file.set_word(HEADER_STACK_OFFSET, 0x10);
    file.set_body({1, 2, 3, 4});

    const std::optional<Exe> exe = parse_exe(file.bytes);
    REQUIRE(exe.has_value());
    CHECK(exe->initial_pc == 0x80011BF8);
    CHECK(exe->initial_gp == 0x80012345);
    CHECK(exe->load_address == 0x80010000);
    CHECK(exe->body == std::vector<u8>{1, 2, 3, 4});

    // The two stack fields are simply added.
    CHECK(exe->stack_pointer() == 0x80200000);
}

TEST_CASE("a header naming no stack gets the kernel's own")
{
    ExeFile file;
    file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
    file.set_body({0});

    const std::optional<Exe> exe = parse_exe(file.bytes);
    REQUIRE(exe.has_value());
    CHECK(exe->stack_pointer() == 0x801FFF00);
}

TEST_CASE("what is not a PS-EXE is refused")
{
    SUBCASE("the magic has to be there")
    {
        ExeFile file;
        file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
        file.set_body({0});
        file.bytes[0] = 'X';
        CHECK(!parse_exe(file.bytes).has_value());
    }

    SUBCASE("a file shorter than the header is not one")
    {
        const std::vector<u8> bytes(16, 0);
        CHECK(!parse_exe(bytes).has_value());
    }

    SUBCASE("a body running past the end of the file is a truncated one")
    {
        ExeFile file;
        file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
        file.set_body({1, 2, 3, 4});
        file.set_word(HEADER_BODY_SIZE, 64);
        CHECK(!parse_exe(file.bytes).has_value());
    }

    SUBCASE("a program has to fit in RAM")
    {
        ExeFile file;
        file.set_word(HEADER_LOAD_ADDRESS, 0x801FF000);
        file.set_body(std::vector<u8>(0x2000, 0xAB));
        CHECK(!parse_exe(file.bytes).has_value());
    }

    SUBCASE("and so does its bss")
    {
        ExeFile file;
        file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
        file.set_word(HEADER_BSS_ADDRESS, 0x801FF000);
        file.set_word(HEADER_BSS_SIZE, 0x2000);
        file.set_body({0});
        CHECK(!parse_exe(file.bytes).has_value());
    }
}

TEST_CASE("a program is placed where its header says")
{
    ExeFile file;
    file.set_word(HEADER_INITIAL_PC, 0x80010008);
    file.set_word(HEADER_INITIAL_GP, 0x80018000);
    file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
    file.set_word(HEADER_STACK_BASE, 0x801F0000);
    file.set_body({0xDE, 0xAD, 0xBE, 0xEF});

    const std::optional<Exe> exe = parse_exe(file.bytes);
    REQUIRE(exe.has_value());

    Loaded loaded;
    REQUIRE(start_exe(*loaded.console, *exe));

    CHECK(loaded.ram(0x80010000) == 0xDE);
    CHECK(loaded.ram(0x80010003) == 0xEF);

    const Cpu& cpu = loaded.console->cpu;
    CHECK(cpu.pc == 0x80010008);
    CHECK(cpu.next_pc == 0x8001000C);
    CHECK(cpu.regs[28] == 0x80018000);
    CHECK(cpu.regs[29] == 0x801F0000);
    CHECK(cpu.regs[30] == 0x801F0000);

    // The entry state has to survive the first instruction, which ends
    // by copying the pending register bank over the visible one.
    CHECK(cpu.out_regs[29] == 0x801F0000);
}

TEST_CASE("the bss is cleared, since the file does not carry it")
{
    ExeFile file;
    file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
    file.set_word(HEADER_BSS_ADDRESS, 0x80020000);
    file.set_word(HEADER_BSS_SIZE, 0x100);
    file.set_body({1});

    const std::optional<Exe> exe = parse_exe(file.bytes);
    REQUIRE(exe.has_value());

    Loaded loaded;
    loaded.console->bus.write32(0x80020000, 0xFFFFFFFF);
    loaded.console->bus.write32(0x800200FC, 0xFFFFFFFF);
    loaded.console->bus.write32(0x80020100, 0xFFFFFFFF);
    REQUIRE(start_exe(*loaded.console, *exe));

    CHECK(loaded.console->bus.read32(0x80020000) == 0);
    CHECK(loaded.console->bus.read32(0x800200FC) == 0);

    // Exactly the region asked for, and not the word after it.
    CHECK(loaded.console->bus.read32(0x80020100) == 0xFFFFFFFF);
}

TEST_CASE("a load left over from the boot does not reach the program")
{
    ExeFile file;
    file.set_word(HEADER_INITIAL_PC, 0x80010000);
    file.set_word(HEADER_LOAD_ADDRESS, 0x80010000);
    file.set_body({0});

    const std::optional<Exe> exe = parse_exe(file.bytes);
    REQUIRE(exe.has_value());

    Loaded loaded;
    loaded.console->cpu.load_reg = 5;
    loaded.console->cpu.load_value = 0xDEADBEEF;
    REQUIRE(start_exe(*loaded.console, *exe));

    CHECK(loaded.console->cpu.load_reg == 0);
}
