#include "exe.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>

#include "console.h"

namespace {

constexpr std::string_view MAGIC = "PS-X EXE";

// The header fields this loader uses, by byte offset into it. They are
// all little-endian words, and the gaps between them are fields the
// kernel ignores too.
constexpr u32 OFFSET_INITIAL_PC = 0x10;
constexpr u32 OFFSET_INITIAL_GP = 0x14;
constexpr u32 OFFSET_LOAD_ADDRESS = 0x18;
constexpr u32 OFFSET_BODY_SIZE = 0x1C;
constexpr u32 OFFSET_BSS_ADDRESS = 0x28;
constexpr u32 OFFSET_BSS_SIZE = 0x2C;
constexpr u32 OFFSET_STACK_BASE = 0x30;
constexpr u32 OFFSET_STACK_OFFSET = 0x34;

// Where the kernel puts the stack when the header names no address,
// which is what a program built without a linker script gets. Near the
// top of RAM, with a little left above it for the kernel itself.
constexpr u32 DEFAULT_STACK_POINTER = 0x801FFF00;

// The register numbers the entry state is written into. Named because
// $28 and $29 mean nothing at a glance and these three are the whole
// of what a program is handed.
constexpr u32 REG_GP = 28;
constexpr u32 REG_SP = 29;
constexpr u32 REG_FP = 30;

u32 read_word(const std::vector<u8>& bytes, u32 offset)
{
    return u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8) |
        (u32{bytes[offset + 2]} << 16) | (u32{bytes[offset + 3]} << 24);
}

// Turns an address into an offset into the RAM array, or nothing when
// the range named does not lie in RAM. Loading is not the CPU running,
// so this masks the region bits off itself rather than going through
// the bus: the program is put in memory before anything executes, and
// a program that asked to be put anywhere else is a broken one.
std::optional<u32> ram_offset(u32 address, u32 length)
{
    constexpr u32 REGION_BITS = 0x1FFFFFFF;
    const u32 physical = address & REGION_BITS;
    if (physical > Bus::RAM_SIZE || length > Bus::RAM_SIZE - physical) {
        return std::nullopt;
    }
    return physical;
}

}  // namespace

u32 Exe::stack_pointer() const
{
    if (stack_base == 0) {
        return DEFAULT_STACK_POINTER;
    }
    return stack_base + stack_offset;
}

std::optional<Exe> parse_exe(const std::vector<u8>& bytes)
{
    if (bytes.size() < Exe::HEADER_SIZE) {
        return std::nullopt;
    }
    const std::string_view magic(reinterpret_cast<const char*>(bytes.data()),
                                 MAGIC.size());
    if (magic != MAGIC) {
        return std::nullopt;
    }

    Exe exe;
    exe.initial_pc = read_word(bytes, OFFSET_INITIAL_PC);
    exe.initial_gp = read_word(bytes, OFFSET_INITIAL_GP);
    exe.load_address = read_word(bytes, OFFSET_LOAD_ADDRESS);
    exe.bss_address = read_word(bytes, OFFSET_BSS_ADDRESS);
    exe.bss_size = read_word(bytes, OFFSET_BSS_SIZE);
    exe.stack_base = read_word(bytes, OFFSET_STACK_BASE);
    exe.stack_offset = read_word(bytes, OFFSET_STACK_OFFSET);

    // A body that runs past the end of the file is a truncated
    // download, and the half of it that did arrive is not worth
    // running: the header is believed about the size, and the file has
    // to have it.
    const u32 body_size = read_word(bytes, OFFSET_BODY_SIZE);
    if (bytes.size() - Exe::HEADER_SIZE < body_size) {
        return std::nullopt;
    }

    // Both the body and the bss have to land in RAM. Checked here
    // rather than while copying, so a program is either loaded whole
    // or not at all.
    if (!ram_offset(exe.load_address, body_size)) {
        return std::nullopt;
    }
    if (exe.bss_size != 0 && !ram_offset(exe.bss_address, exe.bss_size)) {
        return std::nullopt;
    }

    const auto body_begin = bytes.begin() + Exe::HEADER_SIZE;
    exe.body.assign(body_begin, body_begin + body_size);
    return exe;
}

std::optional<Exe> read_exe(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    return parse_exe(bytes);
}

bool run_to_shell_entry(Console& console, u64 instruction_budget)
{
    Cpu& cpu = console.cpu;
    u64 executed = 0;
    while (executed < instruction_budget && !cpu.halted) {
        const u64 deadline = console.scheduler.next_deadline();
        while (console.scheduler.now < deadline && !cpu.halted &&
               executed < instruction_budget) {
            // Checked before the step rather than after it: `pc` is
            // the instruction about to run, so arriving is seen with
            // the first instruction of the program still unexecuted.
            if (cpu.pc == SHELL_ENTRY) {
                return true;
            }
            console.scheduler.advance(cpu.step());
            executed++;
        }
        console.dispatch_due_events();
    }
    return false;
}

bool start_exe(Console& console, const Exe& exe)
{
    // Both regions are worked out before either is written, so a
    // program that does not fit leaves memory as it found it.
    const auto body_at =
        ram_offset(exe.load_address, static_cast<u32>(exe.body.size()));
    if (!body_at) {
        return false;
    }

    // Empty stays empty: a program with no bss has nothing to clear,
    // which is not the same as one whose bss does not fit.
    std::optional<u32> bss_at;
    if (exe.bss_size != 0) {
        bss_at = ram_offset(exe.bss_address, exe.bss_size);
        if (!bss_at) {
            return false;
        }
    }

    std::copy(
        exe.body.begin(), exe.body.end(), console.bus.ram.begin() + *body_at);
    if (bss_at) {
        std::fill_n(console.bus.ram.begin() + *bss_at, exe.bss_size, u8{0});
    }

    Cpu& cpu = console.cpu;
    cpu.pc = exe.initial_pc;
    cpu.next_pc = cpu.pc + 4;
    cpu.current_pc = cpu.pc;

    // Written to both banks. A register set only in `regs` would be
    // overwritten at the end of the next step, which copies the
    // pending bank over it — see Cpu::step.
    cpu.regs[REG_GP] = exe.initial_gp;
    cpu.regs[REG_SP] = exe.stack_pointer();
    cpu.regs[REG_FP] = cpu.regs[REG_SP];
    cpu.out_regs = cpu.regs;

    // The BIOS reached its entry point by jumping to it, and the delay
    // slot of that jump may have been a load. Dropping it keeps the
    // last instruction of the boot from writing a register of the
    // program that replaced it.
    cpu.load_reg = 0;
    cpu.load_value = 0;
    return true;
}

std::string sideload_exe(Console& console, const std::string& path)
{
    const std::optional<Exe> exe = read_exe(path);
    if (!exe) {
        return std::format("{} is not a PS-EXE this loader can read", path);
    }
    if (!run_to_shell_entry(console, BOOT_INSTRUCTION_BUDGET)) {
        return std::format(
            "the BIOS did not reach {:08X}, so there is no kernel to run "
            "a program on",
            SHELL_ENTRY);
    }
    start_exe(console, *exe);
    return "";
}
