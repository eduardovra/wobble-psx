#pragma once

#include <string>

#include "types.h"

// Conventional MIPS register names, in register-number order. The
// hardware only numbers them; the names are an ABI convention, but
// they are what disassembly and BIOS documentation use.
extern const char* const REG_NAMES[32];

// One instruction as text, in the form the assembler would take.
//
// `pc` is where the instruction sits, and is needed because branches
// encode an offset while jumps encode part of an address — neither
// means anything without knowing where it was read from. Both are
// printed resolved, so a trace can be read without doing the
// arithmetic by hand.
//
// An encoding this emulator does not implement still disassembles if
// it is a real instruction; only genuinely unknown words come back as
// a bare hex value.
std::string disassemble(u32 instr, u32 pc);
