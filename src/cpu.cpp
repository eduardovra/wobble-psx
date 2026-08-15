#include "cpu.h"

#include <format>
#include <utility>

namespace {

constexpr u32 RESET_PC = 0xBFC00000;
constexpr u32 SR_ISOLATE_CACHE = 1 << 16;
constexpr u32 SR_BOOT_VECTORS = 1 << 22;  // BEV: vectors in ROM
constexpr u32 CAUSE_BRANCH_DELAY = 1u << 31;

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
    cause = 0;
    epc = 0;
    in_delay_slot = false;
    branching = false;
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

    // BIOS putchar: A-function 0x3C / B-function 0x3D, char in $a0
    const u32 masked_pc = current_pc & 0x1FFFFFFF;
    const bool is_putchar_a = masked_pc == 0xA0 && regs[9] == 0x3C;
    const bool is_putchar_b = masked_pc == 0xB0 && regs[9] == 0x3D;
    if (is_putchar_a || is_putchar_b) {
        tty += static_cast<char>(regs[4]);
    }

    const u32 instr = bus.read32(pc);
    pc = next_pc;
    next_pc += 4;

    in_delay_slot = branching;
    branching = false;

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
    branching = true;
}

void Cpu::raise_exception(Exception code)
{
    // mode/interrupt bit pairs in SR act as a 3-deep stack: push
    const u32 mode = sr & 0x3F;
    sr = (sr & ~0x3Fu) | ((mode << 2) & 0x3F);

    cause = static_cast<u32>(code) << 2;
    epc = current_pc;
    if (in_delay_slot) {
        epc -= 4;
        cause |= CAUSE_BRANCH_DELAY;
    }

    const bool use_rom_vector = sr & SR_BOOT_VECTORS;
    pc = use_rom_vector ? 0xBFC00180 : 0x80000080;
    next_pc = pc + 4;  // exception entry has no delay slot
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
    case 0x01: {  // BLTZ / BGEZ / BLTZAL / BGEZAL
        const u32 cond = rt(instr);
        const bool is_bgez = cond & 1;
        const bool links = (cond & 0x1E) == 0x10;
        const bool is_negative = static_cast<s32>(reg(rs(instr))) < 0;
        if (links) {
            set_reg(31, next_pc);
        }
        if (is_bgez != is_negative) {
            branch(imm_se(instr));
        }
        break;
    }
    case 0x02:  // J
        next_pc = (pc & 0xF0000000) | ((instr & 0x3FFFFFF) << 2);
        branching = true;
        break;
    case 0x03:  // JAL
        set_reg(31, next_pc);
        next_pc = (pc & 0xF0000000) | ((instr & 0x3FFFFFF) << 2);
        branching = true;
        break;
    case 0x04:  // BEQ
        if (reg(rs(instr)) == reg(rt(instr))) {
            branch(imm_se(instr));
        }
        break;
    case 0x05:  // BNE
        if (reg(rs(instr)) != reg(rt(instr))) {
            branch(imm_se(instr));
        }
        break;
    case 0x06:  // BLEZ
        if (static_cast<s32>(reg(rs(instr))) <= 0) {
            branch(imm_se(instr));
        }
        break;
    case 0x07:  // BGTZ
        if (static_cast<s32>(reg(rs(instr))) > 0) {
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
    case 0x0A: {  // SLTI
        const s32 a = static_cast<s32>(reg(rs(instr)));
        const s32 b = static_cast<s32>(imm_se(instr));
        set_reg(rt(instr), a < b ? 1 : 0);
        break;
    }
    case 0x0B:  // SLTIU
        set_reg(rt(instr), reg(rs(instr)) < imm_se(instr) ? 1 : 0);
        break;
    case 0x0C:  // ANDI
        set_reg(rt(instr), reg(rs(instr)) & imm(instr));
        break;
    case 0x0D:  // ORI
        set_reg(rt(instr), reg(rs(instr)) | imm(instr));
        break;
    case 0x0E:  // XORI
        set_reg(rt(instr), reg(rs(instr)) ^ imm(instr));
        break;
    case 0x0F:  // LUI
        set_reg(rt(instr), imm(instr) << 16);
        break;
    case 0x10:
        execute_cop0(instr);
        break;
    case 0x20: {  // LB
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        const s8 value = static_cast<s8>(bus.read8(addr));
        schedule_load(rt(instr), static_cast<u32>(value));
        break;
    }
    case 0x21: {  // LH
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 2 != 0) {
            halt(std::format("unaligned LH at {:08X}", current_pc));
            break;
        }
        const s16 value = static_cast<s16>(bus.read16(addr));
        schedule_load(rt(instr), static_cast<u32>(value));
        break;
    }
    case 0x23: {  // LW
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 4 != 0) {
            halt(std::format("unaligned LW at {:08X}", current_pc));
            break;
        }
        schedule_load(rt(instr), bus.read32(addr));
        break;
    }
    case 0x24: {  // LBU
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        schedule_load(rt(instr), bus.read8(addr));
        break;
    }
    case 0x25: {  // LHU
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 2 != 0) {
            halt(std::format("unaligned LHU at {:08X}", current_pc));
            break;
        }
        schedule_load(rt(instr), bus.read16(addr));
        break;
    }
    case 0x28: {  // SB
        if (sr & SR_ISOLATE_CACHE) {
            break;
        }
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        bus.write8(addr, static_cast<u8>(reg(rt(instr))));
        break;
    }
    case 0x29: {  // SH
        if (sr & SR_ISOLATE_CACHE) {
            break;
        }
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 2 != 0) {
            halt(std::format("unaligned SH at {:08X}", current_pc));
            break;
        }
        bus.write16(addr, static_cast<u16>(reg(rt(instr))));
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
    case 0x02:  // SRL
        set_reg(rd(instr), reg(rt(instr)) >> shamt(instr));
        break;
    case 0x03: {  // SRA (arithmetic: keeps the sign bit)
        const s32 value = static_cast<s32>(reg(rt(instr)));
        set_reg(rd(instr), static_cast<u32>(value >> shamt(instr)));
        break;
    }
    case 0x04:  // SLLV (only the low 5 bits of rs count)
        set_reg(rd(instr), reg(rt(instr)) << (reg(rs(instr)) & 0x1F));
        break;
    case 0x06:  // SRLV
        set_reg(rd(instr), reg(rt(instr)) >> (reg(rs(instr)) & 0x1F));
        break;
    case 0x07: {  // SRAV
        const s32 value = static_cast<s32>(reg(rt(instr)));
        const u32 amount = reg(rs(instr)) & 0x1F;
        set_reg(rd(instr), static_cast<u32>(value >> amount));
        break;
    }
    case 0x08:  // JR
        next_pc = reg(rs(instr));
        branching = true;
        break;
    case 0x09:  // JALR
        set_reg(rd(instr), next_pc);
        next_pc = reg(rs(instr));
        branching = true;
        break;
    case 0x0C:  // SYSCALL
        raise_exception(Exception::Syscall);
        break;
    case 0x10:  // MFHI
        set_reg(rd(instr), hi);
        break;
    case 0x11:  // MTHI
        hi = reg(rs(instr));
        break;
    case 0x12:  // MFLO
        set_reg(rd(instr), lo);
        break;
    case 0x13:  // MTLO
        lo = reg(rs(instr));
        break;
    case 0x18: {  // MULT
        const s64 a = static_cast<s32>(reg(rs(instr)));
        const s64 b = static_cast<s32>(reg(rt(instr)));
        const u64 product = static_cast<u64>(a * b);
        hi = static_cast<u32>(product >> 32);
        lo = static_cast<u32>(product);
        break;
    }
    case 0x19: {  // MULTU
        const u64 a = reg(rs(instr));
        const u64 b = reg(rt(instr));
        const u64 product = a * b;
        hi = static_cast<u32>(product >> 32);
        lo = static_cast<u32>(product);
        break;
    }
    case 0x1A: {  // DIV (special-cased results, never traps)
        const s32 n = static_cast<s32>(reg(rs(instr)));
        const s32 d = static_cast<s32>(reg(rt(instr)));
        if (d == 0) {
            hi = static_cast<u32>(n);
            lo = (n >= 0) ? 0xFFFFFFFF : 1;
        } else if (n == INT32_MIN && d == -1) {
            hi = 0;
            lo = 0x80000000;
        } else {
            lo = static_cast<u32>(n / d);
            hi = static_cast<u32>(n % d);
        }
        break;
    }
    case 0x1B: {  // DIVU
        const u32 n = reg(rs(instr));
        const u32 d = reg(rt(instr));
        if (d == 0) {
            hi = n;
            lo = 0xFFFFFFFF;
        } else {
            lo = n / d;
            hi = n % d;
        }
        break;
    }
    case 0x20: {  // ADD (traps on signed overflow)
        const s32 a = static_cast<s32>(reg(rs(instr)));
        const s32 b = static_cast<s32>(reg(rt(instr)));
        s32 result = 0;
        if (__builtin_add_overflow(a, b, &result)) {
            halt(std::format("ADD overflow at {:08X}", current_pc));
            break;
        }
        set_reg(rd(instr), static_cast<u32>(result));
        break;
    }
    case 0x21:  // ADDU
        set_reg(rd(instr), reg(rs(instr)) + reg(rt(instr)));
        break;
    case 0x23:  // SUBU
        set_reg(rd(instr), reg(rs(instr)) - reg(rt(instr)));
        break;
    case 0x24:  // AND
        set_reg(rd(instr), reg(rs(instr)) & reg(rt(instr)));
        break;
    case 0x25:  // OR
        set_reg(rd(instr), reg(rs(instr)) | reg(rt(instr)));
        break;
    case 0x26:  // XOR
        set_reg(rd(instr), reg(rs(instr)) ^ reg(rt(instr)));
        break;
    case 0x27:  // NOR
        set_reg(rd(instr), ~(reg(rs(instr)) | reg(rt(instr))));
        break;
    case 0x2A: {  // SLT
        const s32 a = static_cast<s32>(reg(rs(instr)));
        const s32 b = static_cast<s32>(reg(rt(instr)));
        set_reg(rd(instr), a < b ? 1 : 0);
        break;
    }
    case 0x2B:  // SLTU
        set_reg(rd(instr), reg(rs(instr)) < reg(rt(instr)) ? 1 : 0);
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
    case 0x00:  // MFC0 (value arrives via the load delay slot)
        switch (rd(instr)) {
        case 12:
            schedule_load(rt(instr), sr);
            break;
        case 13:
            schedule_load(rt(instr), cause);
            break;
        case 14:
            schedule_load(rt(instr), epc);
            break;
        default:
            halt(std::format("MFC0 cop0_r{} at {:08X}",
                             rd(instr),
                             current_pc));
            break;
        }
        break;
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
    case 0x10:  // RFE: pop the mode stack pushed by the exception
        if ((instr & 0x3F) != 0x10) {
            halt(std::format("unhandled COP0 op {:08X} at {:08X}",
                             instr,
                             current_pc));
            break;
        }
        sr = (sr & ~0xFu) | ((sr >> 2) & 0xF);
        break;
    default:
        halt(std::format("unhandled COP0 {:08X} at {:08X}",
                         instr,
                         current_pc));
        break;
    }
}
