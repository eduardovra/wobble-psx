#include "disasm.h"

#include <format>
#include <string>

// clang-format off
const char* const REG_NAMES[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};
// clang-format on

namespace {

// The same field accessors the CPU decodes with; see cpu.cpp for the
// instruction layout they carve up.
u32 rs(u32 instr) { return (instr >> 21) & 0x1F; }
u32 rt(u32 instr) { return (instr >> 16) & 0x1F; }
u32 rd(u32 instr) { return (instr >> 11) & 0x1F; }
u32 shamt(u32 instr) { return (instr >> 6) & 0x1F; }
u32 funct(u32 instr) { return instr & 0x3F; }
u32 imm(u32 instr) { return instr & 0xFFFF; }
s32 imm_se(u32 instr) { return static_cast<s16>(instr & 0xFFFF); }

// Signed immediates print as hex like everything else here, because in
// this domain they are nearly always addresses or register offsets —
// 0x1010 says "memory control" where 4112 says nothing. The sign goes
// outside the digits so a negative stack offset still reads at a
// glance.
std::string signed_hex(s32 value)
{
    if (value < 0) {
        return std::format("-0x{:X}", -static_cast<s64>(value));
    }
    return std::format("0x{:X}", value);
}

const char* reg(u32 index) { return REG_NAMES[index]; }

// Where a branch lands: the offset counts instructions from the delay
// slot, which is the instruction after this one.
u32 branch_target(u32 instr, u32 pc)
{
    return pc + 4 + static_cast<u32>(imm_se(instr) << 2);
}

// A jump keeps the top four bits of the delay slot's address and
// supplies the rest, so it cannot leave its 256 MB region.
u32 jump_target(u32 instr, u32 pc)
{
    return ((pc + 4) & 0xF0000000) | ((instr & 0x3FFFFFF) << 2);
}

std::string with_operands(const char* mnemonic, const std::string& operands)
{
    // A fixed mnemonic column keeps operands aligned down a trace,
    // which is most of what makes one readable in bulk.
    return std::format("{:<7} {}", mnemonic, operands);
}

std::string rd_rs_rt(const char* mnemonic, u32 instr)
{
    return with_operands(
        mnemonic,
        std::format(
            "${}, ${}, ${}", reg(rd(instr)), reg(rs(instr)), reg(rt(instr))));
}

std::string rt_rs_imm(const char* mnemonic, u32 instr, bool sign_extended)
{
    std::string value;
    if (sign_extended) {
        value = signed_hex(imm_se(instr));
    } else {
        value = std::format("0x{:X}", imm(instr));
    }
    return with_operands(
        mnemonic,
        std::format("${}, ${}, {}", reg(rt(instr)), reg(rs(instr)), value));
}

// The load and store forms, which all address as offset(base).
std::string memory(const char* mnemonic, u32 instr)
{
    return with_operands(mnemonic,
                         std::format("${}, {}(${})",
                                     reg(rt(instr)),
                                     signed_hex(imm_se(instr)),
                                     reg(rs(instr))));
}

std::string shift(const char* mnemonic, u32 instr)
{
    return with_operands(
        mnemonic,
        std::format(
            "${}, ${}, {}", reg(rd(instr)), reg(rt(instr)), shamt(instr)));
}

// Shifts by a register put the amount in rs, which reverses the usual
// operand order.
std::string shift_variable(const char* mnemonic, u32 instr)
{
    return with_operands(
        mnemonic,
        std::format(
            "${}, ${}, ${}", reg(rd(instr)), reg(rt(instr)), reg(rs(instr))));
}

std::string branch_two(const char* mnemonic, u32 instr, u32 pc)
{
    return with_operands(mnemonic,
                         std::format("${}, ${}, 0x{:08X}",
                                     reg(rs(instr)),
                                     reg(rt(instr)),
                                     branch_target(instr, pc)));
}

std::string branch_one(const char* mnemonic, u32 instr, u32 pc)
{
    return with_operands(
        mnemonic,
        std::format("${}, 0x{:08X}", reg(rs(instr)), branch_target(instr, pc)));
}

std::string disassemble_special(u32 instr)
{
    switch (funct(instr)) {
    case 0x00:
        // An all-zero word is a shift of $zero by nothing, which is
        // how MIPS spells "do nothing".
        if (instr == 0) {
            return "nop";
        }
        return shift("sll", instr);
    case 0x02:
        return shift("srl", instr);
    case 0x03:
        return shift("sra", instr);
    case 0x04:
        return shift_variable("sllv", instr);
    case 0x06:
        return shift_variable("srlv", instr);
    case 0x07:
        return shift_variable("srav", instr);
    case 0x08:
        return with_operands("jr", std::format("${}", reg(rs(instr))));
    case 0x09:
        // The link register is almost always $ra, and is left implicit
        // when it is.
        if (rd(instr) == 31) {
            return with_operands("jalr", std::format("${}", reg(rs(instr))));
        }
        return with_operands(
            "jalr", std::format("${}, ${}", reg(rd(instr)), reg(rs(instr))));
    case 0x0C:
        return "syscall";
    case 0x0D:
        return "break";
    case 0x10:
        return with_operands("mfhi", std::format("${}", reg(rd(instr))));
    case 0x11:
        return with_operands("mthi", std::format("${}", reg(rs(instr))));
    case 0x12:
        return with_operands("mflo", std::format("${}", reg(rd(instr))));
    case 0x13:
        return with_operands("mtlo", std::format("${}", reg(rs(instr))));
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: {
        // Multiply and divide write hi and lo rather than a register,
        // so they name only their two inputs.
        const char* names[] = {"mult", "multu", "div", "divu"};
        return with_operands(
            names[funct(instr) - 0x18],
            std::format("${}, ${}", reg(rs(instr)), reg(rt(instr))));
    }
    case 0x20:
        return rd_rs_rt("add", instr);
    case 0x21:
        return rd_rs_rt("addu", instr);
    case 0x22:
        return rd_rs_rt("sub", instr);
    case 0x23:
        return rd_rs_rt("subu", instr);
    case 0x24:
        return rd_rs_rt("and", instr);
    case 0x25:
        return rd_rs_rt("or", instr);
    case 0x26:
        return rd_rs_rt("xor", instr);
    case 0x27:
        return rd_rs_rt("nor", instr);
    case 0x2A:
        return rd_rs_rt("slt", instr);
    case 0x2B:
        return rd_rs_rt("sltu", instr);
    default:
        return std::format(".word   0x{:08X}", instr);
    }
}

// Opcode 0x01 is four conditional branches sharing one opcode, told
// apart by the rt field rather than a funct code.
std::string disassemble_bcond(u32 instr, u32 pc)
{
    switch (rt(instr)) {
    case 0x00:
        return branch_one("bltz", instr, pc);
    case 0x01:
        return branch_one("bgez", instr, pc);
    case 0x10:
        return branch_one("bltzal", instr, pc);
    case 0x11:
        return branch_one("bgezal", instr, pc);
    default:
        return std::format(".word   0x{:08X}", instr);
    }
}

std::string disassemble_cop0(u32 instr)
{
    switch (rs(instr)) {
    case 0x00:
        return with_operands(
            "mfc0", std::format("${}, cop0_r{}", reg(rt(instr)), rd(instr)));
    case 0x04:
        return with_operands(
            "mtc0", std::format("${}, cop0_r{}", reg(rt(instr)), rd(instr)));
    case 0x10:
        if (funct(instr) == 0x10) {
            return "rfe";
        }
        return std::format(".word   0x{:08X}", instr);
    default:
        return std::format(".word   0x{:08X}", instr);
    }
}

}  // namespace

std::string disassemble(u32 instr, u32 pc)
{
    switch (instr >> 26) {
    case 0x00:
        return disassemble_special(instr);
    case 0x01:
        return disassemble_bcond(instr, pc);
    case 0x02:
        return with_operands("j",
                             std::format("0x{:08X}", jump_target(instr, pc)));
    case 0x03:
        return with_operands("jal",
                             std::format("0x{:08X}", jump_target(instr, pc)));
    case 0x04:
        return branch_two("beq", instr, pc);
    case 0x05:
        return branch_two("bne", instr, pc);
    case 0x06:
        return branch_one("blez", instr, pc);
    case 0x07:
        return branch_one("bgtz", instr, pc);
    case 0x08:
        return rt_rs_imm("addi", instr, true);
    case 0x09:
        return rt_rs_imm("addiu", instr, true);
    case 0x0A:
        return rt_rs_imm("slti", instr, true);
    case 0x0B:
        return rt_rs_imm("sltiu", instr, true);
    case 0x0C:
        return rt_rs_imm("andi", instr, false);
    case 0x0D:
        return rt_rs_imm("ori", instr, false);
    case 0x0E:
        return rt_rs_imm("xori", instr, false);
    case 0x0F:
        // LUI has no source register: the immediate is the whole
        // operand.
        return with_operands(
            "lui", std::format("${}, 0x{:X}", reg(rt(instr)), imm(instr)));
    case 0x10:
        return disassemble_cop0(instr);
    case 0x12:
        // The geometry coprocessor, which this emulator does not
        // implement. Shown as an opcode rather than decoded, so a
        // trace still lines up.
        return with_operands("cop2",
                             std::format("0x{:07X}", instr & 0x1FFFFFF));
    case 0x20:
        return memory("lb", instr);
    case 0x21:
        return memory("lh", instr);
    case 0x22:
        return memory("lwl", instr);
    case 0x23:
        return memory("lw", instr);
    case 0x24:
        return memory("lbu", instr);
    case 0x25:
        return memory("lhu", instr);
    case 0x26:
        return memory("lwr", instr);
    case 0x28:
        return memory("sb", instr);
    case 0x29:
        return memory("sh", instr);
    case 0x2A:
        return memory("swl", instr);
    case 0x2B:
        return memory("sw", instr);
    case 0x2E:
        return memory("swr", instr);
    case 0x32:
        return memory("lwc2", instr);
    case 0x3A:
        return memory("swc2", instr);
    default:
        return std::format(".word   0x{:08X}", instr);
    }
}
