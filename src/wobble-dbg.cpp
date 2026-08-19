// The headless emulator: the same console the window drives, with a
// debugger on the front and no display attached.
//
//   wobble-dbg [bios] [game] [-c "command"]... [--gdb port] [script]
//
// Commands come from -c arguments, a script file, or standard input,
// whichever was given. Standard input is read only when neither of the
// others was: a -c session that then waited on a terminal would hang
// every script that used one, which is the way this is mostly driven.
//
// --gdb serves the GDB remote protocol instead of reading commands.
// Any -c commands run first, so a session can be set up — run to an
// address, load a state — before gdb takes over.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "console.h"
#include "debugger.h"
#include "gdbstub.h"

namespace {

// Runs one line and prints whatever it produced. Returns false when
// the line asked to stop.
bool run_line(Console& console, Debugger& debugger, const std::string& line)
{
    if (line == "quit" || line == "q") {
        return false;
    }
    const std::string output = debugger.execute(console, line);
    std::fputs(output.c_str(), stdout);
    std::fflush(stdout);
    return true;
}

// The usage the header comment describes, for a reader holding only
// the binary. What the commands themselves are is `help`'s business,
// since the debugger is the one that knows them.
void print_usage()
{
    std::puts(
        R"(wobble-dbg - the PlayStation 1 emulator, headless, with a debugger

Usage: wobble-dbg [bios] [game] [-c "command"]... [--gdb port] [script]

  bios              a BIOS image to boot (default: SCPH1001.BIN)
  game              a .zip, .cue or .chd to put in the drive
  script            a file of commands, one per line

Options:
  -c "command"      run a command; may be given more than once
  --disc PATH       put PATH in the drive, whatever it is named
                    (.bin and .iso images need this, since a .bin is
                    as likely to be the BIOS as a disc)
  --gdb PORT        serve the GDB remote protocol on 127.0.0.1:PORT
                    instead of reading commands
  -h, --help        show this and exit

Commands come from -c, a script file, or standard input, whichever was
given. Run `help` for the list of them.

Examples:
  wobble-dbg SCPH1001.BIN -c "frames #320" -c "screen boot.ppm"
  wobble-dbg SCPH1001.BIN game.chd -c "frames #900" -c "screen logo.ppm"
  wobble-dbg SCPH1001.BIN -c "exe test.exe" -c "run #8000000" -c "tty"
  wobble-dbg SCPH1001.BIN --gdb 3333)");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string bios_path = "SCPH1001.BIN";
    std::vector<std::string> commands;
    std::string script_path;
    std::string disc_path;
    int gdb_port = 0;

    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            print_usage();
            return 0;
        }
        if (argument == "-c" && i + 1 < argc) {
            commands.emplace_back(argv[++i]);
        } else if (argument == "--disc" && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (argument.ends_with(".zip") || argument.ends_with(".cue") ||
                   argument.ends_with(".chd")) {
            // A game named without the flag, the way the window takes
            // one. Only the extensions a disc is unambiguously named
            // with: anything else is a script of commands, and a disc
            // read as one is pages of nonsense before it says so.
            disc_path = argument;
        } else if (argument == "--gdb" && i + 1 < argc) {
            gdb_port = std::atoi(argv[++i]);
        } else if (argument.ends_with(".BIN") || argument.ends_with(".bin")) {
            bios_path = argument;
        } else if (argument.starts_with("-")) {
            // Anything else beginning with a dash is a mistyped
            // option, not a script to run: taking it for one would
            // fail much later and say the wrong thing about why.
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            print_usage();
            return 1;
        } else {
            script_path = argument;
        }
    }

    // The console is megabytes of arrays, too big for the stack.
    static Console console;
    if (!console.bus.load_bios(bios_path)) {
        std::fprintf(
            stderr, "failed to load BIOS from %s\n", bios_path.c_str());
        return 1;
    }
    // Before the reset, so the drive has the disc in it from the
    // moment the BIOS first asks what is there.
    if (!disc_path.empty() && !console.bus.cdrom.disc.load(disc_path)) {
        std::fprintf(
            stderr, "failed to load the disc from %s\n", disc_path.c_str());
        return 1;
    }
    console.reset();

    Debugger debugger;

    for (const std::string& command : commands) {
        if (!run_line(console, debugger, command)) {
            return 0;
        }
    }
    if (gdb_port > 0 && gdb_port <= 0xFFFF) {
        GdbStub stub(console, debugger);
        std::printf("gdb stub listening on 127.0.0.1:%d\n", gdb_port);
        std::fflush(stdout);
        if (!stub.serve(static_cast<u16>(gdb_port))) {
            std::fprintf(stderr, "could not listen on port %d\n", gdb_port);
            return 1;
        }
        return 0;
    }

    if (!commands.empty() && script_path.empty()) {
        return 0;  // -c said everything it wanted to
    }

    if (!script_path.empty()) {
        std::ifstream script(script_path);
        if (!script) {
            std::fprintf(
                stderr, "failed to open script %s\n", script_path.c_str());
            return 1;
        }
        std::string line;
        while (std::getline(script, line)) {
            if (!run_line(console, debugger, line)) {
                return 0;
            }
        }
        return 0;
    }

    // Nothing was asked for on the command line, so take commands
    // from wherever standard input comes from — a terminal or a pipe,
    // without caring which.
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!run_line(console, debugger, line)) {
            break;
        }
    }
    return 0;
}
