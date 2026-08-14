#pragma once

#include <array>
#include <string>

#include "bus.h"
#include "types.h"

struct Cpu {
    explicit Cpu(Bus& bus) : bus(bus) { reset(); }

    void reset();
    void step();

    Bus& bus;

    // regs holds the values instructions read; writes go to out_regs
    // and become visible after the step. This models the load delay
    // slot: a load's value lands only after the *next* instruction.
    std::array<u32, 32> regs{};
    std::array<u32, 32> out_regs{};
    u32 pc = 0;
    u32 next_pc = 0;
    u32 current_pc = 0;
    u32 hi = 0;
    u32 lo = 0;
    u32 sr = 0;  // COP0 r12: status register

    u32 load_reg = 0;
    u32 load_value = 0;

    bool halted = false;
    std::string halt_reason;

private:
    void execute(u32 instr);
    void execute_special(u32 instr);
    void execute_cop0(u32 instr);
    void branch(u32 offset);
    u32 reg(u32 index) const { return regs[index]; }
    void set_reg(u32 index, u32 value);
    void schedule_load(u32 index, u32 value);
    void halt(std::string reason);
};
