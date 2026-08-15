#include "savestate.h"

void State::raw(void* data, std::size_t size)
{
    if (saving) {
        const auto* source = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), source, source + size);
        return;
    }

    if (position + size > bytes.size()) {
        // Short buffer: stop rather than read past the end, and leave
        // the rest of the traversal writing nothing.
        ok = false;
        return;
    }
    std::memcpy(data, bytes.data() + position, size);
    position += size;
}

void State::operator()(std::string& text)
{
    // Length first, so loading knows how much to take.
    u32 length = static_cast<u32>(text.size());
    (*this)(length);
    if (!ok) {
        return;
    }

    if (saving) {
        raw(text.data(), length);
        return;
    }

    if (position + length > bytes.size()) {
        ok = false;
        return;
    }
    text.assign(reinterpret_cast<const char*>(bytes.data() + position), length);
    position += length;
}
