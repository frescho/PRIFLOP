#!/usr/bin/env python3
"""Build our own DOS for the Primo (phase B): dos.asm, assembled
at org 0, and the list of bytes the loader relocates.

The load limit must sit at offset 0x0B8F, where the drive's turbo reads it;
dos.asm pads up to it and body() checks it.

The body is assembled twice, at 0x0000 and at 0x0100: every byte that differs
is the high byte of an absolute reference into the body, so the word starting
one byte earlier gets the load address added (as build_turbo.module() does).

    python3 tools/primo/dos/build_dos.py      # size, relocations, the parts

Used by tests/primo_dos_sim.py.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TURBO = os.path.join(os.path.dirname(HERE), "turbo")
# z88dk's assembler: the copy in tools/external/, else the one on PATH
# (z88dk-z80asm, as z88dk's own build names it). Not $Z80ASM: z80asm reads
# that variable itself, as extra arguments, and assembles its own binary.
_BUNDLED = os.path.join(ROOT, "tools", "external", "z88dk", "bin", "z80asm")
Z80ASM = ((_BUNDLED if os.path.exists(_BUNDLED) else None)
          or shutil.which("z88dk-z80asm") or shutil.which("z80asm") or _BUNDLED)


def assemble(source="dos.asm", org=0, patch=(), extra=None):
    """(bytes, {symbol: address}) for `source` assembled at `org`. The
    turbo's sources (xfer.asm) are beside it, as they are shared. `patch`:
    (old, new) text replacements in `source` -- the tests' mutations.
    `extra`: {name: text}, more files beside it (an include of equates)."""
    with tempfile.TemporaryDirectory() as tmp:
        for name, text in (extra or {}).items():
            open(os.path.join(tmp, name), "w").write(text)
        for d in (TURBO, HERE):
            for name in os.listdir(d):
                if name.endswith(".asm"):
                    text = open(os.path.join(d, name)).read()
                    if name == source:
                        for old, new in patch:
                            if old not in text:
                                raise ValueError(f"patch: {old!r} is not in {source}")  # not a caught mutation
                            text = text.replace(old, new, 1)
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


ANCHOR = 0x0B8F     # the load limit, where the drive reads it (point 6)


def body(patch=()):
    """(code at org 0, symbols, relocation offsets)."""
    a, syms = assemble(org=0, patch=patch)
    b, _ = assemble(org=0x100, patch=patch)
    if len(a) != len(b):
        raise RuntimeError("the body changed size between origins")
    if syms["limit"] != ANCHOR:
        raise RuntimeError(f"the load limit is at {syms['limit']:#06x}, not {ANCHOR:#06x}: "
                           "the drive's turbo would read it from the wrong place")
    if syms["dossig"] != ANCHOR - 8:
        raise RuntimeError(f"the signature is at {syms['dossig']:#06x}: the loader and M "
                           f"(tools/primo/turbo/mem.asm, SIGOFS) look for it at {ANCHOR - 8:#06x}")
    relocs = []
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            if y != (x + 1) & 0xFF or i == 0:
                raise RuntimeError(f"byte {i:#x} moved by {y - x}, not by one page")
            relocs.append(i - 1)
    return a, syms, relocs


def relocate(code, relocs, base):
    out = bytearray(code)
    for r in relocs:
        w = (out[r] | out[r + 1] << 8) + base
        out[r], out[r + 1] = w & 0xFF, (w >> 8) & 0xFF
    return bytes(out)


BOOT = 0x4500       # where the boot ROM stores the boot file, and jumps


def gaps(relocs):
    """The relocations as the loader reads them: one byte each, the distance
    from the one before (from the image's start for the first); 0FFh steps
    254 on without one; 0 ends the list."""
    out, at = bytearray(), 0
    for r in relocs:
        g = r - at
        if g < 1:
            raise RuntimeError(f"relocations at {at:#x} and {r:#x}: a gap of {g}")
        while g > 254:
            out.append(0xFF)
            g -= 254
        out.append(g)
        at = r
    return bytes(out + b"\x00")


def boot_file(dos=None, patch=()):
    """(the boot-slot file of our own DOS, the loader's symbols): loader.asm
    at 0x4500, the body's relocations (gaps()), the body without its
    padding, which the loader puts back. No CDOS code is in it.
    Phase A's module is recognised by its first bytes, so the turbo's build
    gives them (build_turbo.module())."""
    code, syms, relocs = dos or body()
    sys.path.insert(0, TURBO)
    import build_turbo
    acode = build_turbo.module()[0]

    pad = syms["p_anchor"] - syms["p_pad"]
    if not 0 < pad < 256 or any(code[syms["p_pad"]:syms["p_anchor"]]):
        raise RuntimeError(f"the padding ({pad} bytes) must be 1-255 zeros: the loader makes it")
    image = code[:syms["p_pad"]] + code[syms["p_anchor"]:]
    rels = gaps(relocs)

    def inc(at):
        names = {"S18": "s18", "OUTPORT": "outport", "ERRBUF": "errbuf", "SIG": "dossig", "OLDCMD": "oldcmd",
                 "OLDHOOK": "oldhook", "SVC": "svc", "EHOOK": "ehook", "EFN": "efn", "PEND": "pend",
                 "COUT": "cout"}
        lines = [f"B_{k} equ {syms[v]}" for k, v in names.items()]
        lines += [f"BODYSZ equ {len(code)}", f"RELS equ {at}", f"IMG equ {at + len(rels)}",
                  f"PADAT equ {syms['p_pad']}", f"PADSZ equ {pad}", f"TAILSZ equ {len(code) - syms['p_anchor']}",
                  f"A_SIZE equ {len(acode)}", f"A_OP equ {acode[0]}", f"A_OPW equ {acode[1] | acode[2] << 8}"]
        return {"dos_syms.inc": "\n".join(lines) + "\n"}
    first, _ = assemble("loader.asm", org=BOOT, patch=patch, extra=inc(0))
    at = BOOT + len(first)
    loader, lsyms = assemble("loader.asm", org=BOOT, patch=patch, extra=inc(at))
    if len(loader) != len(first) or lsyms["go"] != BOOT:
        raise RuntimeError("the loader must start at 0x4500 and keep its size")
    return loader + rels + image, lsyms


def parts(syms, size):
    """{part: bytes}, from the `p_<part>` labels that start each part."""
    marks = sorted((a, n[2:]) for n, a in syms.items() if n.startswith("p_"))
    marks.append((size, None))
    return {n: marks[i + 1][0] - a for i, (a, n) in enumerate(marks[:-1])}


if __name__ == "__main__":
    code, syms, relocs = body()
    ps = parts(syms, len(code))
    print(f"body {len(code)} bytes, {len(relocs)} relocations; "
          f"{len(code) - ps['pad']} without the padding up to the anchor")
    for n, s in ps.items():
        print(f"  {n:10} {s:5}")
    f, lsyms = boot_file((code, syms, relocs))
    print(f"boot file {len(f)} bytes: the loader {len(f) - len(gaps(relocs)) - len(code) + ps['pad']}, "
          f"{len(gaps(relocs))} bytes of relocations, the body without its padding")
    if len(sys.argv) > 1:
        open(sys.argv[1], "wb").write(f)
        print("written", sys.argv[1])
    sys.exit(0)
