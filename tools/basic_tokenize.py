#!/usr/bin/env python3
"""Tokenize C64 BASIC 2.0 source text into a loadable .PRG.

Why this exists: the sd2iec test program has to be typed into a real C64
otherwise, and a 60-line program is a lot of typing to get wrong. This
emits a real tokenized PRG that can be uploaded through the web file
manager and LOAD"...",8 / RUN directly.

Deliberately minimal: longest-match keyword tokenizing, no abbreviations
(no "?" for PRINT), quotes and REM text passed through verbatim.
"""
import sys, re

# BASIC 2.0 keywords, token 0x80 upward, in canonical order.
KEYWORDS = [
    "END","FOR","NEXT","DATA","INPUT#","INPUT","DIM","READ","LET","GOTO",
    "RUN","IF","RESTORE","GOSUB","RETURN","REM","STOP","ON","WAIT","LOAD",
    "SAVE","VERIFY","DEF","POKE","PRINT#","PRINT","CONT","LIST","CLR","CMD",
    "SYS","OPEN","CLOSE","GET","NEW","TAB(","TO","FN","SPC(","THEN",
    "NOT","STEP","+","-","*","/","^","AND","OR",">",
    "=","<","SGN","INT","ABS","USR","FRE","POS","SQR","RND",
    "LOG","EXP","COS","SIN","TAN","ATN","PEEK","LEN","STR$","VAL",
    "ASC","CHR$","LEFT$","RIGHT$","MID$","GO",
]
TOKENS = {kw: 0x80 + i for i, kw in enumerate(KEYWORDS)}
# Longest first so PRINT# wins over PRINT, INPUT# over INPUT.
ORDERED = sorted(KEYWORDS, key=len, reverse=True)

def tokenize_line(text):
    out = bytearray()
    i = 0
    in_quotes = False
    while i < len(text):
        ch = text[i]
        if in_quotes:
            out.append(ord(ch))
            if ch == '"':
                in_quotes = False
            i += 1
            continue
        if ch == '"':
            in_quotes = True
            out.append(ord(ch))
            i += 1
            continue
        matched = None
        for kw in ORDERED:
            if text.startswith(kw, i):
                matched = kw
                break
        if matched:
            out.append(TOKENS[matched])
            i += len(matched)
            # REM swallows the rest of the line verbatim
            if matched == "REM":
                out.extend(text[i:].encode("ascii", "replace"))
                i = len(text)
            continue
        out.append(ord(ch))
        i += 1
    return bytes(out)

def build(src, load_addr=0x0801):
    prog = bytearray()
    addr = load_addr
    for raw in src.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r'^(\d+)\s*(.*)$', line)
        if not m:
            raise SystemExit("line without a number: %r" % line)
        num, body = int(m.group(1)), m.group(2)
        toks = tokenize_line(body)
        # link(2) + lineno(2) + tokens + NUL
        length = 2 + 2 + len(toks) + 1
        addr += length
        prog += addr.to_bytes(2, "little")
        prog += num.to_bytes(2, "little")
        prog += toks
        prog += b"\x00"
    prog += b"\x00\x00"                      # end of program
    return load_addr.to_bytes(2, "little") + bytes(prog)

if __name__ == "__main__":
    src = open(sys.argv[1]).read()
    out = build(src)
    open(sys.argv[2], "wb").write(out)
    print("wrote %s (%d bytes)" % (sys.argv[2], len(out)))
