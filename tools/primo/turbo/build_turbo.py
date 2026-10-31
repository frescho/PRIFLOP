#!/usr/bin/env python3
"""Build the Primo turbo's resident module (turbo.asm) and find its relocations.

turbo.asm is assembled at org 0 and moved by the installer to wherever the top
of RAM is. To know which bytes to adjust, it is assembled twice, at 0x0000 and
at 0x0100: every byte that differs is the high byte of an absolute reference to
the module itself (the low bytes cannot differ, the shift being a whole page),
so the word starting one byte earlier gets the load address added.

    python3 tools/primo/turbo/build_turbo.py      # prints size and relocations

Used by tests/primo_turbo_sim.py and by the TURBO.PRG builder.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
# z88dk's assembler: the copy in tools/external/, else the one on PATH
# (z88dk-z80asm, as z88dk's own build names it). Not $Z80ASM: z80asm reads
# that variable itself, as extra arguments, and assembles its own binary.
_BUNDLED = os.path.join(ROOT, "tools", "external", "z88dk", "bin", "z80asm")
Z80ASM = ((_BUNDLED if os.path.exists(_BUNDLED) else None)
          or shutil.which("z88dk-z80asm") or shutil.which("z80asm") or _BUNDLED)
LIMIT = 560     # was 600 (2026-10-05); 578 after step 5a, 559 after 5b (2026-10-07)


def assemble(source="turbo.asm", org=0, extra=None):
    """(bytes, {symbol: address}) for `source` assembled at `org`.
    `extra`: {filename: text} written beside the sources first."""
    with tempfile.TemporaryDirectory() as tmp:
        for name, text in (extra or {}).items():
            open(os.path.join(tmp, name), "w").write(text)
        for name in os.listdir(HERE):
            if name.endswith(".asm"):
                text = open(os.path.join(HERE, name)).read()
                if name == source:
                    text = re.sub(r"(?m)^\s+org\s+0\s*$", f"    org {org}", text, count=1)
                open(os.path.join(tmp, name), "w").write(text)
        out = os.path.join(tmp, "out.bin")
        r = subprocess.run([Z80ASM, "-b", "-m", source, f"-o={out}"], cwd=tmp,
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        code = open(out, "rb").read()
        syms = {}
        for line in open(os.path.join(tmp, "out.map")):
            m = re.match(r"(\w+)\s+= \$([0-9A-F]+) ; addr", line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    return code, syms


def module():
    """(code at org 0, symbols, relocation offsets)."""
    a, syms = assemble(org=0)
    b, _ = assemble(org=0x100)
    if len(a) != len(b):
        raise RuntimeError("the module changed size between origins")
    relocs = []
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            if y != (x + 1) & 0xFF or i == 0:
                raise RuntimeError(f"byte {i:#x} moved by {y - x}, not by one page")
            relocs.append(i - 1)
    if 1 not in relocs:
        raise RuntimeError("inst_core.asm recognises an installed module by the "
                           "relocated word at offset 1, and there is none")
    return a, syms, relocs


def relocate(code, relocs, base):
    out = bytearray(code)
    for r in relocs:
        w = (out[r] | out[r + 1] << 8) + base
        out[r], out[r + 1] = w & 0xFF, (w >> 8) & 0xFF
    return bytes(out)


INSTALL_AT = 0x7000


def installer():
    """The installer's code (install.asm at 0x7000) followed by the module
    image and its relocation list: the payload of TURBO.PRG."""
    code, syms, relocs = module()
    inc = (f"SIZE equ {len(code)}\nOUTPORT_OFS equ {syms['outport']}\n"
           f"OLDTAB_OFS equ {syms['oldtab']}\n")
    if syms["svc"] != 0:
        raise RuntimeError("the installer expects svc at offset 0")
    inst, isyms = assemble("install.asm", extra={"turbo_syms.inc": inc})
    if INSTALL_AT + len(inst) != isyms["image"]:
        raise RuntimeError("the module image must follow the installer directly")
    table = b"".join(bytes([r & 0xFF, r >> 8]) for r in relocs) + b"\xff\xff"
    return inst + code + table


def prg():
    """TURBO.PRG in CDOS's record format: one 0xD9 record at 0x7000, then a
    0xC3 record that starts the installer (the way tools/primo/deploy.py
    builds HELLO)."""
    body = installer()
    return (bytes([0xD9, INSTALL_AT & 0xFF, INSTALL_AT >> 8, len(body) & 0xFF, len(body) >> 8])
            + body + bytes([0xC3, INSTALL_AT & 0xFF, INSTALL_AT >> 8]))


def pturbo(dev=8):
    """PTURBO<dev>.PRG: the soak test (pturbo.asm) at 0x7000 for drive `dev`,
    a 0xD9 record and the 0xC3 that starts it. Under our own DOS (phase B),
    which has no verify, it finds the DOS's signature and reads back instead."""
    code, syms = assemble("pturbo.asm")
    code = bytearray(code)
    code[syms["dev"] - 0x7000] = dev
    return (bytes([0xD9, 0x00, 0x70, len(code) & 0xFF, len(code) >> 8]) + bytes(code)
            + bytes([0xC3, 0x00, 0x70]))


BANNER_CALL = 0x4658 - 0x4500   # boot80.prg's CALL 2B75h that prints its banner


def boot_file(boot80):
    """The boot-slot file of our own DOS, phase A: BOOTU (boot80.prg with the
    port-detecting prologue), then boot.asm, then the module image and its
    relocation list. The loader's banner call goes to boot.asm, which
    installs the turbo only if the drive answers `XT+`. `boot80` is the
    site's boot80.prg; the result is mostly that file and is never committed
    (third-party, as build_unified_boot.py says)."""
    sys.path.insert(0, os.path.dirname(HERE))
    import build_unified_boot
    bootu = build_unified_boot.build(boot80)
    at = 0x4500 + len(bootu)
    code, syms, relocs = module()
    inc = (f"SIZE equ {len(code)}\nOUTPORT_OFS equ {syms['outport']}\n"
           f"OLDTAB_OFS equ {syms['oldtab']}\n")
    inst, isyms = assemble("boot.asm", org=at, extra={"turbo_syms.inc": inc})
    if isyms["boot"] != at or at + len(inst) != isyms["image"]:
        raise RuntimeError("boot.asm must start at its org and end with the image")
    out = bytearray(bootu + inst + code
                    + b"".join(bytes([r & 0xFF, r >> 8]) for r in relocs) + b"\xff\xff")
    if out[BANNER_CALL:BANNER_CALL + 3] != b"\xcd\x75\x2b":
        raise RuntimeError("not the boot80.prg this was written for: no banner call at 0x4658")
    out[BANNER_CALL + 1:BANNER_CALL + 3] = bytes([at & 0xFF, at >> 8])
    return bytes(out), isyms


def pcall():
    """PCALL.PRG: a file with two 0xCD records between its data (pcall.asm).
    Returns (file, symbols)."""
    code, syms = assemble("pcall.asm")
    code = bytearray(code)
    data = bytes((i * 37 + (i >> 8) * 11 + 5) & 0xFF for i in range(0x1000))
    total = sum(data) & 0xFFFF
    code[syms["want"] - 0x7000:syms["want"] - 0x6FFE] = bytes([total & 0xFF, total >> 8])

    def rec(addr, body):
        return bytes([0xD9, addr & 0xFF, addr >> 8, len(body) & 0xFF, len(body) >> 8]) + body

    def jump(t, addr):
        return bytes([t, addr & 0xFF, addr >> 8])
    d = 0x7400                          # DATA in pcall.asm
    return (rec(0x7000, bytes(code)) + jump(0xCD, syms["call1"]) + rec(d, data[:0x800])
            + jump(0xCD, syms["call2"]) + rec(d + 0x800, data[0x800:])
            + jump(0xC3, syms["start"])), syms


def memprg():
    """M.PRG: one line of what the boot left in memory (mem.asm), a 0xD9
    record at 0x7000 and the 0xC3 that starts it. Returns (file, symbols)."""
    code, _, _ = module()
    code, syms = assemble("mem.asm", extra={"turbo_syms.inc": f"SIZE equ {len(code)}\n"})
    return (bytes([0xD9, 0x00, 0x70, len(code) & 0xFF, len(code) >> 8]) + code
            + bytes([0xC3, 0x00, 0x70])), syms


if __name__ == "__main__":
    code, syms, relocs = module()
    print(f"turbo.asm: {len(code)} bytes (limit {LIMIT}), {len(relocs)} relocations")
    print(f"TURBO.PRG: {len(prg())} bytes")
    print(f"PTURBO8.PRG: {len(pturbo(8))} bytes")
    sys.exit(0 if len(code) <= LIMIT else 1)
