#!/usr/bin/env python3
"""Build PFM, the PRIFLOP Manager: pfm.asm assembled at its
origin, written as a CDOS file -- one 0xD9 record with everything up to the
variables (`bss`, not in the file), a 0xC3 autostart at the start.

    python3 tools/primo/pfm/build_pfm.py            # sizes
    python3 tools/primo/pfm/build_pfm.py PFM.PRG    # and the file

Used by tests/primo_pfm_sim.py and debug-tools/deploy_tests.py --primo.
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
SOURCE = os.path.join(HERE, "pfm.asm")


def assemble(patch=()):
    """(the bytes from the origin to `bss`, {symbol: address}). `patch`:
    (old, new) text replacements -- the tests' mutations."""
    text = open(SOURCE).read()
    for old, new in patch:
        if old not in text:
            raise ValueError(f"patch: {old!r} is not in pfm.asm")     # not a caught mutation
        text = text.replace(old, new, 1)
    with tempfile.TemporaryDirectory() as tmp:
        open(os.path.join(tmp, "pfm.asm"), "w").write(text)
        out = os.path.join(tmp, "out.bin")
        r = subprocess.run([Z80ASM, "-b", "-m", "pfm.asm", f"-o={out}"], cwd=tmp,
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        code = open(out, "rb").read()
        syms = {}
        with open(os.path.join(tmp, "out.map")) as f:
            for line in f:
                m = re.match(r"(\w+)\s+= \$([0-9A-F]+) ; addr", line)
                if m:
                    syms[m.group(1)] = int(m.group(2), 16)
    org = syms["start"]
    return code[:syms["bss"] - org], syms


def pfm_file(patch=()):
    """(the file's bytes, symbols)."""
    code, syms = assemble(patch)
    org = syms["start"]
    rec = bytes([0xD9, org & 0xFF, org >> 8, len(code) & 0xFF, len(code) >> 8]) + code
    return rec + bytes([0xC3, org & 0xFF, org >> 8]), syms


def main():
    data, syms = pfm_file()
    org = syms["start"]
    print(f"PFM: {org:#06x}-{syms['bss'] - 1:#06x} in the file ({syms['bss'] - org} bytes), "
          f"variables to {syms['bssend'] - 1:#06x}: {syms['bssend'] - org} bytes in all; "
          f"the file {len(data)} bytes")
    if len(sys.argv) > 1:
        open(sys.argv[1], "wb").write(data)
        print("written", sys.argv[1])


if __name__ == "__main__":
    main()
