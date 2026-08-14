#include "cpu.h"

#include <format>
#include <utility>

namespace {

constexpr u32 RESET_PC = 0xBFC00000;
constexpr u32 SR_ISOLATE_CACHE = 1 << 16;

u32 rs(u32 instr) { return (instr >> 21) & 0x1F; }
u32 rt(u32 instr) { return (instr >> 16) & 0x1F; }
u32 rd(u32 instr) { return (instr >> 11) & 0x1F; }
u32 shamt(u32 instr) { return (instr >> 6) & 0x1F; }
u32 imm(u32 instr) { return instr & 0xFFFF; }
u32 imm_se(u32 instr)
{
    return static_cast<u32>(static_cast<s16>(instr & 0xFFFF));
}

}  // namespace

void Cpu::reset()
{
    regs.fill(0);
    out_regs.fill(0);
    pc = RESET_PC;
    next_pc = pc + 4;
    current_pc = pc;
    hi = 0;
    lo = 0;
    sr = 0;
    load_reg = 0;
    load_value = 0;
    halted = false;
    halt_reason.clear();
}

void Cpu::step()
{
    if (halted) {
        return;
    }

    current_pc = pc;
    const u32 instr = bus.read32(pc);
    pc = next_pc;
    next_pc += 4;

    // The load issued by the previous instruction lands now; the
    // current instruction's own write (below) overrides it.
    set_reg(load_reg, load_value);
    load_reg = 0;
    load_value = 0;

    execute(instr);

    regs = out_regs;
}

void Cpu::set_reg(u32 index, u32 value)
{
    out_regs[index] = value;
    out_regs[0] = 0;  // $zero is hardwired
}

void Cpu::schedule_load(u32 index, u32 value)
{
    load_reg = index;
    load_value = value;
}

void Cpu::branch(u32 offset)
{
    // offset is relative to the delay slot, which pc points at now
    next_pc = pc + (offset << 2);
}

void Cpu::halt(std::string reason)
{
    halted = true;
    halt_reason = std::move(reason);
}

void Cpu::execute(u32 instr)
{
    switch (instr >> 26) {
    case 0x00:
        execute_special(instr);
        break;
    case 0x02:  // J
        next_pc = (pc & 0xF0000000) | ((instr & 0x3FFFFFF) << 2);
        break;
    case 0x05:  // BNE
        if (reg(rs(instr)) != reg(rt(instr))) {
            branch(imm_se(instr));
        }
        break;
    case 0x08: {  // ADDI (traps on signed overflow)
        const s32 a = static_cast<s32>(reg(rs(instr)));
        const s32 b = static_cast<s32>(imm_se(instr));
        s32 result = 0;
        if (__builtin_add_overflow(a, b, &result)) {
            halt(std::format("ADDI overflow at {:08X}", current_pc));
            break;
        }
        set_reg(rt(instr), static_cast<u32>(result));
        break;
    }
    case 0x09:  // ADDIU
        set_reg(rt(instr), reg(rs(instr)) + imm_se(instr));
        break;
    case 0x0D:  // ORI
        set_reg(rt(instr), reg(rs(instr)) | imm(instr));
        break;
    case 0x0F:  // LUI
        set_reg(rt(instr), imm(instr) << 16);
        break;
    case 0x10:
        execute_cop0(instr);
        break;
    case 0x23: {  // LW
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 4 != 0) {
            halt(std::format("unaligned LW at {:08X}", current_pc));
            break;
        }
        schedule_load(rt(instr), bus.read32(addr));
        break;
    }
    case 0x2B: {  // SW
        if (sr & SR_ISOLATE_CACHE) {
            break;  // cache writes, not memory — ignore for now
        }
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 4 != 0) {
            halt(std::format("unaligned SW at {:08X}", current_pc));
            break;
        }
        bus.write32(addr, reg(rt(instr)));
        break;
    }
    default:
        halt(std::format("unhandled instruction {:08X} at {:08X}",
                         instr,
                         current_pc));
        break;
    }
}

void Cpu::execute_special(u32 instr)
{
    switch (instr & 0x3F) {
    case 0x00:  // SLL (SLL r0,r0,0 is the canonical NOP)
        set_reg(rd(instr), reg(rt(instr)) << shamt(instr));
        break;
    case 0x25:  // OR
        set_reg(rd(instr), reg(rs(instr)) | reg(rt(instr)));
        break;
    default:
        halt(std::format("unhandled SPECIAL {:08X} at {:08X}",
                         instr,
                         current_pc));
        break;
    }
}

void Cpu::execute_cop0(u32 instr)
{
    switch (rs(instr)) {
    case 0x04:  // MTC0
        switch (rd(instr)) {
        case 12:
            sr = reg(rt(instr));
            break;
        default:
            // breakpoint registers etc. — the BIOS zeroes them
            if (reg(rt(instr)) != 0) {
                halt(std::format("MTC0 cop0_r{} at {:08X}",
                                 rd(instr),
                                 current_pc));
            }
            break;
        }
        break;
    default:
        halt(std::format("unhandled COP0 {:08X} at {:08X}",
                         instr,
                         current_pc));
        break;
    }
}
