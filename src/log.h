#pragma once

#include <string_view>

// Diagnostics for conditions the emulator noticed but can carry on
// from — an unmapped memory access, say, as opposed to the halts that
// stop execution outright.
//
// It exists so the emulated machine has no dependency on the host's
// logging. The core would otherwise pull in SDL for the sake of
// SDL_Log alone, which would drag a window system into the test
// binary.
//
// Callers format their own message. Building the string is far more
// expensive than the call, so it is left at the call site where it
// can be skipped entirely when there is nothing to report.
void log_message(std::string_view text);
