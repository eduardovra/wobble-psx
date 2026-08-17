#pragma once

#include <optional>
#include <string>
#include <vector>

#include "types.h"

struct Console;

// A PS-EXE: the only executable format this machine has. The BIOS
// reads one off the disc and jumps to it, and every homebrew program
// and emulator test suite is built as one — so loading a PS-EXE
// directly runs that whole corpus without a drive to read it from.
//
// The file is a 2 KB header and then the program, which the header
// says where to put and where to start. Only a dozen of the header's
// words are used; the rest is padding and a region marker.
//
// Nothing here is a shortcut around the CD-ROM. A sideloaded program
// still needs a booted kernel behind it — it calls the BIOS to print,
// to read the pad, to install its own handlers — which is why loading
// one means running the real boot first and stepping in at the end of
// it, rather than starting a cold machine at the program's entry.
struct Exe {
    static constexpr u32 HEADER_SIZE = 0x800;

    u32 initial_pc = 0;
    u32 initial_gp = 0;

    // Where the body goes, and the zero-filled region after it that
    // the program expects but the file does not carry.
    u32 load_address = 0;
    u32 bss_address = 0;
    u32 bss_size = 0;

    // The stack is given as a base and an offset which are simply
    // added; a file that wants the kernel's own stack leaves both zero
    // rather than naming an address.
    u32 stack_base = 0;
    u32 stack_offset = 0;

    std::vector<u8> body;

    // Where the stack pointer starts, the header's answer or the
    // kernel's default when it gave none.
    u32 stack_pointer() const;
};

// Where the BIOS jumps once it has a kernel and something to run. The
// shell it loads off the ROM lands here, and so does a game the shell
// loads off a disc: it is the one address that means "whatever this
// console was going to execute starts now", which is what makes it the
// place to put a program of our own.
constexpr u32 SHELL_ENTRY = 0x80030000;

// How long to let the boot run before giving up on it ever arriving.
// Reaching the entry point takes a few million instructions on a good
// BIOS image and never on a bad one.
constexpr u64 BOOT_INSTRUCTION_BUDGET = 100'000'000;

// Reads a header and body out of `bytes`; nullopt if it is not a
// PS-EXE, or says its body is longer than the file it came in.
std::optional<Exe> parse_exe(const std::vector<u8>& bytes);

// The same, from a file.
std::optional<Exe> read_exe(const std::string& path);

// Runs the machine until it is about to enter whatever it was going to
// run, and reports whether it got there. Timing is untouched: the CPU
// is let loose only as far as the next deadline, exactly as a normal
// run does it, so the kernel the program inherits is the one a real
// boot would have left.
bool run_to_shell_entry(Console& console, u64 instruction_budget);

// Puts the program in RAM and the CPU at its entry point, which is
// what the kernel's own Exec() does to a program it has just loaded.
// False, having touched nothing, if it does not fit in RAM — which
// parse_exe has already ruled out for anything it returned, so this
// only ever answers for an Exe built by hand.
bool start_exe(Console& console, const Exe& exe);

// The whole of it: read the file, boot the BIOS, hand the program the
// machine. Returns an empty string on success and why not otherwise,
// so a caller has something to print without having to know which of
// the steps above there are.
std::string sideload_exe(Console& console, const std::string& path);
