#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "types.h"

struct Console;
struct Debugger;

// A GDB remote serial protocol server, so gdb-multiarch — and anything
// that drives gdb, such as an IDE — can debug the emulated CPU over a
// socket.
//
// The stub is a translation layer and nothing more: breakpoints,
// watchpoints and the run loop are the Debugger's, shared with the
// text commands, so both interfaces see the same session. Memory reads
// go around the bus (see peek8) because gdb reads memory constantly
// and a debugger that nudges GPU state by looking at it cannot be
// trusted.
//
// Protocol handling is separate from the socket so the tests can speak
// packets to it directly: handle_packet takes one decoded payload and
// returns the reply, and serve() is only framing, checksums and I/O.
struct GdbStub {
    GdbStub(Console& console, Debugger& debugger) :
        console(console),
        debugger(debugger)
    { }

    // Handles one packet payload (the text between $ and #). Returns
    // the reply payload to send, or nullopt when the packet asked to
    // resume execution — the caller then runs the machine via resume()
    // and sends whatever that returns.
    std::optional<std::string> handle_packet(std::string_view payload);

    // Runs until something stops the machine or `interrupted` returns
    // true (the caller polls the socket for gdb's Ctrl-C there).
    // Returns the stop reply to send.
    std::string resume(const std::function<bool()>& interrupted);

    // Accepts connections on 127.0.0.1:port and serves them one at a
    // time until the process ends. Returns false if the port could not
    // be opened.
    bool serve(u16 port);

    // Set by D (detach) and k (kill); serve() drops the client and
    // listens for the next one.
    bool client_done = false;

    Console& console;
    Debugger& debugger;
};
