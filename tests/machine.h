#pragma once

#include <initializer_list>
#include <memory>

#include "bus.h"
#include "cpu.h"
#include "scheduler.h"

// A bus with a clock of its own. Bus is built around a Scheduler
// because a device asked for its state has to answer for the instant
// it was asked; a test wanting one device rather than a whole console
// still has to supply one. It also holds the bus by pointer, because
// two and a half megabytes of arrays is too much for the stack a test
// runs on.
struct LooseBus {
    Scheduler scheduler;
    std::unique_ptr<Bus> bus = std::make_unique<Bus>(scheduler);

    Bus& operator*() const { return *bus; }
    Bus* operator->() const { return bus.get(); }
};

// A CPU with RAM behind it, running a short program assembled into
// RAM. Nothing here touches the BIOS, so the tests need no ROM image
// and stay runnable in CI.
struct Machine {
    static constexpr u32 CODE = 0x00001000;
    static constexpr u32 DATA = 0x00002000;

    LooseBus bus;
    Cpu cpu{*bus};

    // The geometry engine is behind SR's COP2 enable, and a program
    // that wants it switches it on before its first instruction
    // reaches it — so this stands in for that, and the tests below can
    // be about the engine rather than about the bit.
    Machine() { cpu.sr |= 1u << 30; }

    // Places a program at CODE and points the CPU at it, replacing
    // the BIOS reset vector that Cpu starts from.
    void load(std::initializer_list<u32> program)
    {
        u32 addr = CODE;
        for (const u32 instr : program) {
            bus->write32(addr, instr);
            addr += 4;
        }
        cpu.pc = CODE;
        cpu.next_pc = CODE + 4;
        cpu.current_pc = CODE;
    }

    void run(int instructions)
    {
        for (int i = 0; i < instructions; i++) {
            cpu.step();
        }
    }

    u32 reg(u32 index) const { return cpu.regs[index]; }

    // The ExcCode field of Cause, i.e. which exception was taken.
    u32 exc_code() const { return (cpu.cause >> 2) & 0x1F; }

    bool in_branch_delay() const { return (cpu.cause >> 31) != 0; }
};

// Just enough of a MIPS assembler for the tests to read as code. Only
// the forms they use are here.
namespace mips {

// Registers by their conventional ABI names.
constexpr u32 zero = 0;
constexpr u32 a0 = 4;
constexpr u32 t0 = 8;
constexpr u32 t1 = 9;
constexpr u32 t2 = 10;
constexpr u32 t3 = 11;
constexpr u32 ra = 31;

constexpr u32 r_type(u32 rs, u32 rt, u32 rd, u32 shamt, u32 funct)
{
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct;
}

constexpr u32 i_type(u32 op, u32 rs, u32 rt, s32 imm)
{
    const u32 field = static_cast<u32>(imm) & 0xFFFF;
    return (op << 26) | (rs << 21) | (rt << 16) | field;
}

constexpr u32 nop() { return 0; }

constexpr u32 addiu(u32 rt, u32 rs, s32 imm)
{
    return i_type(0x09, rs, rt, imm);
}

constexpr u32 ori(u32 rt, u32 rs, s32 imm) { return i_type(0x0D, rs, rt, imm); }

constexpr u32 lui(u32 rt, s32 imm) { return i_type(0x0F, 0, rt, imm); }

constexpr u32 lb(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x20, rs, rt, offset);
}

constexpr u32 lw(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x23, rs, rt, offset);
}

constexpr u32 sw(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x2B, rs, rt, offset);
}

// The unaligned pairs. Each half names the end of the register it
// transfers, so lwl/lwr and swl/swr are always written together.
constexpr u32 lwl(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x22, rs, rt, offset);
}

constexpr u32 lwr(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x26, rs, rt, offset);
}

constexpr u32 swl(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x2A, rs, rt, offset);
}

constexpr u32 swr(u32 rt, u32 rs, s32 offset)
{
    return i_type(0x2E, rs, rt, offset);
}

constexpr u32 beq(u32 rs, u32 rt, s32 offset)
{
    return i_type(0x04, rs, rt, offset);
}

// The jump target is an instruction count, so it drops its low bits.
constexpr u32 jal(u32 target)
{
    return (0x03u << 26) | ((target >> 2) & 0x3FFFFFF);
}

constexpr u32 addu(u32 rd, u32 rs, u32 rt)
{
    return r_type(rs, rt, rd, 0, 0x21);
}

constexpr u32 add(u32 rd, u32 rs, u32 rt)
{
    return r_type(rs, rt, rd, 0, 0x20);
}

constexpr u32 sub(u32 rd, u32 rs, u32 rt)
{
    return r_type(rs, rt, rd, 0, 0x22);
}

// Named with a suffix to stay clear of the POSIX syscall().
constexpr u32 syscall_op() { return r_type(0, 0, 0, 0, 0x0C); }

constexpr u32 mfc0(u32 rt, u32 rd)
{
    return (0x10u << 26) | r_type(0x00, rt, rd, 0, 0);
}

constexpr u32 rfe() { return (0x10u << 26) | r_type(0x10, 0, 0, 0, 0x10); }

}  // namespace mips
