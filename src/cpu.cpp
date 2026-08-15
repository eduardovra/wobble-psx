#include "cpu.h"

#include <format>
#include <utility>

namespace {

// Execution starts in the BIOS ROM, at the top of the uncached KSEG1
// region.
constexpr u32 RESET_PC = 0xBFC00000;

// What one instruction costs the master clock. A flat charge is a
// deliberate placeholder: the real chip pays extra for memory waits,
// multiply/divide latency and cache misses, and refining any of that
// means returning a per-instruction figure from step() instead of
// this one. Games need a clock that advances at roughly the right
// rate far more than they need it to be exact.
constexpr u32 CYCLES_PER_INSTRUCTION = 1;

// Status register bits. Isolating the cache redirects stores into the
// scratchpad instead of memory; the BIOS does this while it clears the
// cache at boot.
constexpr u32 SR_INTERRUPT_ENABLE = 1 << 0;  // IEc, the current level
constexpr u32 SR_ISOLATE_CACHE = 1 << 16;
constexpr u32 SR_BOOT_VECTORS = 1 << 22;  // BEV: vectors in ROM

constexpr u32 CAUSE_EXC_CODE = 0x7C;  // bits 6..2
constexpr u32 CAUSE_BRANCH_DELAY = 1u << 31;

// SR and Cause both carry an 8-bit interrupt field at bits 15..8:
// which lines are pending, and which of them are unmasked.
constexpr u32 INTERRUPT_SHIFT = 8;
constexpr u32 INTERRUPT_MASK = 0xFF;

// Of those eight lines the R3000A offers, the PSX wires up exactly
// one: IP2, driven by the interrupt controller, which every device in
// the machine shares. IP0 and IP1 are software-settable and used as a
// way to post an interrupt to yourself; the rest are unconnected.
constexpr u32 CAUSE_HARDWARE_INTERRUPT = 1 << 10;  // IP2
constexpr u32 CAUSE_SOFTWARE_INTERRUPTS = 0x300;   // IP1..IP0

// MIPS instruction encoding. Every instruction is exactly 32 bits and
// its fields sit at fixed bit positions, so decoding is a shift and a
// mask rather than a parse. Three layouts share those positions:
//
//   R (register)   op(6) rs(5) rt(5) rd(5) shamt(5) funct(6)
//   I (immediate)  op(6) rs(5) rt(5) imm(16)
//   J (jump)       op(6) target(26)
//
//   31    26 25  21 20  16 15  11 10   6 5     0
//   +-------+------+------+------+------+-------+
//   |  op   |  rs  |  rt  |  rd  |shamt | funct |  R
//   +-------+------+------+------+------+-------+
//   |  op   |  rs  |  rt  |        imm          |  I
//   +-------+------+------+---------------------+
//   |  op   |               target              |  J
//   +-------+-----------------------------------+
//
// Which fields are meaningful depends on the opcode, so these
// accessors just carve out bit ranges without judging whether the
// field applies. Each register field is 5 bits: 32 registers.
u32 rs(u32 instr) { return (instr >> 21) & 0x1F; }
u32 rt(u32 instr) { return (instr >> 16) & 0x1F; }
u32 rd(u32 instr) { return (instr >> 11) & 0x1F; }

// Shift amount, 0-31, used by the constant-distance shift ops.
u32 shamt(u32 instr) { return (instr >> 6) & 0x1F; }

// The I-format immediate, zero-extended (logical ops treat it so).
u32 imm(u32 instr) { return instr & 0xFFFF; }

// The same immediate sign-extended, for arithmetic and for the
// address offsets of loads, stores and branches. The round trip
// through s16 is what does the extension.
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
    bad_vaddr = 0;
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

u32 Cpu::step()
{
    if (halted) {
        return 0;
    }

    current_pc = pc;

    // The BIOS exposes its kernel calls as jumps to fixed addresses,
    // with the function number in $t1. Watching for the putchar calls
    // here gets us the boot log without emulating a serial port.
    // BIOS putchar: A-function 0x3C / B-function 0x3D, char in $a0
    const u32 masked_pc = current_pc & 0x1FFFFFFF;
    const bool is_putchar_a = masked_pc == 0xA0 && regs[9] == 0x3C;
    const bool is_putchar_b = masked_pc == 0xB0 && regs[9] == 0x3D;
    if (is_putchar_a || is_putchar_b) {
        tty += static_cast<char>(regs[4]);
    }

    // A branch set `branching` on the previous step, which makes this
    // instruction its delay slot. Settled before anything can raise an
    // exception, since that is what decides the reported epc.
    in_delay_slot = branching;
    branching = false;

    // The load issued by the previous instruction lands now; the
    // current instruction's own write (below) overrides it. It
    // completes even if this step faults — the load already happened.
    set_reg(load_reg, load_value);
    load_reg = 0;
    load_value = 0;

    if (interrupt_pending()) {
        // Taken in place of the instruction, which re-runs on return.
        raise_exception(Exception::Interrupt);
    } else if (current_pc % 4 != 0) {
        // A jump to a misaligned address faults on the fetch itself.
        raise_address_error(Exception::AddressLoad, current_pc);
    } else {
        const u32 instr = bus.read32(current_pc);

        // Advance both counters before executing, so a branch taken by
        // this instruction rewrites next_pc while pc — already
        // pointing at the delay slot — is left alone.
        pc = next_pc;
        next_pc += 4;

        execute(instr);
    }

    // Writes made by this instruction become readable from here on.
    regs = out_regs;

    return CYCLES_PER_INSTRUCTION;
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
    // The offset counts instructions, not bytes, so it is stored
    // pre-divided by 4 — shifting it back up buys 16 bits of encoding
    // a ±128 KB reach. It is relative to the delay slot, which pc
    // already points at.
    next_pc = pc + (offset << 2);
    branching = true;
}

void Cpu::raise_exception(Exception code)
{
    // The low 6 bits of SR are three (interrupt-enable, user-mode)
    // pairs acting as a 3-deep stack. Shifting them left by 2 pushes a
    // new entry — zeroed, so the handler runs in kernel mode with
    // interrupts off — and drops the oldest. RFE shifts them back.
    const u32 mode = sr & 0x3F;
    sr = (sr & ~0x3Fu) | ((mode << 2) & 0x3F);

    // Update only the fields this exception owns. The pending-
    // interrupt bits belong to the interrupt controller, so Cause is
    // edited rather than overwritten; BD is rewritten every time so a
    // previous delay-slot exception cannot leave it set.
    cause &= ~(CAUSE_EXC_CODE | CAUSE_BRANCH_DELAY);
    cause |= (static_cast<u32>(code) << 2) & CAUSE_EXC_CODE;

    epc = current_pc;
    if (in_delay_slot) {
        // Returning into a delay slot alone would skip the branch, so
        // epc points at the branch and the handler re-runs both.
        epc -= 4;
        cause |= CAUSE_BRANCH_DELAY;
    }

    const bool use_rom_vector = sr & SR_BOOT_VECTORS;
    pc = use_rom_vector ? 0xBFC00180 : 0x80000080;
    next_pc = pc + 4;  // exception entry has no delay slot
}

void Cpu::raise_address_error(Exception code, u32 addr)
{
    bad_vaddr = addr;
    raise_exception(code);
}

u32 Cpu::cause_register() const
{
    if (!bus.irq.active()) {
        return cause;
    }
    return cause | CAUSE_HARDWARE_INTERRUPT;
}

bool Cpu::interrupt_pending() const
{
    if ((sr & SR_INTERRUPT_ENABLE) == 0) {
        return false;
    }
    const u32 pending = (cause_register() >> INTERRUPT_SHIFT) & INTERRUPT_MASK;
    const u32 enabled = (sr >> INTERRUPT_SHIFT) & INTERRUPT_MASK;
    return (pending & enabled) != 0;
}

void Cpu::halt(std::string reason)
{
    halted = true;
    halt_reason = std::move(reason);
}

// Primary decode. The top 6 bits (31..26) are the opcode, so shifting
// the instruction right by 26 leaves just that field to switch on.
// Two of its values are escapes into a secondary table rather than
// instructions of their own: 0x00 (SPECIAL) and 0x10 (COP0).
void Cpu::execute(u32 instr)
{
    switch (instr >> 26) {
    case 0x00:
        execute_special(instr);
        break;
    case 0x01: {  // BLTZ / BGEZ / BLTZAL / BGEZAL
        // Four instructions packed into one opcode: the rt field is
        // not a register here but a selector. Bit 0 picks the test
        // direction, and 0b10000 in bits 4..1 asks for the link.
        const u32 cond = rt(instr);
        const bool is_bgez = cond & 1;
        const bool links = (cond & 0x1E) == 0x10;
        const bool is_negative = static_cast<s32>(reg(rs(instr))) < 0;
        if (links) {
            set_reg(31, next_pc);
        }
        // BGEZ wants non-negative, BLTZ wants negative: one test
        // serves both when the wanted answer differs by variant.
        if (is_bgez != is_negative) {
            branch(imm_se(instr));
        }
        break;
    }
    case 0x02:  // J
        // The 26-bit target is also an instruction count, so it
        // shifts up by 2 to give 28 bits of address. The missing top
        // 4 bits come from the current pc: a jump cannot leave its
        // 256 MB region — only JR, taking a full register, can.
        next_pc = (pc & 0xF0000000) | ((instr & 0x3FFFFFF) << 2);
        branching = true;
        break;
    case 0x03:  // JAL
        // Same jump, but $ra (r31) keeps the return address. This is
        // how a call is made; the matching return is JR $ra.
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
            raise_exception(Exception::Overflow);
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
        // No instruction can carry a 32-bit constant, so one is built
        // in two steps: LUI puts the immediate in the high half, then
        // an ORI supplies the low half.
        set_reg(rt(instr), imm(instr) << 16);
        break;
    case 0x10:
        execute_cop0(instr);
        break;

    // Loads and stores, the only instructions that touch memory. All
    // of them address as register + sign-extended offset. Loads take
    // effect one instruction later (schedule_load); stores are
    // immediate. Anything wider than a byte must be aligned — real
    // hardware raises an exception, which this emulator does not
    // implement yet, so it halts instead.
    case 0x20: {  // LB
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        const s8 value = static_cast<s8>(bus.read8(addr));
        schedule_load(rt(instr), static_cast<u32>(value));
        break;
    }
    case 0x21: {  // LH
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 2 != 0) {
            raise_address_error(Exception::AddressLoad, addr);
            break;
        }
        const s16 value = static_cast<s16>(bus.read16(addr));
        schedule_load(rt(instr), static_cast<u32>(value));
        break;
    }
    case 0x23: {  // LW
        const u32 addr = reg(rs(instr)) + imm_se(instr);
        if (addr % 4 != 0) {
            raise_address_error(Exception::AddressLoad, addr);
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
            raise_address_error(Exception::AddressLoad, addr);
            break;
        }
        schedule_load(rt(instr), bus.read16(addr));
        break;
    }
    case 0x28: {  // SB
        if (sr & SR_ISOLATE_CACHE) {
            break;  // cache writes, not memory — ignore for now
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
            raise_address_error(Exception::AddressStore, addr);
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
            raise_address_error(Exception::AddressStore, addr);
            break;
        }
        bus.write32(addr, reg(rt(instr)));
        break;
    }
    default:
        halt(std::format(
            "unhandled instruction {:08X} at {:08X}", instr, current_pc));
        break;
    }
}

// Opcode 0x00 is not one instruction but a whole second table. These
// are the R-format instructions — register-to-register arithmetic,
// shifts and the register jumps — which need no immediate field, so
// the freed low 6 bits (the funct field) select among them.
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
        // Jump to a full 32-bit address held in a register. JR $ra is
        // how a function returns.
        next_pc = reg(rs(instr));
        branching = true;
        break;
    case 0x09:  // JALR (JR that saves a return address, like JAL)
        set_reg(rd(instr), next_pc);
        next_pc = reg(rs(instr));
        branching = true;
        break;
    case 0x0C:  // SYSCALL
        raise_exception(Exception::Syscall);
        break;
    // Multiply and divide write the hi/lo pair rather than a general
    // register — a 32x32 product needs 64 bits, and division yields a
    // quotient and a remainder. MFHI/MFLO move the halves back out.
    // Real hardware stalls a reader until the result is ready; here
    // they are instant.
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
        // Divide by zero and INT32_MIN / -1 have no right answer, but
        // the hardware still produces defined garbage instead of an
        // exception — compilers rely on not having to check.
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
            raise_exception(Exception::Overflow);
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
        halt(std::format(
            "unhandled SPECIAL {:08X} at {:08X}", instr, current_pc));
        break;
    }
}

// Opcode 0x10 reaches coprocessor 0, the system control unit that
// holds the status, exception and (on other MIPS chips) MMU state.
// The R3000A defines four coprocessor slots; the PlayStation wires
// COP0 to system control and COP2 to the GTE, the geometry engine
// that transforms 3D vertices. COP1 (an FPU) and COP3 are absent.
//
// The rs field, a register number in ordinary instructions, is the
// operation selector here: move from / move to the coprocessor, or
// a coprocessor-specific op such as RFE.
void Cpu::execute_cop0(u32 instr)
{
    switch (rs(instr)) {
    case 0x00:  // MFC0 (value arrives via the load delay slot)
        switch (rd(instr)) {
        case 8:
            schedule_load(rt(instr), bad_vaddr);
            break;
        case 12:
            schedule_load(rt(instr), sr);
            break;
        case 13:
            schedule_load(rt(instr), cause_register());
            break;
        case 14:
            schedule_load(rt(instr), epc);
            break;
        default:
            halt(std::format("MFC0 cop0_r{} at {:08X}", rd(instr), current_pc));
            break;
        }
        break;
    case 0x04:  // MTC0
        switch (rd(instr)) {
        case 12:
            sr = reg(rt(instr));
            break;
        case 13: {
            // Only the two software interrupt bits are writable. The
            // rest of Cause is the hardware's to report, and IP2 in
            // particular is cleared at the interrupt controller, never
            // here — the BIOS still writes the whole register, so the
            // other bits are dropped rather than refused.
            const u32 value = reg(rt(instr));
            cause &= ~CAUSE_SOFTWARE_INTERRUPTS;
            cause |= value & CAUSE_SOFTWARE_INTERRUPTS;
            break;
        }
        default:
            // breakpoint registers etc. — the BIOS zeroes them
            if (reg(rt(instr)) != 0) {
                halt(std::format(
                    "MTC0 cop0_r{} at {:08X}", rd(instr), current_pc));
            }
            break;
        }
        break;
    case 0x10:  // RFE: pop the mode stack pushed by the exception
        // Not a move at all — the funct field distinguishes the
        // coprocessor's own operations, of which RFE is the only one
        // the BIOS uses.
        if ((instr & 0x3F) != 0x10) {
            halt(std::format(
                "unhandled COP0 op {:08X} at {:08X}", instr, current_pc));
            break;
        }
        sr = (sr & ~0xFu) | ((sr >> 2) & 0xF);
        break;
    default:
        halt(std::format("unhandled COP0 {:08X} at {:08X}", instr, current_pc));
        break;
    }
}
