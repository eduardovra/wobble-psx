#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "types.h"

// Save states, as a single traversal of the machine's state that runs
// in both directions.
//
// Each device describes its state once, by handing its fields to a
// State in order. Saving writes them into the buffer; loading reads
// them back out of it. Because there is only the one description, a
// field cannot be saved and then not loaded, which is the way this
// sort of code usually rots — the two halves drift apart and a state
// silently restores something stale.
//
// The format is the fields back to back with no names or tags, so it
// is only readable by the exact build that wrote it. A version number
// at the front makes a mismatch an error rather than a mystery.
struct State {
    static constexpr u32 MAGIC = 0x42424F57;  // "WOBB"
    static constexpr u32 VERSION = 5;

    bool saving = true;
    std::vector<u8> bytes;
    std::size_t position = 0;

    // False once a read has run off the end of the buffer, which is
    // how a truncated or foreign file is caught.
    bool ok = true;

    void raw(void* data, std::size_t size);

    // Anything flat: integers, enumerations, bools. Deliberately not
    // pointers — a saved pointer would be meaningless on reload.
    template <typename T> void operator()(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "state fields must be flat data");
        static_assert(!std::is_pointer_v<T>, "a pointer cannot be saved");
        raw(&value, sizeof(T));
    }

    template <typename T, std::size_t N>
    void operator()(std::array<T, N>& values)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "state arrays must hold flat data");
        raw(values.data(), sizeof(T) * N);
    }

    // A queue with no fixed depth. The count goes in front so a load
    // knows how much to take, and a count larger than what is left of
    // the buffer is a foreign file rather than a long queue — reading
    // it would ask for the memory the file claims instead of the
    // memory it has.
    template <typename T> void operator()(std::vector<T>& values)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "state vectors must hold flat data");
        u32 count = static_cast<u32>(values.size());
        (*this)(count);
        if (!saving) {
            if (!ok || count > (bytes.size() - position) / sizeof(T)) {
                ok = false;
                return;
            }
            values.resize(count);
        }
        raw(values.data(), sizeof(T) * count);
    }

    void operator()(std::string& text);
};
