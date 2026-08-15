// The headless emulator: the same console the window drives, with a
// debugger on the front and no display attached.
//
//   wobble-dbg [bios] [-c "command"]... [script]
//
// Commands come from -c arguments, a script file, or standard input,
// whichever was given. Standard input is read only when neither of the
// others was: a -c session that then waited on a terminal would hang
// every script that used one, which is the way this is mostly driven.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "console.h"
#include "debugger.h"

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

}  // namespace

int main(int argc, char** argv)
{
    std::string bios_path = "SCPH1001.BIN";
    std::vector<std::string> commands;
    std::string script_path;

    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "-c" && i + 1 < argc) {
            commands.emplace_back(argv[++i]);
        } else if (argument.ends_with(".BIN") || argument.ends_with(".bin")) {
            bios_path = argument;
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
    console.reset();

    Debugger debugger;

    for (const std::string& command : commands) {
        if (!run_line(console, debugger, command)) {
            return 0;
        }
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
