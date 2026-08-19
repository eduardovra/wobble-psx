# wobble-psx

A PlayStation 1 emulator, named after the console's famously wobbly
vertex graphics.

It boots the retail BIOS to its shell — the main menu, with the memory
card and CD player entries, drawn and responding to the controller —
and it reads discs. A game image in the drive is found, identified and
booted: the BIOS reads `SYSTEM.CNF` off the disc, loads the executable
it names, and runs it.

```sh
./build-rel/wobble SCPH1001.BIN "Ridge Racer (USA).zip"
```

The zip a game is distributed in can be given as it is — there is no
need to unpack it first, or to know which of the files inside is the
one to open. A `.chd`, `.cue` or `.bin` already sitting on disk works
just as well, with `--disc` if the extension is one the BIOS image also
uses.

The disc is unpacked into the system's temporary directory, and only
the part the drive can actually read: the cue, and the one image it
names first. A game with music on it leaves its audio tracks in the
archive, since nothing here can play them yet — which for Ridge Racer
is 3.6 MB unpacked instead of 446 MB.

Nothing ever deletes that copy, which is the reason for putting it
where every system already empties by itself. So a game is unpacked
once and then read straight from disk until the next reboot, and no
policy here has to decide what of the user's to throw away. Deleting
it by hand costs only the unpacking; `TMPDIR` puts it somewhere else.

```
Ridge Racer     3.6 MB    0.02 s to unpack
Crash Bandicoot  603 MB    2.8 s to unpack
```

Crash is the awkward case and cannot be helped: its disc is a single
Mode 2 track, so the whole of it is the part that gets read.

BIN/CUE is the format underneath, since that is what a PlayStation disc
actually is — 2352-byte sectors with their own sync and header, and any
Redbook audio in tracks of its own. A bare `.iso` of 2048-byte blocks
is accepted too and has its missing sync and headers synthesised.

A `.chd` needs none of this and is the better way to keep a game. It is
the same disc compressed in hunks of a few sectors each, with an index
in front saying where every one of them landed, so the drive can seek
in it where it lies — one hunk decompressed per sector read, and no
unpacking at all. Crash is 443 MB as a CHD against the 603 MB it
occupies unpacked, and it starts the moment it is asked to. The
decompression is [libchdr](https://github.com/rtissera/libchdr)'s,
which is what MAME reads the format with.

Games boot, run their startup, draw and make sound. Symphony of the
Night plays its opening movie and waits at its title screen; Resident
Evil 2 reaches its menu. None is playable yet.

## Video

The MDEC decodes. It is the console's video codec with the front half
taken off — dequantise, inverse DCT, colour space conversion — and the
Huffman coding a movie is really stored in is undone by the CPU before
anything reaches it, which is how the console did it too.

Both monochrome depths and both colour ones are there, along with the
quantisation and scale tables a game loads before a movie, and both DMA
channels, so a frame can arrive and leave without the CPU touching a
word of it. `mdec/frame` from ps1-tests decodes to the photograph a
console decodes it to, `mdec/step-by-step-log` — which feeds the
decoder by hand and reads the status register at every step — comes out
with the same image, and `mdec/movie` plays. A snapshot of the last of
those cannot be compared with the console's, since it is an animation
and the two are never at the same instant of it.

What is not the same is the last bit of arithmetic. Against the
24-bit reference frame, 88% of the bytes are what hardware produced and
97% are within one of it — the inverse DCT here keeps its full product
and rounds once, where the hardware's rounds somewhere inside, and
nobody has worked out where. It is a step of brightness on some pixels
of a photograph and cannot be seen in a moving one.

A movie is shown in the display mode the still pictures never use:
three bytes to the pixel rather than a packed halfword, which the GPU
cannot draw into and only carries. The video signal reads VRAM as bytes
when GP1(08h) asks it to, so what the MDEC decoded arrives on screen as
the colours it decoded rather than as every third pixel of them.

The timing is not the hardware's either. A real MDEC decodes while the
data-in channel is still feeding it and stalls that channel when it
gets ahead; here a word is decoded the moment it is written, because a
DMA transfer runs to completion inside the store that starts one.
Software sees the same words in the same order, and sooner.

## Sound

The SPU plays. All twenty-four voices decode their ADPCM out of the
half megabyte of sample memory, resample it to the pitch each was
given, shape it with the envelope software described, and mix down to
a stereo pair every 768 master cycles — which is 44100 times a second,
the rate the whole console is clocked from.

What is not there is the wet path and the corners: no reverb, no noise
generator, no pitch modulation, no volume sweeps (a fade is heard at
full volume rather than fading), and no CD audio, so a game whose music
is Redbook tracks on the disc still plays its sound effects and nothing
else. Each is marked in `src/spu.cpp` where it would have gone.

A host with no sound card is not a failure to start: the SPU runs
either way and takes the same time to play a sound, since a game that
paces itself against its own music must not run differently for being
inaudible.

A program can also be handed to the machine directly, with no disc
involved. `--exe` boots the BIOS and then puts a PS-EXE where the
disc's program would have gone, which is how homebrew and the test
suites written for emulator development are run.

```sh
./build-rel/wobble SCPH1001.BIN --exe program.exe
```

## Controls

The keyboard is the controller in the first socket: the arrow keys are
the d-pad, `X` `S` `Z` `A` are cross, circle, square and triangle,
`Q` `W` `E` `R` are the shoulder buttons, and Enter and right shift are
start and select. The second socket is empty, and so is every memory
card slot.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. SDL3, Dear ImGui and miniz
are fetched and built automatically by CMake (nothing is installed to the
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
look at it, `pad` holds a controller button down, `trace` shows the last
instructions retired, `profile` reports where the time and the memory
accesses went, `screen` writes what the display is showing as a PPM,
`audio` writes what the SPU has played as a WAV, `exe` loads a PS-EXE,
and `save`/`load` take snapshots. Numbers are hex unless prefixed
with `#`.

`exe` is what makes the test suites written for emulator development
usable, since those ship as PS-EXEs and report their results as text
the BIOS prints — so a run is a diff against the log the same program
produced on real hardware:

```sh
./build-rel/wobble-dbg SCPH1001.BIN -c "exe cpu/cop/cop.exe" \
    -c "run #8000000" -c "tty" | diff - cpu/cop/psx.log
```

So the shell can be driven without a window, which is how the controller
is tested:

```sh
./build-rel/wobble-dbg SCPH1001.BIN -c "frames #2400" \
    -c "pad down down" -c "frames #10" -c "pad up down" \
    -c "frames #40" -c "screen menu.ppm"
```

So the picture the machine is producing can be had without a window:

```sh
./build-rel/wobble-dbg SCPH1001.BIN -c "frames #320" -c "screen boot.ppm"
```

And so can the sound, which is the only way to hear a run on a machine
with no sound card — or to measure one, since a WAV is what every
plotting script already opens:

```sh
./build-rel/wobble-dbg SCPH1001.BIN -c "audio on" -c "frames #400" \
    -c "audio boot.wav"
```

Nothing the debugger does changes what the machine does — a test
asserts that a traced, watched, profiled run reaches the same state as
a plain one, since everything else here depends on it.

The Release build matters here too, and for the same reason: getting
past the kernel's startup takes tens of millions of instructions, which
the sanitisers turn from seconds into minutes.

## The GDB stub

`--gdb` serves the GDB remote protocol on loopback instead of reading
commands, so gdb — and anything that drives gdb, such as VS Code — can
debug the emulated CPU:

```sh
./build-rel/wobble-dbg SCPH1001.BIN --gdb 3333
gdb-multiarch -ex "target remote 127.0.0.1:3333"
```

The stub says what machine it is when asked, so no `set architecture`
is needed anywhere. That is not a convenience: a frontend that
connects first and runs its configuration afterwards would otherwise
read the register file as some other machine's and show a pc of zero.

Any `-c` commands run before the stub starts listening, so a session
can be set up first — run to an address, load a save state — and then
handed to gdb. Breakpoints and watchpoints are the debugger's own,
shared with the text commands; memory reads answer only for RAM and
the BIOS, because reading a hardware register is an event the device
reacts to and gdb reads memory constantly.

For VS Code, the [Native Debug][native-debug] extension plus a
`.vscode` configuration make it one key: F5 builds the release tree,
makes sure a stub is listening, waits for it, and attaches. The
launch configuration, the task that starts the stub and the script it
runs are in `.vscode/`, which a global gitignore keeps out of the
repository — so they are per-checkout unless force-added.

There are three configurations. "disassembly" is the one to step with;
"run and attach" does the build-and-start above; and "attach to
running stub" skips the building and starting entirely, for a stub
already listening — one the task left behind, or one launched by hand
with its own `-c` setup.

Stepping needs a word of explanation, because it does not work on ROM
code by default. Stepping by source line means asking gdb where the
current line ends, and a BIOS carries no line numbers to answer with,
so it refuses: "cannot find bounds of current function", which a
frontend reports as a failed step.

What it will do is step to the end of the function it is in, and a
function four bytes long is left by executing exactly one instruction.
So `tools/make-bios-elf.py` gives every instruction its own four-byte
function symbol, and the ordinary step buttons become instruction
stepping. The one surprise is that a call is stepped into rather than
over, since arriving at the callee also counts as leaving. The
"disassembly" configuration additionally has VS Code's Disassembly
View (right-click in the editor to show it), which is worth having
open while stepping.

The same script is what gives the debugger a file to open at all: the
ROM bytes unchanged, with a header saying they load at `0xBFC00000` on
a MIPS I, and named frames in place of `?? ()`. Homebrew built with
real symbols needs none of it and steps by source in the ordinary way.
The task generates the file, so it is only worth running by hand to
look at:

```sh
python3 tools/make-bios-elf.py SCPH1001.BIN build-rel/bios.elf
```

A stub that is already up is reused rather than restarted, because
detaching leaves the machine exactly where it was — so gdb can come
and go, and F5 lands back in the same boot instead of starting it
over. The "restart gdb stub" task is there for when a fresh machine
is what is wanted. Nothing else on the port is ever touched: if
something that is not the stub holds 3333, the launch stops and says
so rather than killing a process for standing on a number.

Retail games and the BIOS are raw binaries, so gdb debugs them at the
assembly level; a homebrew ELF built with symbols gets source-level
stepping by adding it to gdb with `file` (or `executable` in the
config above).

[native-debug]: https://marketplace.visualstudio.com/items?itemName=webfreak.debug

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
the hardware test programs are for. `exe` now runs them; making them
pass is tracked in
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
