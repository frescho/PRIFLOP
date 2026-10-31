#!/usr/bin/env python3
"""Assemble a Primo Z80 program and deploy it to PRIFLOP in one step.

  ./deploy.py <file.asm> <CBM-name> [host] [admin-password]

Reads the load address from the source's own `org` directive (single
source of truth -- no separate address argument to drift out of sync),
assembles with z88dk's z80asm, wraps it in the real .PRI/.PRG on-disk
format CDOS's own LOAD expects, and PUTs it straight to PRIFLOP's root
under the given name.

This targets ordinary CMD LOAD off a running CDOS session, not the boot
slot -- an ordinary CBM name is exactly right here. (The boot slot is
different: it must be the raw CHR$(133) byte, see
build_unified_boot.py.)

**Corrected 2026-09-07** -- the first version of this script emitted a
bare [2-byte load address][code] stream, by (wrong) analogy with
boot40.prg/boot80.prg. Real-hardware test: `CMD LOAD"HELLO"` failed with
"SN Error in 32376" -- BASIC's line-linker had wandered off into
garbage far past the actual program, exactly what happens when it tries
to walk untagged machine code as if it were BASIC's own next-line-
pointer chain. boot40.prg/boot80.prg are loaded by the boot EPROM's own
separate, minimal bootstrap (that's genuinely bare-header), which is a
different code path from CDOS's real CMD LOAD -- confirmed by checking
boot80.prg's own first byte, which isn't D1h/D5h/D9h at all. The real
on-disk format, confirmed from konvertfajlok.html and cross-checked
against its own PRI2PTP/PTP2PRI tool description on
konvertpritoptp.html, is block-tagged:

  data block:    [type: D1h BASIC / D5h screen / D9h machine code]
                 [load address, 2 bytes LE -- absolute for D9h]
                 [byte count, 2 bytes LE]
                 [that many data bytes]
  closing block: [C3h -- call the next 2 bytes (start address, LE)
                  | C9h -- return control to BASIC, no address follows]

Per the CDOS manual (primofuzetek-cdos.txt): a machine-code program
loaded by a direct `CMD LOAD"name"` at the READY prompt runs
automatically via that C3h closing block's start address -- no run
flag, no D64 needed. This is Phase 2's dev loop, one script.
"""
import base64
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import urllib.error
from pathlib import Path

# z88dk's assembler: the copy in tools/external/ (see the README there), else
# the one on PATH. Not $Z80ASM: z80asm reads that variable itself, as extra
# arguments, and then assembles its own binary.
_BUNDLED = Path(__file__).resolve().parents[2] / "tools" / "external" / "z88dk" / "bin" / "z80asm"
Z80ASM = ((str(_BUNDLED) if _BUNDLED.exists() else None)
          or shutil.which("z88dk-z80asm") or shutil.which("z80asm") or str(_BUNDLED))


def read_org(asm_path: str) -> int:
    with open(asm_path) as f:
        text = f.read()
    m = re.search(r"(?im)^\s*org\s+([0-9a-f]+)h\s*$", text)
    if not m:
        raise ValueError(f"no `org NNNNh` directive found in {asm_path}")
    return int(m.group(1), 16)


def assemble(asm_path: str) -> bytes:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name
    result = subprocess.run(
        [Z80ASM, "-b", asm_path, f"-o={bin_path}"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"z80asm failed:\n{result.stdout}\n{result.stderr}")
    with open(bin_path, "rb") as f:
        return f.read()


DATA_BLOCK_MACHINE_CODE = 0xD9
CLOSE_BLOCK_CALL = 0xC3


def le16(n: int) -> bytes:
    return bytes([n & 0xFF, (n >> 8) & 0xFF])


def build_prg(body: bytes, load_addr: int, start_addr: int = None) -> bytes:
    """Wrap assembled machine code in the real .PRI/.PRG on-disk format.

    One data block holding the whole program, one closing block that
    calls back into it. start_addr defaults to load_addr -- every test
    program so far is a single entry point starting exactly where it
    was loaded, matching hello.asm; pass start_addr explicitly if that
    ever stops being true.
    """
    if start_addr is None:
        start_addr = load_addr
    data_block = (
        bytes([DATA_BLOCK_MACHINE_CODE]) + le16(load_addr) + le16(len(body)) + body
    )
    close_block = bytes([CLOSE_BLOCK_CALL]) + le16(start_addr)
    return data_block + close_block


def cbm_prg_name(name: str) -> str:
    """Ensure a .PRG extension, matching IECTEST.PRG/IMGTEST.PRG's own
    naming on PRIFLOP. Not load-bearing for CDOS -- a bare name already
    defaults to PRG type (lfsops.c's check_extension(): "Unknown
    extension: PRG"), and CMD LOAD"NAME" works identically either way
    since the .PRG gets stripped from the CBM-visible name -- this is
    host-side file-manager hygiene, requested after PRIFLOP's own root
    ended up with a mix of bare and .PRG names from ad hoc uploads."""
    return name if name.upper().endswith(".PRG") else name + ".PRG"


def upload(prg: bytes, name: str, host: str, password: str):
    name = cbm_prg_name(name)
    auth = base64.b64encode(f"admin:{password}".encode()).decode()
    req = urllib.request.Request(
        f"http://{host}/files/{name}",
        data=prg,
        method="PUT",
        headers={"Authorization": f"Basic {auth}"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.read().decode(errors="replace")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    asm_path, name = sys.argv[1], sys.argv[2]
    host = sys.argv[3] if len(sys.argv) > 3 else "priflop.local"
    password = sys.argv[4] if len(sys.argv) > 4 else "admin"

    load_addr = read_org(asm_path)
    body = assemble(asm_path)
    prg = build_prg(body, load_addr)

    try:
        result = upload(prg, name, host, password)
    except urllib.error.HTTPError as e:
        print(f"upload failed: HTTP {e.code} {e.reason}", file=sys.stderr)
        sys.exit(1)

    print(f"{asm_path} ({len(body)} bytes, org 0x{load_addr:04X}) "
          f"-> {name} ({len(prg)} bytes) on {host}: {result}")


if __name__ == "__main__":
    main()
