#!/usr/bin/env python3
"""Convert a Primo .ptp tape image to the real .PRI/.PRG disk format.

  ./ptp2disk.py [--salvage] [--no-autorun] <file.ptp> [out.pri]

A BASIC program gets CMD SAVE AUTO's record, so CMD LOAD runs it
(--no-autorun: not). A .ptp holding several programs gives out.pri,
out-2.pri, ...

Phase 3: the disk-side target format was already
answered by Phase 2 (see deploy.py) -- what's new
here is parsing .ptp's own tape-block format, documented on
konvertfajlok.html, and re-tagging its blocks onto the disk equivalents.

.ptp structure (one "primo file"; a .ptp may hold several back to back, see
split_ptp -- *corrected 2026-10-15*: this said none of the library needs
that, and 21 files do):

  FF <total length, 2 bytes LE>
  then blocks, each wrapped as:
    55h <size, 2 bytes LE> <size bytes: the inner Primo block>   (normal)
    AAh <size, 2 bytes LE> <size bytes: the inner Primo block>   (closing)

  inner block, name  (first block; identifies the program, not carried
                       into the disk file -- the disk file's name comes
                       from its directory entry, not its own bytes):
    83h|87h <seq=00h> <namelen 1-16> <name bytes> <crc>

  inner block, data:
    F1h BASIC program / F5h screen / F7h BASIC data / F9h machine code
    <block number, BCD> <load address, 2 bytes LE> <byte count, 1 byte,
    documented as "0 means 256" -- true for a full block, but a real
    library file has a short trailing block where 0 genuinely means 0;
    this code derives the payload length from the wrapper's own
    unambiguous 16-bit block size instead of trusting this byte>
    <data bytes> <crc>

  inner block, closing:
    B1h (BASIC/machine-code end, return to BASIC)
    B5h (screen end, return to BASIC)
    B7h (BASIC data end, return to BASIC)
    B9h (auto-starting machine code -- carries a start address)
    <block number, BCD> [B9h only: start address, 2 bytes LE] <crc>

  crc = low byte of the sum of every byte in the inner block from the
  byte right after the type tag through the last data byte (type byte
  and crc byte itself excluded) -- verified while parsing, not just
  computed while writing, so a misreading of the format fails loudly
  instead of silently producing a wrong file.

Salvage mode (--salvage, off by default). The documented checksum verifies
475 of the 488 fetched library files (473 until 2026-10-15, when primo-demo
and primo-kepkirako turned out to be sound: their autostart block is
declared one byte short, see _frames). The other 13 are not bit-rot, as this
file's first version assumed: inside each one, every failing block obeys a
single consistent alternative -- the type byte counted in the sum
(hoslovag, primo40, standing_wave), the block number left out and one added
(loverseny, rt, countthemutants), or a checksum taken before the block's two
data bytes were patched in, i.e. over the header alone (seven files in
ujdonsagok/). --salvage accepts a file only if ONE of those rules explains
every failing block, says which, and still refuses anything else -- a real
corrupted byte in such a file is caught like any other. Closing blocks are
the exception that cannot be checked: a B1/B5/B7 closing block carries
nothing but its number, so a bad checksum there is waived (with a warning);
a B9 block that is too short to hold a start address becomes "return to
BASIC", losing the autostart, with a warning. (Three files were counted here
until 2026-10-15; they were the one-byte-short blocks _frames now reads.) The strict default
is unchanged: fail loudly rather than guess.

Disk-side data block type follows the source: F1h/F9h map straight to
the matching D1h/D9h (deploy.py's DATA_BLOCK_MACHINE_CODE); F5h -> D5h;
F7h has no separately-documented disk equivalent on konvertfajlok.html,
so it maps to D1h too (BASIC-associated data) -- untested, no F7h file
found in the fetched library so far. Closing block: B9h carries its own
start address through to a C3h disk closing block; B1h/B5h/B7h (no
address, "return to BASIC") all become C9h.
"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import deploy  # for le16()

NAME_TYPES = {0x83: "program", 0x87: "data-only"}
DATA_TYPES = {0xF1: "basic-program", 0xF5: "screen", 0xF7: "basic-data", 0xF9: "machine-code"}
CLOSE_TYPES = {0xB1: "end", 0xB5: "screen-end", 0xB7: "basic-data-end", 0xB9: "auto-machine-code"}

DATA_TYPE_TO_DISK = {
    0xF1: 0xD1,  # BASIC program
    0xF5: 0xD5,  # screen content
    0xF7: 0xD1,  # BASIC data -- no distinct disk type documented; best effort
    0xF9: 0xD9,  # machine code
}


class PtpError(ValueError):
    pass


def crc8(data: bytes) -> int:
    return sum(data) & 0xFF


# Alternative checksum conventions found in the fetched library -- see the
# module docstring. Each takes (block type, body without type and crc) and
# returns the byte that convention would have stored, or None if it does not
# apply to that kind of block.
ALTERNATIVE_CRCS = {
    "type byte included in the sum": lambda t, body: (t + sum(body)) & 0xFF,
    "block number left out, plus one": lambda t, body: (sum(body[1:]) + 1) & 0xFF,
    "taken over the header before the data was patched in":
        lambda t, body: sum(body[:4]) & 0xFF if t in DATA_TYPES and len(body) >= 4 else None,
}


def _frames(data: bytes):
    """[(offset, wrapper, inner block)] -- the framing only, no checksums."""
    if data[0] != 0xFF:
        raise PtpError(f"expected 0xFF file marker, got 0x{data[0]:02X}")
    # total_len counts from byte 0 (the 0xFF marker itself), not from
    # after this 3-byte header -- confirmed against konvertfajlok.html's
    # own worked example, whose declared length of 65 only reconciles
    # against its blocks' actual sizes when counted this way; a trailing
    # byte beyond it is normal (the site's own note: some files carry a
    # stray 0x0D/0x00 after the real end, "probably conversion leftover").
    total_len = data[1] | (data[2] << 8)
    if total_len > len(data):
        raise PtpError(f"declared length {total_len} exceeds available {len(data)} bytes")

    pos = 3
    end = total_len
    saw_closing = False
    out = []
    while pos < end:
        wrapper = data[pos]
        if wrapper not in (0x55, 0xAA):
            raise PtpError(f"bad block wrapper 0x{wrapper:02X} at offset {pos}")
        size = data[pos + 1] | (data[pos + 2] << 8)
        inner = data[pos + 3: pos + 3 + size]
        if len(inner) != size:
            raise PtpError(f"truncated block at offset {pos}: wanted {size}, got {len(inner)}")
        nxt = pos + 3 + size
        if (wrapper == 0xAA and size == 4 and inner[0] == 0xB9 and nxt < len(data)
                and data[nxt] in (sum(inner[1:]) & 0xFF, sum(inner) & 0xFF)):
            # An autostart block declared one byte short, its checksum just
            # after it (2024 releases: primo-demo, standing_wave ...): B9,
            # number, address; the byte after checks out by the documented
            # rule or with the type byte included. The tape loader jumps to
            # the address; reading it as 3 bytes plus checksum lost it.
            inner = inner + data[nxt:nxt + 1]
            size += 1
        out.append((pos, wrapper, inner))
        pos += 3 + size
        if wrapper == 0xAA:
            saw_closing = True
            break

    if not saw_closing:
        raise PtpError("reached end of file without a closing (0xAA) block")
    return out


def _mismatch(pos, t, body, crc):
    return PtpError(
        f"CRC mismatch in block type 0x{t:02X} at offset {pos}: "
        f"expected 0x{crc:02X}, computed 0x{crc8(body):02X}"
    )


def _accept_alternative_crcs(failing, warnings):
    """Salvage: decide whether the blocks that failed the documented checksum
    are explained by ONE alternative convention. Raises PtpError if not."""
    def warn(msg):
        if warnings is not None:
            warnings.append(msg)

    data_fail = [b for b in failing if b[1] not in CLOSE_TYPES]
    closing_fail = [b for b in failing if b[1] in CLOSE_TYPES]

    used = None
    if data_fail:
        for name, rule in ALTERNATIVE_CRCS.items():
            if all(rule(t, body) == crc for _, t, body, crc in data_fail):
                used = name
                break
        if used is None:
            unexplained = [b for b in data_fail
                           if not any(rule(b[1], b[2]) == b[3] for rule in ALTERNATIVE_CRCS.values())]
            raise _mismatch(*(unexplained or data_fail)[0])
        warn(f"{len(data_fail)} block(s) verified by a non-standard checksum: {used}")

    rule = ALTERNATIVE_CRCS.get(used)
    for pos, t, body, crc in closing_fail:
        if rule is not None and rule(t, body) == crc:
            continue
        if t == 0xB9 and len(body) >= 3:
            raise _mismatch(pos, t, body, crc)      # a start address that cannot be trusted
        if t == 0xB9:
            warn(f"closing block at offset {pos} is too short to hold a start address "
                 f"and its checksum is wrong; the autostart is lost")
        else:
            warn(f"closing block at offset {pos} has a wrong checksum; it carries only "
                 f"its block number, so it is accepted")


def read_inner_blocks(data: bytes, salvage: bool = False, warnings=None):
    """Yields (block_type, payload_without_crc) for each 55h/AAh-wrapped
    inner block, verifying every CRC. Stops at (and includes) the first
    AAh (closing) wrapper. With salvage=True, blocks that fail the
    documented checksum are accepted if one alternative convention explains
    all of them (see the module docstring); what was relaxed is appended to
    `warnings`."""
    blocks = [(pos, inner[0], inner[1:-1], inner[-1]) for pos, _, inner in _frames(data)]
    failing = [b for b in blocks if crc8(b[2]) != b[3]]
    if failing:
        if not salvage:
            raise _mismatch(*failing[0])
        _accept_alternative_crcs(failing, warnings)
    for _, t, body, _ in blocks:
        yield t, body


def merge_contiguous_blocks(data_blocks):
    """Merge consecutive same-type data blocks whose addresses are
    exactly contiguous into one.

    .ptp's 256-byte-per-block cap is a TAPE hardware limit, not a
    meaningful split on disk -- confirmed real: a big BASIC/screen/
    machine-code unit is stored as many small tape blocks with
    perfectly cumulative addresses (block N's address == block N-1's
    address + its length), exactly the shape one continuous stream
    chopped up for tape would have. The one real CDOS-authored disk
    file available for comparison (detect-tokenized.prg) uses exactly
    one data block for its whole content, not one per 256 bytes.

    This matters beyond tidiness: relink_basic_lines() must see a
    BASIC program's true complete byte stream, not an arbitrary
    256-byte slice of it (a line record can span a slice boundary, and
    even where it doesn't, treating each slice as its own program
    starting fresh at BASIC_PROGRAM_BASE makes every slice after the
    first overlap the same addresses as the first). Found this way:
    primo-library/ptp/demokazi/vitorlas.ptp (34 tape blocks) converted
    without any parse/CRC error but failed to load on the real Primo at
    all -- structurally invalid output from relinking each 256-byte
    slice independently, not a runtime error like betunagy's."""
    if not data_blocks:
        return []
    merged = [list(data_blocks[0])]
    for block_type, load_addr, payload in data_blocks[1:]:
        last_type, last_addr, last_payload = merged[-1]
        if block_type == last_type and load_addr == last_addr + len(last_payload):
            merged[-1][2] = last_payload + payload
        elif block_type == last_type and load_addr == last_addr and payload == last_payload:
            # An exact duplicate of the block just accumulated -- a real
            # source-data quirk (jatek/jetpac.ptp repeats the same
            # 141-byte block twice in a row before the real contiguous
            # chain starts), not a second block that happens to share an
            # address. Drop it: two disk blocks targeting the same
            # address is not a shape any real CDOS-authored file has,
            # and there is nothing to gain by preserving a redundant
            # write. Only catches an immediate repeat of the most
            # recently accumulated block, not duplicates elsewhere.
            pass
        else:
            merged.append([block_type, load_addr, payload])
    return [tuple(m) for m in merged]


def parse_ptp(data: bytes, salvage: bool = False, warnings=None):
    """Returns (name, name_type, [(data_type, load_addr, bytes), ...],
    close_type, start_addr_or_None). The block list has already been
    merged (see merge_contiguous_blocks) -- callers see logical units,
    not the source tape's own 256-byte chunking. `salvage` and `warnings`
    are passed to read_inner_blocks()."""
    blocks = list(read_inner_blocks(data, salvage, warnings))

    head_type, head_body = blocks[0]
    if head_type not in NAME_TYPES:
        raise PtpError(f"expected a name block (0x83/0x87) first, got 0x{head_type:02X}")
    namelen = head_body[1]
    name = head_body[2:2 + namelen].decode("latin-1")

    data_blocks = []
    close_type, start_addr = None, None
    for block_type, body in blocks[1:]:
        if block_type in DATA_TYPES:
            load_addr = body[1] | (body[2] << 8)
            # nb (body[3]) is documented as "0 means 256", but that does
            # NOT hold universally: a short trailing/remainder block can
            # carry a genuine nb=0 meaning zero bytes (found in real
            # library data -- primo-library/ptp/egyeb/lo.ptp's last
            # screen block is 6 raw bytes, i.e. no data at all, not 256).
            # The wrapper's own 16-bit block size is unambiguous and
            # already known from read_inner_blocks, so derive the
            # payload length from what's structurally left in the body
            # instead of re-deriving and cross-checking against nb.
            payload = body[4:]
            data_blocks.append((block_type, load_addr, payload))
        elif block_type in CLOSE_TYPES:
            close_type = block_type
            if block_type == 0xB9:
                if len(body) >= 3:
                    start_addr = body[1] | (body[2] << 8)
                elif salvage:
                    close_type = 0xB1               # no address to jump to: return to BASIC
                else:
                    raise PtpError("auto-start closing block (0xB9) too short to hold a start address")
        else:
            raise PtpError(f"unrecognised inner block type 0x{block_type:02X}")

    if close_type is None:
        raise PtpError("no closing block found")
    if not data_blocks:
        raise PtpError("no data blocks found")

    return name, head_type, merge_contiguous_blocks(data_blocks), close_type, start_addr


# Confirmed real, not assumed: the actual address BASIC's program area
# starts at on this specific Primo + CDOS configuration, computed from
# tools/primo/reference/detect-tokenized.prg -- a file this exact ROM
# tokenized itself (typed and CMD SAVEd by the user). That file's own
# first line record STORES 0x43F7, but that value is line 1's own
# pointer FIELD, which holds the address of line *2* (where the record
# right after line 1's 13 bytes -- 2-byte pointer + 11 bytes of line#/
# tokens/terminator -- begins), not line 1's own starting address.
# Line 1 itself starts 13 bytes earlier, at 0x43EA -- confirmed by
# recomputing from the file directly, not asserted from memory (an
# earlier version of this constant used 0x43F7 itself and was wrong by
# exactly that 13-byte offset, caught by a no-op self-check against this
# same file before it ever reached real hardware).
#
# Why this matters at all: a BASIC program's in-memory line records
# chain together via ABSOLUTE next-line-pointer addresses, and CDOS's
# LOAD does not relink them to wherever the file actually lands this
# time -- confirmed the hard way. betunagy.ptp converted and loaded
# cleanly (no CRC error, structurally valid) but gave a real-hardware
# "SN Error" the moment BASIC advanced past line 1: its own internally-
# consistent pointers are all based at 0x53F7, exactly 0x1000 above this
# machine's real base -- almost certainly because it was originally
# saved on a machine with a different memory layout (e.g. no CDOS
# resident, or different RAM size), not because the file is corrupt.
# Every third-party BASIC program in the library likely has the same
# problem to some degree, so this is a real fix, not a one-off patch.
BASIC_PROGRAM_BASE = 0x43EA


def relink_basic_lines(payload: bytes, base: int = BASIC_PROGRAM_BASE) -> bytes:
    """Rewrite a BASIC program's next-line pointers to `base` -- the
    target machine's actual program-area address -- instead of trusting
    whatever address the file happened to carry from wherever it was
    originally saved. Line content (line numbers, tokens, text) is
    untouched; only the 2-byte pointer at the front of each line record
    is recomputed from the real cumulative lengths.

    Whatever follows the end-of-program sentinel is kept, byte for byte.
    It matters: 20 library programs (13 of them CRC-clean) have bytes there
    -- a stray byte or two in three of them, but in most a routine the BASIC
    part CALLs, e.g. a one-line loader `A=CALL(17408)` with the machine code
    stored in the same tape block right after the program. The first
    version of this function dropped all of it (15,619 bytes became 17 for
    otodolo64_color; 6,718 became 16 for skramble_daymon). The tape loader
    put the whole block into the program area, so the disk file must hold
    it too."""
    lines = []
    pos = 0
    tail = b""
    while pos < len(payload) - 1:
        old_ptr = payload[pos] | (payload[pos + 1] << 8)
        if old_ptr == 0:
            tail = payload[pos + 2:]
            break
        end = pos + 4
        while end < len(payload) and payload[end] != 0x00:
            end += 1
        lines.append(payload[pos + 2:end + 1])  # line#(2) + tokens + terminator
        pos = end + 1

    out = bytearray()
    addr = base
    for line in lines:
        addr += 2 + len(line)  # this line's own pointer field + everything after it
        out += deploy.le16(addr) + line
    out += deploy.le16(0)  # end-of-program sentinel
    return bytes(out) + tail


# BASIC's current line. CMD SAVE AUTO writes a 0xD9 record that sets it to
# 0 before the program; CMD LOAD finds it no longer 0xFFFF and RUNs the
# program ("CMD SAVE AUTO"). A tape has no such mark -- a
# BASIC program never starts by itself from tape -- so `autorun` adds it.
CURLIN = 0x40A2
AUTORUN_RECORD = bytes([0xD9, CURLIN & 0xFF, CURLIN >> 8, 2, 0, 0, 0])


def build_pri(data_blocks, close_type: int, start_addr: int, autorun: bool = False) -> bytes:
    """data_blocks: [(ptp_data_type, load_addr, payload), ...]. `autorun`:
    a BASIC program (one with an 0xF1 block and no machine-code autostart)
    gets CMD SAVE AUTO's record first, so CMD LOAD runs it."""
    out = bytearray()
    if autorun and close_type != 0xB9 and any(t == 0xF1 for t, _, _ in data_blocks):
        out += AUTORUN_RECORD
    for ptp_type, load_addr, payload in data_blocks:
        disk_type = DATA_TYPE_TO_DISK[ptp_type]
        if ptp_type == 0xF1 and load_addr == 0:  # the BASIC program -- relink, see above
            payload = relink_basic_lines(payload)
            # load_addr stays 0: relative, and the base is baked into the pointers now
        # A BASIC block at another offset is a piece of the program area, not
        # a program: kept as it is, at its offset (the program's end bytes
        # after an autostart stub, hanoi/nim2/moo ...). Until 2026-10-15 it
        # was relinked and written at offset 0, over the program's first line.
        out += bytes([disk_type]) + deploy.le16(load_addr) + deploy.le16(len(payload)) + payload

    if close_type == 0xB9:
        out += bytes([deploy.CLOSE_BLOCK_CALL]) + deploy.le16(start_addr)
    else:
        out += bytes([0xC9])
    return bytes(out)


def convert(ptp_bytes: bytes, salvage: bool = False, warnings=None, autorun: bool = False):
    """The first (often the only) program of a .ptp; see convert_all."""
    name, name_type, data_blocks, close_type, start_addr = parse_ptp(split_ptp(ptp_bytes)[0], salvage, warnings)
    pri = build_pri(data_blocks, close_type, start_addr, autorun)
    return name, pri


def split_ptp(data: bytes):
    """The programs of a .ptp, one bytes each. A .ptp may hold several
    tape files back to back -- each FF <its own length, 2 bytes LE, this
    header included> ... -- such as a loader and the part it loads, or a
    series (21 of the library's 488 files; demokazi/demo.ptp has 14)."""
    if not data or data[0] != 0xFF:
        raise PtpError(f"expected 0xFF file marker, got 0x{data[0]:02X}" if data else "empty file")
    parts, pos = [], 0
    while pos < len(data) and data[pos] == 0xFF:      # anything else after a file: a stray
        if pos + 3 > len(data):                       # leftover byte, as _frames allows
            break
        ln = data[pos + 1] | data[pos + 2] << 8
        if ln < 3 or pos + ln > len(data):
            raise PtpError(f"file at {pos}: length {ln} does not fit the {len(data)} bytes")
        parts.append(data[pos:pos + ln])
        pos += ln
    parts[-1] = parts[-1] + data[pos:]                # the stray bytes stay with the last file
    return parts


def is_tape_autoloader(data_blocks) -> bool:
    """A part whose only job is to read the next part from tape and run it
    (demokazi/demo.ptp, emblema.ptp): a BASIC program of one line, `LOAD`
    (token A7h) alone, and a few bytes of machine code (CLEAR, then RUN).
    From a disk there is no tape to read."""
    basic = [d for t, _, d in data_blocks if t == 0xF1]
    code = [d for t, _, d in data_blocks if t == 0xF9]
    if any(t not in (0xF1, 0xF9) for t, _, _ in data_blocks) or not all(len(c) <= 16 for c in code):
        return False
    if basic:
        return (len(basic) == 1 and len(basic[0]) == 8 and basic[0][4:6] == b"\xA7\x00"
                and basic[0][6:8] == b"\x00\x00")
    # machine code alone that jumps into the ROM's tape loader (hammm, panik,
    # kigyok, kincses: LD HL,0A00h; LD (40A2h),HL; JP 050Fh)
    return bool(code) and any(b"\xC3\x0F\x05" in c for c in code)


LOAD_TOKEN, CMD_TOKEN = 0xA7, 0x85


def chain_tape_loads(payload: bytes, names) -> bytes:
    """A BASIC program that reads its next part from tape with a bare LOAD
    statement (hammm, panik: `710 LOAD`; kincses: `LOAD:LOAD:RUN`) gets
    CMD LOAD"<name>" instead, the next of `names` for each, so it loads the
    part from the drive. Strings are left alone; only a statement that is
    LOAD and nothing else is changed. Relink afterwards."""
    names = iter(names)
    out = bytearray()
    pos = 0
    while pos < len(payload) - 1:
        if payload[pos] | payload[pos + 1] << 8 == 0:
            out += payload[pos:]
            return bytes(out)
        end = pos + 4
        while end < len(payload) and payload[end] != 0x00:
            end += 1
        head, body = payload[pos:pos + 4], payload[pos + 4:end]
        stmts, cur, quoted = [], bytearray(), False
        for b in body:
            if b == 0x22:
                quoted = not quoted
            if b == 0x3A and not quoted:
                stmts.append(bytes(cur)); cur = bytearray()
            else:
                cur.append(b)
        stmts.append(bytes(cur))
        new = []
        for st in stmts:
            if st.strip(b" ") == bytes([LOAD_TOKEN]):
                name = next(names, None)
                st = st if name is None else bytes([CMD_TOKEN, LOAD_TOKEN, 0x22]) + name.encode("latin-1") + b"\x22"
            new.append(st)
        out += head + b":".join(new) + b"\x00"
        pos = end + 1
    out += payload[pos:]
    return bytes(out)


def convert_all(ptp_bytes: bytes, salvage: bool = False, warnings=None, autorun: bool = False,
                names=None, basic_fix=None):
    """[(tape name, disk file bytes)] for every program in the .ptp. A part
    that is only a tape autoloader (is_tape_autoloader) is left out when more
    parts follow: the part it would have loaded takes its place. `names`:
    called with the number of parts kept, returns their disk names (without
    .PRG); a part that loads the next from tape with a bare LOAD then gets
    CMD LOAD of the following name (chain_tape_loads). `basic_fix`: called
    with each BASIC program's linked lines, returns them changed (a32fix)."""
    parsed = [parse_ptp(part, salvage, warnings) for part in split_ptp(ptp_bytes)]
    if len(parsed) > 1 and is_tape_autoloader(parsed[0][2]):
        if warnings is not None:
            warnings.append(f"part 1 ({parsed[0][0].strip()!r}) only loads the next from tape: left out")
        parsed = parsed[1:]
    disk = names(len(parsed)) if names and len(parsed) > 1 else None
    out = []
    for i, (name, _, blocks, close_type, start_addr) in enumerate(parsed):
        if basic_fix:
            blocks = [(t, a, basic_fix(d) if t == 0xF1 and a == 0 else d) for t, a, d in blocks]
        if disk:
            chained = []
            for t, a, d in blocks:
                if t == 0xF1 and a == 0:
                    new = chain_tape_loads(d, disk[i + 1:])
                    if new != d and warnings is not None:
                        warnings.append(f"part {name.strip()!r}: LOAD from tape now CMD LOAD from the drive")
                    d = new
                chained.append((t, a, d))
            blocks = chained
        out.append((name, build_pri(blocks, close_type, start_addr, autorun)))
    return out


def main():
    flags = {"--salvage", "--no-autorun"}
    args = [a for a in sys.argv[1:] if a not in flags]
    salvage = "--salvage" in sys.argv[1:]
    autorun = "--no-autorun" not in sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    ptp_path = args[0]
    with open(ptp_path, "rb") as f:
        ptp_bytes = f.read()

    warnings = []
    progs = convert_all(ptp_bytes, salvage, warnings, autorun)
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)

    base = args[1] if len(args) > 1 else ptp_path.rsplit(".", 1)[0] + ".pri"
    stem, dot, ext = base.rpartition(".")
    for i, (name, pri) in enumerate(progs):
        out_path = base if i == 0 else f"{stem}-{i + 1}{dot}{ext}"   # more programs in it: -2, -3 ...
        with open(out_path, "wb") as f:
            f.write(pri)
        print(f"{ptp_path} ({len(ptp_bytes)} bytes, name={name!r}) -> "
              f"{out_path} ({len(pri)} bytes)")


if __name__ == "__main__":
    try:
        main()
    except PtpError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
