# wobble-psx

A PlayStation 1 emulator, named after the console's famously wobbly
vertex graphics.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. SDL3 and Dear ImGui are
fetched and built automatically by CMake (nothing is installed to the
system). On Debian/Ubuntu the X11/Wayland dev headers SDL needs come
from `build-essential libx11-dev libxext-dev libwayland-dev
libxkbcommon-dev libgl1-mesa-dev`.

```sh
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/wobble SCPH1001.BIN
```

Run the emulator from a Release build. A Debug build turns on the
address and undefined-behaviour sanitisers, which is what you want for
the tests and is about **46 times slower** — 5.4 seconds of console
time takes 1 second in Release and 47 in Debug, so the window runs at a
tenth of real speed there rather than at the display's rate.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # for the tests
cmake --build build
```

## The debugger

`wobble-dbg` is the same emulated console with no display attached and
a debugger on the front. Commands are text in and text out, so a
question about the machine is a line of input rather than a program
written against the emulator.

```sh
./build/wobble-dbg SCPH1001.BIN -c "until BFC00150 #5000" -c "regs"
./build/wobble-dbg SCPH1001.BIN session.txt      # a script
./build/wobble-dbg SCPH1001.BIN                  # or interactively
```

`help` lists the commands. Briefly: `run`/`runc`/`frames`/`until` move
the machine, `break` and `watch` stop it, `regs`/`mem`/`disas`/`dev`
look at it, `trace` shows the last instructions retired, `profile`
reports where the time and the memory accesses went, `screen` writes
what the display is showing as a PPM, and `save`/`load` take snapshots.
Numbers are hex unless prefixed with `#`.

So the picture the machine is producing can be had without a window:

```sh
./build-rel/wobble-dbg SCPH1001.BIN -c "frames #320" -c "screen boot.ppm"
```

Nothing the debugger does changes what the machine does — a test
asserts that a traced, watched, profiled run reaches the same state as
a plain one, since everything else here depends on it.

The Release build matters here too, and for the same reason: getting
past the kernel's startup takes tens of millions of instructions, which
the sanitisers turn from seconds into minutes.

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

Validating the interpreter against a real test ROM is tracked in
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

## References

- [psx-spx](https://psx-spx.consoledev.net/) — hardware documentation
