#include "log.h"

#include <cstdio>

void log_message(std::string_view text)
{
    std::fprintf(stderr, "%.*s\n", static_cast<int>(text.size()), text.data());
}
