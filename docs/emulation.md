# What is emulated

Games boot, run their startup, draw and make sound. Symphony of the
Night plays its opening movie and waits at its title screen; Resident
Evil 2 reaches its menu. None is playable yet.

## Discs

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

## Running a program without a disc

A program can also be handed to the machine directly, with no disc
involved. `--exe` boots the BIOS and then puts a PS-EXE where the
disc's program would have gone, which is how homebrew and the test
suites written for emulator development are run.

```sh
./build-rel/wobble SCPH1001.BIN --exe program.exe
```
