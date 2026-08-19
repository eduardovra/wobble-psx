# Development

## Builds

Run the emulator from a Release build. A Debug build turns on the
address and undefined-behaviour sanitisers, which is what you want for
the tests and is about **46 times slower** — 5.4 seconds of console
time takes 1 second in Release and 47 in Debug, so the window runs at a
tenth of real speed there rather than at the display's rate.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # for the tests
cmake --build build
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The emulated machine is built as `core`, a library with no dependency
on SDL or any other host library, so the tests link it and run without
a window system or a BIOS image. They assemble short programs straight
into RAM and assert on the resulting CPU state, which covers the parts
that are easy to get subtly wrong: the load and branch delay slots,
what an exception records, the scheduler's timekeeping, what each
instruction costs the clock, and the handshake between a device raising
an interrupt and the CPU taking it.

The devices are tested through the bus rather than by calling into
them, so the register decode and the banking are part of what is
checked. Timing is part of it too, and not incidentally: an answer that
arrives too early is as wrong as one that never comes, since a driver
that clears an interrupt after asking for it will throw away one
delivered inside its own store.

None of that says the machine agrees with the real one, which is what
the hardware test programs are for. The debugger's `exe` command runs
them; making them pass is tracked in
[issue #1](https://github.com/eduardovra/wobble-psx/issues/1).

## Formatting and linting

`clang-format` fixes layout and include ordering; `clang-tidy` looks
for bugs. Both are enforced in CI, and both read their configuration
from the repository root.

```sh
clang-format -i src/*.cpp src/*.h tests/*.cpp tests/*.h
clang-tidy -p build src/*.cpp tests/*.cpp
```

Install them with `apt install clang-format clang-tidy`, or without
root via `pip install clang-format clang-tidy`. CI pins version 18,
because which checks exist and what they flag varies between releases.

A hook runs the same four checks — format, build, test, lint — before
each commit, taking a few seconds on an up-to-date build. Enable it
once per clone:

```sh
git config core.hooksPath .githooks
```

It skips commits that touch neither the build nor the sources, and
`git commit --no-verify` bypasses it for a single commit.
