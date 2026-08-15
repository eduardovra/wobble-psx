#pragma once

#include <array>
#include <string>

#include "bus.h"
#include "types.h"

// The PlayStation's main CPU: a 33.87 MHz MIPS R3000A, 32-bit, with
// 32 general-purpose registers and no MMU. Everything it can reach —
// RAM, BIOS ROM, hardware registers — goes through the Bus.
//
// Two pieces of R3000A behaviour shape this whole struct, because both
// are visible to software rather than hidden by interlocks:
//
//   Branch delay slot — the instruction *after* a branch always runs,
//   before the jump takes effect. Modelled with two program counters:
//   `pc` is the instruction about to run, `next_pc` the one after it.
//   A branch rewrites next_pc, so the already-fetched delay slot still
//   executes.
//
//   Load delay slot — a load's result is not in its target register
//   for the next instruction; it lands one instruction later. Modelled
//   with regs/out_regs plus a single pending load (see below).
struct Cpu {
    explicit Cpu(Bus& bus) : bus(bus) { reset(); }

    // Puts the CPU back in its power-on state, with pc at the BIOS
    // reset vector.
    void reset();

    // Runs one instruction: fetch at pc, retire the pending load, then
    // execute. Does nothing once halted.
    void step();

    Bus& bus;

    // regs holds the values instructions read; writes go to out_regs
    // and become visible after the step. This models the load delay
    // slot: a load's value lands only after the *next* instruction.
    std::array<u32, 32> regs{};
    std::array<u32, 32> out_regs{};

    u32 pc = 0;          // instruction about to execute
    u32 next_pc = 0;     // the one after it; a branch rewrites this
    u32 current_pc = 0;  // the one executing now, kept for exceptions

    // Result of multiply/divide. They live outside the register file
    // and are read back with MFHI/MFLO.
    u32 hi = 0;
    u32 lo = 0;

    // The three COP0 (system control coprocessor) registers this
    // emulator needs. COP0 has no general-purpose role — it holds
    // privileged state and is accessed only via MFC0/MTC0.
    u32 sr = 0;     // COP0 r12: status register
    u32 cause = 0;  // COP0 r13: exception cause
    u32 epc = 0;    // COP0 r14: exception return address

    // whether the instruction being executed sits in a branch delay
    // slot (an exception there must report the branch's address)
    bool in_delay_slot = false;
    bool branching = false;

    // The load issued by the previous instruction, waiting out its
    // delay slot. Register 0 means "none" — a load into $zero is a
    // no-op anyway, so it needs no separate flag.
    u32 load_reg = 0;
    u32 load_value = 0;

    // Set when execution hits something unimplemented or invalid.
    // The emulator stops rather than silently running wrong code.
    bool halted = false;
    std::string halt_reason;

    // characters the BIOS printed through its putchar kernel calls
    std::string tty;

    // Exception codes as they appear in the Cause register's ExcCode
    // field. Only the ones actually reached so far are listed.
    enum class Exception : u32 {
        Syscall = 0x8,
    };

private:
    // Decodes and runs one instruction. execute() dispatches on the
    // primary opcode and hands the two escape opcodes to the helpers.
    void execute(u32 instr);
    void execute_special(u32 instr);
    void execute_cop0(u32 instr);

    // Jumps to the exception vector, saving the return address in epc.
    void raise_exception(Exception code);

    // offset is a sign-extended 16-bit instruction count, as encoded.
    void branch(u32 offset);

    u32 reg(u32 index) const { return regs[index]; }

    // Writes land in out_regs, so they are not visible to the current
    // instruction — see the regs/out_regs note above.
    void set_reg(u32 index, u32 value);

    // Queues a load result to appear one instruction from now.
    void schedule_load(u32 index, u32 value);

    void halt(std::string reason);
};
