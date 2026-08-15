#pragma once

#include <format>
#include <string_view>
#include <utility>

// Diagnostics for conditions the emulator noticed but can carry on
// from — an unmapped memory access, say, as opposed to the halts that
// stop execution outright.
//
// It exists so the emulated machine has no dependency on the host's
// logging. The core would otherwise pull in SDL for the sake of
// SDL_Log alone, which would drag a window system into the test
// binary.
void log_message(std::string_view text);

template <typename... Args>
void log_format(std::format_string<Args...> fmt, Args&&... args)
{
    log_message(std::format(fmt, std::forward<Args>(args)...));
}
