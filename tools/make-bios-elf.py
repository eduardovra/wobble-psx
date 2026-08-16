#!/usr/bin/env python3
"""Wraps a BIOS image in an ELF so a debugger will take it seriously.

gdb can debug the emulated machine with no file at all, but a raw
image leaves it with no architecture until the stub says otherwise,
no name for any address, and nothing to hand a frontend that insists
on a program to open. Neither the bytes nor the addresses change on
the way through: this is the same ROM, wearing a header that says
where it is loaded and what it is.

    tools/make-bios-elf.py SCPH1001.BIN bios.elf
"""

import struct
import sys

BIOS_BASE = 0xBFC00000

EM_MIPS = 8
ET_EXEC = 2
PT_LOAD = 1
PF_R_X = 5
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB = 1, 2, 3
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4
STB_GLOBAL, STT_FUNC = 1, 2

EHDR_SIZE, PHDR_SIZE, SHDR_SIZE, SYM_SIZE = 52, 32, 40, 16

Symbol = tuple[str, int, int]


def symbols_for(image: bytes) -> list[Symbol]:
    """One symbol per instruction, named for its address.

    This is what makes an ordinary "step over" work on ROM code.
    Stepping by source line asks gdb where the current line ends, and
    a BIOS has no line numbers to answer with — so it refuses, and a
    frontend reports the refusal as a failed step. Stepping does work
    when it can find the bounds of the function it is in, and a
    function four bytes long is left by executing exactly one
    instruction. So every instruction is its own function, and the
    ordinary step buttons step one instruction at a time.

    The cost is that a call is not stepped over but into, since
    reaching the callee also counts as leaving. For code with no
    source that is the honest behaviour anyway.
    """
    return [
        (f"L{BIOS_BASE + offset:08X}", BIOS_BASE + offset, 4)
        for offset in range(0, len(image), 4)
    ]


def strtab(names: list[str]) -> tuple[bytes, dict[str, int]]:
    """A string table, and the offset each name landed at.

    Built as a list and joined once. There is a symbol per
    instruction, and appending to a bytes object in that loop copies
    the whole table each time round.
    """
    pieces = [b"\0"]
    offsets: dict[str, int] = {}
    position = 1
    for name in names:
        offsets[name] = position
        encoded = name.encode() + b"\0"
        pieces.append(encoded)
        position += len(encoded)
    return b"".join(pieces), offsets


def symbol_table(symbols: list[Symbol],
                 name_offsets: dict[str, int]) -> bytes:
    # A null symbol first: index 0 has to mean "no symbol".
    entries = [b"\0" * SYM_SIZE]
    for name, address, size in symbols:
        entries.append(struct.pack("<IIIBBH",
                                   name_offsets[name],
                                   address,
                                   size,
                                   (STB_GLOBAL << 4) | STT_FUNC,
                                   0,
                                   1))  # defined in .text
    return b"".join(entries)


def build(image: bytes) -> bytes:
    section_names = ["", ".text", ".symtab", ".strtab", ".shstrtab"]
    shstrtab, shstr_off = strtab(section_names[1:])
    shstr_off[""] = 0

    symbols = symbols_for(image)
    symbol_names = [name for name, _, _ in symbols]
    symtab_strings, sym_off = strtab(symbol_names)
    symtab = symbol_table(symbols=symbols, name_offsets=sym_off)

    text_off = EHDR_SIZE + PHDR_SIZE
    symtab_off = text_off + len(image)
    strtab_off = symtab_off + len(symtab)
    shstrtab_off = strtab_off + len(symtab_strings)
    shoff = shstrtab_off + len(shstrtab)

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    ehdr = ident + struct.pack("<HHIIIIIHHHHHH",
                               ET_EXEC,
                               EM_MIPS,
                               1,
                               BIOS_BASE,
                               EHDR_SIZE,
                               shoff,
                               0,  # plain MIPS I, which an R3000A is
                               EHDR_SIZE,
                               PHDR_SIZE,
                               1,
                               SHDR_SIZE,
                               len(section_names),
                               4)  # .shstrtab

    phdr = struct.pack("<IIIIIIII",
                       PT_LOAD,
                       text_off,
                       BIOS_BASE,
                       BIOS_BASE,
                       len(image),
                       len(image),
                       PF_R_X,
                       0x1000)

    def shdr(name: str,
             kind: int,
             flags: int = 0,
             addr: int = 0,
             offset: int = 0,
             size: int = 0,
             link: int = 0,
             info: int = 0,
             align: int = 1,
             entsize: int = 0) -> bytes:
        return struct.pack("<IIIIIIIIII",
                           shstr_off[name],
                           kind,
                           flags,
                           addr,
                           offset,
                           size,
                           link,
                           info,
                           align,
                           entsize)

    sections = b"".join([
        shdr(name="", kind=0),
        shdr(name=".text",
             kind=SHT_PROGBITS,
             flags=SHF_ALLOC | SHF_EXECINSTR,
             addr=BIOS_BASE,
             offset=text_off,
             size=len(image),
             align=4),
        # info = index of the first global symbol, i.e. how many local
        # ones precede it; only the null symbol is local here.
        shdr(name=".symtab",
             kind=SHT_SYMTAB,
             offset=symtab_off,
             size=len(symtab),
             link=3,
             info=1,
             align=4,
             entsize=SYM_SIZE),
        shdr(name=".strtab",
             kind=SHT_STRTAB,
             offset=strtab_off,
             size=len(symtab_strings)),
        shdr(name=".shstrtab",
             kind=SHT_STRTAB,
             offset=shstrtab_off,
             size=len(shstrtab)),
    ])

    return ehdr + phdr + image + symtab + symtab_strings + shstrtab + sections


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as rom:
        image = rom.read()
    with open(sys.argv[2], "wb") as elf:
        elf.write(build(image))
    print(f"wrote {sys.argv[2]}: {len(image)} bytes at {BIOS_BASE:#010x}")


main()
