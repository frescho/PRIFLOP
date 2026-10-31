"""Make a BASIC program written for the 32 KB Primo (A-32) run on a 64 KB one.

Such programs read a screen address from the system variables 4039h
(screen RAM start) or 40B1h (end of the BASIC area), as
256*PEEK(hi)+PEEK(lo), and POKE there or keep it in an integer variable.
On a 32 KB Primo the address is below 8000h; on a 64 KB one it is above,
and the ROM stops with OV (an integer variable, or a POKE address, must be
-32768..32767) or FC (POKE of a byte above 255). Measured in MAME's
primob64 on the original tapes of banyasz_berci and lokomotiv, both marked
"(A32)" in the library's notes.

The fix, statement text only:
  256*PEEK(hi) and PEEK(hi)*256  ->  (PEEK(hi)-256)*256   (the signed address)
  ,INT(X/256)                    ->  ,(INT(X/256)AND255)   (its high byte again)
On a 32 KB Primo the patched program would compute wrong addresses: the
patch is for the 64 KB machine only, and only for the programs named in
build_library.A32_PROGRAMS.
"""
import re
import struct

import primo_basic

POINTERS = ("16442", "16562")          # high bytes of 4039h and 40B1h


def fix_text(text: str) -> str:
    for hi in POINTERS:
        text = text.replace(f"256*PEEK({hi})", f"(PEEK({hi})-256)*256")
        text = text.replace(f"PEEK({hi})*256", f"(PEEK({hi})-256)*256")
    return re.sub(r",INT\((\w+)/256\)", r",(INT(\1/256)AND255)", text)


def fix_payload(payload: bytes, base: int = primo_basic.BASIC_PROGRAM_BASE) -> bytes:
    """The linked program with fix_text applied; lines it does not change keep
    their exact token bytes."""
    out, addr = bytearray(), base
    for num, tok in primo_basic.payload_lines(payload):
        text = primo_basic.detokenize_bytes(tok)
        new = fix_text(text)
        if new != text:
            tok = primo_basic.tokenize_text(new)
        body = struct.pack("<H", num) + tok + b"\x00"
        addr += 2 + len(body)
        out += struct.pack("<H", addr) + body
    return bytes(out + b"\x00\x00")
