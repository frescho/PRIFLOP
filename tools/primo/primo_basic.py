#!/usr/bin/env python3
"""Primo BASIC on the host: tokenize source text, detokenize files, wrap them
in the on-disk container CDOS's `CMD LOAD` expects.

    primo_basic.py tokenize SOURCE.bas OUT.prg     build a loadable BASIC file
    primo_basic.py list FILE.prg                   show a tokenized file as text
    primo_basic.py upload SOURCE.bas NAME [HOST] [PASSWORD]
                                                   tokenize and PUT to a PRIFLOP drive
                                                   (refuses if lint finds anything)

Everything here is derived from evidence, not from another BASIC's published
tables:

  * the keyword table is parsed out of the stock ROM image (a64rom.bin,
    offset 0x1650, 126 keywords, token = 0x80 + position);
  * the line record ([next-line pointer][line number][tokens][0x00]) and the
    block container (D1 data block + C9 closing block) were checked against
    tools/primo/reference/detect-tokenized.prg, a file a real Primo B saved;
  * test_primo_basic.py re-tokenizes that file and every BASIC program in the
    fetched library and compares byte for byte.

The next-line pointers are ABSOLUTE addresses and CDOS does not relink them on
load, so they are computed for this machine's program-area base, 0x43EA.

Conventions implemented (Microsoft 8-bit BASIC, as the ROM does them):
  * the space(s) between the line number and the text are dropped, every other
    space is kept;
  * keywords are matched, upper case only, in table order at each position,
    with no word boundary check; lower-case letters are never tokenized -- so a variable called TOTAL becomes TO + TAL. `lint()`
    exists to catch that in our own programs;
  * text inside double quotes, after REM and inside DATA is copied verbatim;
  * numbers, including line numbers after GOTO/THEN, stay ASCII digits.
"""
import re
import struct
import sys

# Extracted from a64rom.bin at 0x1650. Index i is token 0x80+i. The exponent
# operator (0xD1) is matched on the control character 0x1F, NOT on '^' (0x5E,
# which stays an ordinary character -- verified against real programs).
KEYWORDS = [
    "END", "FOR", "RESET", "SET", "CLS", "CMD", "RANDOM", "NEXT", "DATA",
    "INPUT", "DIM", "READ", "LET", "GOTO", "RUN", "IF", "RESTORE", "GOSUB",
    "RETURN", "REM", "STOP", "ELSE", "TRON", "TROFF", "DEFSTR", "DEFINT",
    "DEFSNG", "DEFDBL", "BEEP", "EDIT", "ERROR", "RESUME", "OUT", "ON",
    "OPEN", "FIELD", "GET", "PUT", "CLOSE", "LOAD", "MERGE", "TEST", "KILL",
    "CREATE", "fn", "SAVE", "SCREEN", "LPRINT", "DEF", "POKE", "PRINT",
    "CONT", "LIST", "LLIST", "DELETE", "AUTO", "CLEAR", "CLOAD", "CSAVE",
    "NEW", "TAB(", "TO", "FN", "USING", "VARPTR", "CALL", "ERL", "ERR",
    "STRING$", "INSTR", "POINT", "TIME$", "PI", "INKEY$", "THEN", "NOT",
    "STEP", "+", "-", "*", "/", "\x1f", "AND", "OR", ">", "=", "<", "SGN", "INT",
    "ABS", "FRE", "INP", "POS", "SQR", "RND", "LOG", "EXP", "COS", "SIN",
    "TAN", "ATN", "PEEK", "CVI", "CVS", "CVD", "EOF", "LOC", "LOF", "MKI$",
    "MKS$", "MKD$", "CINT", "CSNG", "CDBL", "FIX", "LEN", "STR$", "VAL",
    "ASC", "CHR$", "LEFT$", "RIGHT$", "MID$", "'",
]
assert len(KEYWORDS) == 0xFB - 0x80 + 1
TOKEN_REM = 0x80 + KEYWORDS.index("REM")
TOKEN_DATA = 0x80 + KEYWORDS.index("DATA")

BASIC_PROGRAM_BASE = 0x43EA      # ptp2disk.BASIC_PROGRAM_BASE; test asserts they agree
BLOCK_BASIC = 0xD1
CLOSE_RETURN = 0xC9

# the table is searched in order, first match wins ('fn' is a ROM-internal
# lower-case entry that source text never spells)
_SEARCH = [(w, 0x80 + i) for i, w in enumerate(KEYWORDS) if w not in ("fn", "'")]


class BasicError(ValueError):
    pass


def tokenize_text(text: str) -> bytes:
    """Tokenize one statement line, without its line number."""
    text = text.rstrip("\r\n")
    out = bytearray()
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"':                       # string literal, verbatim to the closing quote
            j = text.find('"', i + 1)
            j = n if j < 0 else j + 1
            out += text[i:j].encode("latin-1")
            i = j
            continue
        if ch == "'":                       # comment shorthand: stored as  :REM'  then the text
            out += bytes([0x3A, TOKEN_REM, 0xFB]) + text[i + 1:].encode("latin-1")
            i = n
            continue
        for word, tok in _SEARCH:
            if text.startswith(word, i):
                out.append(tok)
                i += len(word)
                if tok == TOKEN_REM:        # rest of the line is a comment
                    out += text[i:].encode("latin-1")
                    i = n
                elif tok == TOKEN_DATA:     # copy to the next unquoted colon
                    inq = False
                    while i < n and (inq or text[i] != ":"):
                        if text[i] == '"':
                            inq = not inq
                        out.append(ord(text[i]))
                        i += 1
                break
        else:
            out += ch.encode("latin-1")     # anything else, including lower case, is kept as typed
            i += 1
    return bytes(out)


def detokenize_bytes(data: bytes) -> str:
    return "".join(KEYWORDS[b - 0x80] if 0x80 <= b <= 0xFB else chr(b) for b in data)


def to_source(data: bytes) -> str:
    """Token bytes -> text that tokenize_text() maps back to the same bytes.
    Unlike detokenize_bytes() (which is what LIST shows), bytes inside strings,
    after REM and inside DATA are kept as raw characters."""
    out, i, n, inq = [], 0, len(data), False
    while i < n:
        b = data[i]
        if inq:
            out.append(chr(b))
            inq = b != 0x22
        elif data[i:i + 3] == bytes([0x3A, TOKEN_REM, 0xFB]):
            out.append("'" + "".join(map(chr, data[i + 3:])))
            break
        elif b == 0x22:
            out.append('"'); inq = True
        elif b == TOKEN_REM:
            out.append("REM" + "".join(map(chr, data[i + 1:])))
            break
        elif b == TOKEN_DATA:
            j = i + 1; q = False
            while j < n and (q or data[j] != 0x3A):
                q = q != (data[j] == 0x22); j += 1
            out.append("DATA" + "".join(map(chr, data[i + 1:j])))
            i = j; continue
        elif 0x80 <= b <= 0xFB:
            out.append(KEYWORDS[b - 0x80])
        else:
            out.append(chr(b))
        i += 1
    return "".join(out)


_LINE = re.compile(r"^\s*(\d+)\s?(.*)$")


MAX_LINE = 210      # the manual's input-line limit, line number included


def parse_source(src: str):
    """[(line number, text)] from BASIC source. Blank lines and lines whose
    first character is '#' (host-side comments, never sent to the Primo) are
    ignored; text after the line number keeps its spaces except leading ones."""
    lines, last = [], -1
    for raw in src.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        m = _LINE.match(raw)
        if not m:
            raise BasicError(f"line has no line number: {raw!r}")
        num, text = int(m.group(1)), m.group(2).lstrip(" ").rstrip()
        if not 0 <= num <= 65529:
            raise BasicError(f"line number out of range: {num}")
        if num <= last:
            raise BasicError(f"line {num} is not greater than the one before it ({last})")
        if len(str(num)) + 1 + len(text) > MAX_LINE:
            raise BasicError(f"line {num} is {len(str(num)) + 1 + len(text)} characters; the Primo's input line holds {MAX_LINE}")
        lines.append((num, text))
        last = num
    return lines


def build_payload(lines, base: int = BASIC_PROGRAM_BASE) -> bytes:
    """Linked program body: records with absolute pointers, then 0x0000."""
    out = bytearray()
    addr = base
    for num, text in lines:
        body = struct.pack("<H", num) + tokenize_text(text) + b"\x00"
        addr += 2 + len(body)
        out += struct.pack("<H", addr) + body
    out += b"\x00\x00"
    return bytes(out)


def payload_lines(payload: bytes):
    """[(line number, token bytes)] from a payload; pointers are ignored.
    Stops quietly at a truncated or unterminated record."""
    lines, pos = [], 0
    while pos + 4 <= len(payload):
        if payload[pos] | (payload[pos + 1] << 8) == 0:
            break
        end = payload.find(0, pos + 4)
        if end < 0:
            break
        lines.append((payload[pos + 2] | (payload[pos + 3] << 8), payload[pos + 4:end]))
        pos = end + 1
    return lines


def build_prg(payload: bytes) -> bytes:
    """The file CDOS loads: one BASIC data block, then 'return to BASIC'."""
    return bytes([BLOCK_BASIC, 0, 0]) + struct.pack("<H", len(payload)) + payload + bytes([CLOSE_RETURN])


def prg_payload(prg: bytes) -> bytes:
    if prg[0] != BLOCK_BASIC or prg[-1] != CLOSE_RETURN:
        raise BasicError("not a single-block BASIC file")
    count = prg[3] | (prg[4] << 8)
    if 5 + count + 1 != len(prg):
        raise BasicError("block length does not match the file size")
    return prg[5:5 + count]


def list_program(prg_or_payload: bytes) -> str:
    payload = prg_payload(prg_or_payload) if prg_or_payload[:1] == bytes([BLOCK_BASIC]) else prg_or_payload
    return "\n".join(f"{n} {detokenize_bytes(t)}" for n, t in payload_lines(payload))


_WORD = re.compile(r"[A-Za-z][A-Za-z0-9]*[$%!#]?")
_TARGET = re.compile(r"(?:GOTO|GOSUB|THEN|ELSE|RESUME|RESTORE|RUN)\s*(\d+(?:\s*,\s*\d+)*)")


def code_only(text: str) -> str:
    """The executable part of a line: string literals blanked out, nothing
    after REM, DATA or a comment quote."""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            j = text.find('"', i + 1)
            j = n if j < 0 else j + 1
            out.append('""')
            i = j
        elif ch == "'" or text.startswith("REM", i):
            break
        elif text.startswith("DATA", i):
            out.append("DATA")
            break
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _pieces(toks: bytes) -> str:
    """Token bytes -> 'TO+TAL' (keywords by name, the rest as typed)."""
    parts, run = [], ""
    for b in toks:
        if b >= 0x80:
            if run:
                parts.append(run)
                run = ""
            parts.append(KEYWORDS[b - 0x80])
        else:
            run += chr(b)
    if run:
        parts.append(run)
    return "+".join(parts)


def lint(lines):
    """Problems a real Primo would only show at run time. Returns [str].

    * a word that contains a keyword plus other letters is split by the
      tokenizer (TOTAL -> TO + TAL): the ROM has no word-boundary check;
    * two variables whose first two characters agree are the same variable
      on a TRS-80-lineage BASIC (assumed, not verified on this machine);
    * a GOTO/GOSUB/THEN/RESUME target that is not a line of the program."""
    problems, names, numbers = [], {}, {n for n, _ in lines}
    for num, text in lines:
        code = code_only(text)
        for m in _WORD.finditer(code):
            word = m.group(0)
            if word[-1] in "$%!#" and all(b >= 0x80 for b in tokenize_text(word[:-1])):
                word = word[:-1]   # a keyword followed by '#' etc. (PRINT#, INPUT#) is not a name
            toks = tokenize_text(word)
            if any(b >= 0x80 for b in toks):
                if not all(b >= 0x80 for b in toks):
                    problems.append(f"{num}: '{word}' will be split by the tokenizer into {_pieces(toks)}")
            else:
                names.setdefault((word[:2].upper(), word[-1] if word[-1] in "$%!#" else ""), set()).add(word)
        for m in _TARGET.finditer(code):
            for t in re.split(r"\s*,\s*", m.group(1)):
                if int(t) != 0 and int(t) not in numbers:
                    problems.append(f"{num}: jumps to line {t}, which does not exist")
    for (head, kind), words in sorted(names.items()):
        if len(words) > 1:
            problems.append(f"variables {sorted(words)} share their first two characters '{head}{kind}'")
    return problems


def build_file(path: str):
    """(prg bytes, line count, lint problems) for a source file."""
    lines = parse_source(open(path, encoding="latin-1").read())
    return build_prg(build_payload(lines)), len(lines), lint(lines)


def main(argv):
    if len(argv) == 3 and argv[0] == "tokenize":
        prg, count, problems = build_file(argv[1])
        for p in problems:
            print("warning:", p, file=sys.stderr)
        open(argv[2], "wb").write(prg)
        print(f"{argv[1]}: {count} lines -> {argv[2]} ({len(prg)} bytes)")
        return 0
    if len(argv) == 2 and argv[0] == "list":
        print(list_program(open(argv[1], "rb").read()))
        return 0
    if len(argv) in (3, 4, 5) and argv[0] == "upload":
        import urllib.error
        import deploy
        prg, count, problems = build_file(argv[1])
        if problems:
            for p in problems:
                print("error:", p, file=sys.stderr)
            return 1
        host = argv[3] if len(argv) > 3 else "priflop.local"
        password = argv[4] if len(argv) > 4 else "admin"
        try:
            result = deploy.upload(prg, argv[2], host, password)
        except urllib.error.HTTPError as e:
            print(f"upload failed: HTTP {e.code} {e.reason}", file=sys.stderr)
            return 1
        print(f"{argv[1]} ({count} lines, {len(prg)} bytes) -> {deploy.cbm_prg_name(argv[2])} on {host}: {result}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
