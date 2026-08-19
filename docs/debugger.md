# The debugger

`wobble-dbg` is the same emulated console with no display attached and
a debugger on the front. Commands are text in and text out, so a
question about the machine is a line of input rather than a program
written against the emulator.

```sh
./build/wobble-dbg SCPH1001.BIN -c "until BFC00150 #5000" -c "regs"
./build/wobble-dbg SCPH1001.BIN session.txt      # a script
./build/wobble-dbg SCPH1001.BIN                  # or interactively
```

A disc goes in the drive the same way it does for the window — named
on the command line, or with `--disc` when the extension does not say
what it is — so a game can be booted with no display attached:

```sh
./build-rel/wobble-dbg SCPH1001.BIN game.chd \
    -c "frames #650" -c "screen logo.ppm"
```

The `disc` command puts one in a machine that is already running.

`--help` describes the command line; `help`, once inside, lists the
commands. Briefly: `run`/`runc`/`frames`/`until` move the machine,
`break` and `watch` stop it, `regs`/`mem`/`disas`/`dev` look at it,
`pad` holds a controller button down, `trace` shows the last
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
