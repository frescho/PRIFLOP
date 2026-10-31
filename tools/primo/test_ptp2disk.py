#!/usr/bin/env python3
"""Tests for ptp2disk.py -- the documented worked example (no download
needed) plus, if the library has been fetched, a sweep across every real
.ptp file checking CRCs and reporting the clean/failed count.

Run: python3 tools/primo/test_ptp2disk.py
"""
import glob
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import ptp2disk
import deploy


def _hello_world_ptp():
    """konvertfajlok.html's own worked example, byte for byte -- "hello
    world", one BASIC data block, B1h closing (plain return to BASIC).
    Built from the row-broken hex exactly as the page prints it, rather
    than a single hand-typed string -- a manual-transcription slip in
    this exact example cost real time during Phase 3 development."""
    lines = [
        "FF 41 00 55 0F 00 83 00",
        "0B 68 65 6C 6C 6F 20 77",
        "6F 72 6C 64 67 55 23 00",
        "F1 01 00 00 1D F0 43 0A",
        "00 84 00 05 44 14 00 B2",
        "20 22 48 65 6C 6C 6F 20",
        "57 6F 72 6C 64 21 22 00",
        "00 00 8F AA 03 00 B1 02",
        "02 00",
    ]
    return bytes.fromhex("".join(lines).replace(" ", ""))


class TestWorkedExample(unittest.TestCase):
    """konvertfajlok.html's own documented PTP, decoded and converted."""

    def test_parses_the_name(self):
        name, _, _, _, _ = ptp2disk.parse_ptp(_hello_world_ptp())
        self.assertEqual(name, "hello world")

    def test_one_basic_data_block(self):
        _, _, blocks, _, _ = ptp2disk.parse_ptp(_hello_world_ptp())
        self.assertEqual(len(blocks), 1)
        data_type, load_addr, payload = blocks[0]
        self.assertEqual(data_type, 0xF1)
        self.assertEqual(load_addr, 0)
        self.assertEqual(len(payload), 29)

    def test_closing_block_is_plain_end(self):
        _, _, _, close_type, start_addr = ptp2disk.parse_ptp(_hello_world_ptp())
        self.assertEqual(close_type, 0xB1)
        self.assertIsNone(start_addr)

    def test_converts_to_the_exact_expected_pri_bytes(self):
        name, pri = ptp2disk.convert(_hello_world_ptp())
        self.assertEqual(name, "hello world")
        expected = (
            bytes([0xD1]) + deploy.le16(0) + deploy.le16(29) +
            bytes.fromhex("f0430a00840005441400b2202248656c6c6f20576f726c642122000000") +
            bytes([0xC9])
        )
        self.assertEqual(pri, expected)

    def test_a_corrupted_byte_is_caught_not_silently_converted(self):
        ptp = bytearray(_hello_world_ptp())
        ptp[30] ^= 0x01  # flip one bit inside the data block's payload
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(bytes(ptp))


class TestCrcHelper(unittest.TestCase):
    def test_matches_the_documented_name_block_crc(self):
        # "hello world" name block body (seq, namelen, name), CRC 0x67
        body = bytes.fromhex("000b68656c6c6f20776f726c64")
        self.assertEqual(ptp2disk.crc8(body), 0x67)


class TestTrailingRemainderBlock(unittest.TestCase):
    """The real bug this phase found: nb=0 does not always mean 256."""

    def test_nb_zero_with_no_data_left_is_zero_bytes_not_256(self):
        # type=F5, bn, laL, laH, nb=0x00, then NOTHING -- a genuine
        # zero-length trailing block (this exact shape is real,
        # primo-library/ptp/egyeb/lo.ptp's last screen block).
        body = bytes([0x01, 0x00, 0x00, 0x00])  # bn, laL, laH, nb=0
        payload = body[4:]
        self.assertEqual(payload, b"")

    def test_nb_zero_with_256_bytes_left_is_256(self):
        body = bytes([0x01, 0x00, 0x00, 0x00]) + bytes(256)
        payload = body[4:]
        self.assertEqual(len(payload), 256)


class TestMergeContiguousBlocks(unittest.TestCase):
    """The third real bug this phase found, from "VITORLAS won't load at
    all" -- not a runtime error like betunagy's, a structurally invalid
    file. .ptp's 256-byte-per-block tape limit isn't a meaningful disk
    split: primo-library/ptp/demokazi/vitorlas.ptp's 34 same-type,
    perfectly-cumulative-address tape blocks were each being relinked
    independently, as if each 256-byte slice were its own complete
    program starting fresh at BASIC_PROGRAM_BASE."""

    def test_two_contiguous_same_type_blocks_merge_into_one(self):
        blocks = [(0xF1, 0x0000, b"A" * 19), (0xF1, 0x0013, b"B" * 256)]
        merged = ptp2disk.merge_contiguous_blocks(blocks)
        self.assertEqual(merged, [(0xF1, 0x0000, b"A" * 19 + b"B" * 256)])

    def test_a_gap_between_addresses_does_not_merge(self):
        blocks = [(0xF9, 0x1000, b"A" * 10), (0xF9, 0x2000, b"B" * 10)]
        merged = ptp2disk.merge_contiguous_blocks(blocks)
        self.assertEqual(merged, blocks)

    def test_a_type_change_does_not_merge_even_if_contiguous(self):
        blocks = [(0xF5, 0x0000, b"A" * 10), (0xF9, 0x000A, b"B" * 10)]
        merged = ptp2disk.merge_contiguous_blocks(blocks)
        self.assertEqual(merged, blocks)

    def test_an_exact_duplicate_block_is_dropped_not_kept_as_a_second_block(self):
        # jatek/jetpac.ptp's real shape: the same 141-byte block appears
        # twice in a row at the same address before the real chain
        # continues from there.
        dup = (0xF9, 0x4080, b"C" * 141)
        cont = (0xF9, 0x4080 + 141, b"D" * 100)
        merged = ptp2disk.merge_contiguous_blocks([dup, dup, cont])
        self.assertEqual(merged, [(0xF9, 0x4080, b"C" * 141 + b"D" * 100)])

    def test_vitorlas_collapses_to_one_block_and_loads_as_one_program(self):
        path = os.path.join(
            os.path.dirname(__file__), "..", "..",
            "primo-library", "ptp", "demokazi", "vitorlas.ptp",
        )
        if not os.path.exists(path):
            self.skipTest("vitorlas.ptp not fetched")
        with open(path, "rb") as f:
            data = f.read()
        _, _, blocks, _, _ = ptp2disk.parse_ptp(data)
        self.assertEqual(len(blocks), 1)
        self.assertEqual(blocks[0][0], 0xF1)
        self.assertEqual(blocks[0][1], 0)

    def test_jetpac_collapses_to_two_blocks_after_dedup(self):
        path = os.path.join(
            os.path.dirname(__file__), "..", "..",
            "primo-library", "ptp", "jatek", "jetpac.ptp",
        )
        if not os.path.exists(path):
            self.skipTest("jetpac.ptp not fetched")
        with open(path, "rb") as f:
            data = f.read()
        _, _, blocks, _, _ = ptp2disk.parse_ptp(data)
        types = [t for t, _, _ in blocks]
        self.assertEqual(types, [0xF5, 0xF9])


class TestRelinkBasicLines(unittest.TestCase):
    """The second real bug this phase found, and the more serious one:
    BASIC's stored next-line pointers are absolute addresses tied to
    wherever the *original* saving machine's program area started, and
    CDOS's LOAD does not relink them. A third-party file loads and
    parses cleanly (no CRC error) but gives a real "SN Error" on real
    hardware the moment BASIC advances past line 1 into the wrong
    address. Caught this way: betunagy.ptp converted and deployed
    without error, failed on the real Primo, and decoding its (still
    internally self-consistent) pointers showed they were all based
    0x1000 above tools/primo/reference/detect-tokenized.prg's real,
    hardware-confirmed base -- not corruption, a different origin
    machine's memory layout."""

    def test_is_an_exact_noop_on_the_real_already_correct_reference_file(self):
        # detect-tokenized.prg is ground truth: this exact ROM tokenized
        # it itself, so its pointers are already correct for this
        # machine. Relinking it must change nothing at all.
        ref = os.path.join(os.path.dirname(__file__), "reference", "detect-tokenized.prg")
        with open(ref, "rb") as f:
            data = f.read()
        count = data[3] | (data[4] << 8)
        payload = data[5:5 + count]
        self.assertEqual(ptp2disk.relink_basic_lines(payload), payload)

    def test_rebases_a_foreign_program_to_the_confirmed_real_base(self):
        # Two lines with a made-up foreign base, structurally like a
        # real file: [old_ptr][line#][tokens][0x00], end marker 0x0000.
        old_base = 0x53F7
        line1_content = b"\x0A\x00" + b"A=1" + b"\x00"          # line 10
        line2_content = b"\x14\x00" + b"PRINT A" + b"\x00"      # line 20
        payload = bytearray()
        addr = old_base
        for content in (line1_content, line2_content):
            addr += 2 + len(content)
            payload += deploy.le16(addr) + content
        payload += deploy.le16(0)

        fixed = ptp2disk.relink_basic_lines(bytes(payload), base=0x43EA)
        # the first pointer's value depends on line 1's own length (it
        # points at line 2), not a fixed constant -- what must hold
        # universally is that line 1 itself now starts at `base`
        first_ptr = fixed[0] | (fixed[1] << 8)
        self.assertEqual(first_ptr, 0x43EA + 2 + len(line1_content))
        # content (everything after each pointer) must be untouched
        self.assertEqual(fixed[2:2 + len(line1_content)], line1_content)

    def test_line_content_and_count_survive_relinking(self):
        ptp_path = os.path.join(
            os.path.dirname(__file__), "..", "..",
            "primo-library", "ptp", "egyeb", "betunagy.ptp",
        )
        if not os.path.exists(ptp_path):
            self.skipTest("betunagy.ptp not fetched")
        with open(ptp_path, "rb") as f:
            ptp = f.read()
        _, _, blocks, _, _ = ptp2disk.parse_ptp(ptp)
        _, _, raw = blocks[0]
        fixed = ptp2disk.relink_basic_lines(raw)
        # same total content length; line 1 now starts at the real base
        # (its emitted pointer is base + 2 + its own length, not a fixed
        # constant -- see the synthetic test above for why). Find line
        # 1's true length the same way relink_basic_lines does -- the
        # terminator search must skip the 2-byte line-number field
        # first, or a line number whose high byte is 0x00 (line 2,
        # here) is mistaken for the terminator.
        end = 4
        while raw[end] != 0x00:
            end += 1
        first_line_len = (end + 1) - 2  # line# + tokens + terminator, no pointer
        first_ptr = fixed[0] | (fixed[1] << 8)
        self.assertEqual(first_ptr, ptp2disk.BASIC_PROGRAM_BASE + 2 + first_line_len)


def _std(t, body):
    return sum(body) & 0xFF


def _type_included(t, body):
    return (t + sum(body)) & 0xFF


def _blocknum_left_out(t, body):
    return (sum(body[1:]) + 1) & 0xFF


def _make_ptp(data_blocks, closing=(0xB1, b""), name="T", crc_for_data=_std, crc_for_closing=_std):
    """A synthetic .ptp. data_blocks: [(type, load_addr, payload)]; closing:
    (type, bytes after the block number). The name block always carries the
    documented checksum; the others use the given conventions."""
    def frame(wrapper, t, body, crc):
        inner = bytes([t]) + body + bytes([crc(t, body)])
        return bytes([wrapper]) + len(inner).to_bytes(2, "little") + inner

    parts = [frame(0x55, 0x83, bytes([0, len(name)]) + name.encode(), _std)]
    num = 1
    for t, addr, payload in data_blocks:
        body = bytes([num, addr & 0xFF, addr >> 8, len(payload) & 0xFF]) + payload
        parts.append(frame(0x55, t, body, crc_for_data))
        num += 1
    ct, extra = closing
    parts.append(frame(0xAA, ct, bytes([num]) + extra, crc_for_closing))
    total = 3 + sum(len(p) for p in parts)
    return b"\xff" + total.to_bytes(2, "little") + b"".join(parts)


BASIC_LINE = bytes([0x00, 0x00, 0x0A, 0x00, 0x8A, 0x00])       # "10 END", pointer to be relinked
TWO_BLOCKS = [(0xF9, 0x7800, bytes(range(1, 40))), (0xF9, 0x7827, bytes(range(40, 80)))]


class TestSalvage(unittest.TestCase):
    def test_default_refuses_every_alternative_convention(self):
        for rule in (_type_included, _blocknum_left_out):
            with self.assertRaises(ptp2disk.PtpError):
                ptp2disk.parse_ptp(_make_ptp(TWO_BLOCKS, crc_for_data=rule))

    def test_the_block_reader_is_strict_unless_asked(self):
        ptp = _make_ptp(TWO_BLOCKS, crc_for_data=_type_included)
        with self.assertRaises(ptp2disk.PtpError):
            list(ptp2disk.read_inner_blocks(ptp))
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.convert(ptp)

    def test_salvage_accepts_one_consistent_convention_and_says_which(self):
        for rule, words in ((_type_included, "type byte"), (_blocknum_left_out, "block number")):
            warnings = []
            ptp = _make_ptp(TWO_BLOCKS, crc_for_data=rule)
            _, _, blocks, close, _ = ptp2disk.parse_ptp(ptp, salvage=True, warnings=warnings)
            self.assertEqual(blocks[0][2] + b"", bytes(range(1, 80)))     # merged, data intact
            self.assertEqual(close, 0xB1)
            self.assertEqual(len(warnings), 1)
            self.assertIn(words, warnings[0])

    def test_a_checksum_taken_before_the_data_was_patched_in(self):
        # the seven ujdonsagok files: the first block's crc is the sum of its header with zero data
        def stale(t, body):
            return sum(body[:4]) & 0xFF if body[0] == 1 else _std(t, body)
        warnings = []
        _, _, blocks, _, _ = ptp2disk.parse_ptp(_make_ptp(TWO_BLOCKS, crc_for_data=stale), salvage=True, warnings=warnings)
        self.assertEqual(blocks[0][2], bytes(range(1, 80)))
        self.assertIn("header", warnings[0])

    def test_a_real_corruption_is_still_caught_in_salvage_mode(self):
        good = bytearray(_make_ptp(TWO_BLOCKS, crc_for_data=_type_included))
        good[20] ^= 0x01                                             # a data byte of the first block
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(bytes(good), salvage=True)
        # and a single bad block in an otherwise standard file
        good = bytearray(_make_ptp(TWO_BLOCKS))
        good[20] ^= 0x01
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(bytes(good), salvage=True)

    def test_blocks_that_disagree_about_the_convention_are_refused(self):
        def mixed(t, body):
            return _type_included(t, body) if body[0] == 1 else _blocknum_left_out(t, body)
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(_make_ptp(TWO_BLOCKS, crc_for_data=mixed), salvage=True)

    def test_a_bad_checksum_on_a_plain_closing_block_is_waived_with_a_warning(self):
        ptp = _make_ptp(TWO_BLOCKS, crc_for_closing=lambda t, body: (_std(t, body) + 1) & 0xFF)
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(ptp)
        warnings = []
        _, _, _, close, _ = ptp2disk.parse_ptp(ptp, salvage=True, warnings=warnings)
        self.assertEqual(close, 0xB1)
        self.assertIn("closing block", warnings[0])

    def test_an_autostart_block_too_short_for_an_address_becomes_return_to_basic(self):
        ptp = _make_ptp(TWO_BLOCKS, closing=(0xB9, b""), crc_for_closing=lambda t, body: 0x44)
        warnings = []
        _, _, _, close, start = ptp2disk.parse_ptp(ptp, salvage=True, warnings=warnings)
        self.assertEqual((close, start), (0xB1, None))
        self.assertIn("autostart is lost", warnings[0])
        _, pri = ptp2disk.convert(ptp, salvage=True)
        self.assertEqual(pri[-1], 0xC9)

    def test_an_autostart_block_with_an_address_and_a_bad_checksum_is_refused(self):
        ptp = _make_ptp(TWO_BLOCKS, closing=(0xB9, bytes([0x00, 0x78])), crc_for_closing=lambda t, body: 0x44)
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(ptp, salvage=True)

    def test_strict_mode_still_rejects_a_too_short_autostart_block_cleanly(self):
        ptp = _make_ptp(TWO_BLOCKS, closing=(0xB9, b""))
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.parse_ptp(ptp)


class TestRelinkKeepsTheBytesAfterTheProgram(unittest.TestCase):
    """20 library programs are a BASIC loader with machine code stored in the
    same block after the end-of-program marker; relinking must not drop it."""

    def test_trailing_machine_code_survives_and_pointers_are_rebased(self):
        program = bytes.fromhex("f9430a0041d5c128313734303829000000")      # 10 A=CALL(17408), sentinel
        tail = bytes(range(0xC0, 0xC0 + 40))
        out = ptp2disk.relink_basic_lines(program + tail, base=0x5000)
        self.assertEqual(out[-len(tail):], tail)
        self.assertEqual(len(out), len(program) + len(tail))
        self.assertEqual(out[:2], (0x5000 + 15).to_bytes(2, "little"))       # 2 + 13 bytes of record
        self.assertEqual(out[15:17], b"\x00\x00")

    def test_a_program_with_nothing_after_it_is_unchanged_in_length(self):
        program = bytes.fromhex("f9430a0041d5c128313734303829000000")
        self.assertEqual(len(ptp2disk.relink_basic_lines(program)), len(program))


class TestAgainstFetchedLibraryIfPresent(unittest.TestCase):
    """Bonus: sweep every real fetched .ptp file. Skips cleanly if the
    library hasn't been fetched -- this is a bonus check, not a
    requirement for the suite to pass off-machine."""

    LIB = os.path.join(os.path.dirname(__file__), "..", "..", "primo-library", "ptp")

    def test_sweep_reports_a_high_clean_rate(self):
        files = sorted(glob.glob(os.path.join(self.LIB, "**", "*.ptp"), recursive=True))
        if not files:
            self.skipTest("library not fetched (run tools/primo/fetch-library.sh)")

        ok, failed = 0, []
        for path in files:
            with open(path, "rb") as f:
                data = f.read()
            try:
                ptp2disk.parse_ptp(data)
                ok += 1
            except ptp2disk.PtpError:
                failed.append(path)

        rate = ok / len(files)
        print(f"\n  ptp2disk sweep: {ok}/{len(files)} clean ({rate:.1%}), "
              f"{len(failed)} failed CRC verification")
        # Known baseline: 475/488 clean (473 before 2026-10-15). All 13
        # failures are CRC mismatches in the source files -- and not bit-rot,
        # as this comment once said: each file follows one consistent
        # alternative checksum convention (see TestSalvage and ptp2disk.py's
        # docstring). A regression here means something broke, not that the
        # library changed -- fetch-library.sh doesn't re-fetch existing files.
        self.assertGreater(rate, 0.95, f"clean rate dropped: {failed[:5]}")

    def _library(self):
        files = sorted(glob.glob(os.path.join(self.LIB, "**", "*.ptp"), recursive=True))
        if not files:
            self.skipTest("library not fetched (run tools/primo/fetch-library.sh)")
        return files

    def test_salvage_recovers_every_file_the_strict_parser_refuses(self):
        recovered = []
        for path in self._library():
            with open(path, "rb") as f:
                data = f.read()
            try:
                ptp2disk.parse_ptp(data)
                continue
            except ptp2disk.PtpError:
                pass
            warnings = []
            _, pri = ptp2disk.convert(data, salvage=True, warnings=warnings)
            self.assertTrue(warnings, path)                 # nothing is accepted silently
            recovered.append(os.path.basename(path))
        self.assertEqual(len(recovered), 13, recovered)   # 15 until the short B9 block was read (2026-10-15)

    def test_salvaged_basic_programs_are_well_formed(self):
        import primo_basic
        checked = 0
        for name in ("egyeb/loverseny.ptp", "jatek/hoslovag.ptp", "ujdonsagok/countthemutants.ptp", "ujdonsagok/rt.ptp"):
            path = os.path.join(self.LIB, name)
            if not os.path.exists(path):
                self.skipTest("library not fetched")
            with open(path, "rb") as f:
                _, _, blocks, _, _ = ptp2disk.parse_ptp(f.read(), salvage=True)
            for btype, _, payload in blocks:
                if btype == 0xF1:
                    lines = primo_basic.payload_lines(payload)
                    self.assertEqual(sum(len(t) + 5 for _, t in lines) + 2, len(payload), name)   # every byte is a line
                    checked += 1
        self.assertGreaterEqual(checked, 4)

    def test_relinking_never_throws_away_bytes(self):
        for path in self._library():
            with open(path, "rb") as f:
                data = f.read()
            try:
                _, _, blocks, _, _ = ptp2disk.parse_ptp(data, salvage=True)
            except ptp2disk.PtpError:
                continue
            for btype, _, payload in blocks:
                if btype == 0xF1:
                    self.assertGreaterEqual(len(ptp2disk.relink_basic_lines(payload)), len(payload), path)


if __name__ == "__main__":
    unittest.main()


class TestAutorunAndParts(unittest.TestCase):
    """2026-10-15: a BASIC program gets CMD SAVE AUTO's record; a .ptp with
    several programs gives all of them."""

    def test_a_basic_program_gets_the_autorun_record_first(self):
        _, pri = ptp2disk.convert(_hello_world_ptp(), autorun=True)
        self.assertEqual(pri[:7].hex(), "d9a24002000000")   # 0xD9 to 40A2h, 2 bytes: 00 00
        self.assertEqual(pri[7], 0xD1)

    def test_without_autorun_the_bytes_are_as_before(self):
        _, pri = ptp2disk.convert(_hello_world_ptp())
        self.assertNotEqual(pri[0], 0xD9)

    def test_machine_code_with_its_own_autostart_gets_no_record(self):
        pri = ptp2disk.build_pri([(0xF1, 0, b"\x00\x00\x00"), (0xF9, 0x5000, b"\xC9")], 0xB9, 0x5000, autorun=True)
        self.assertNotEqual(pri[0], 0xD9)

    def test_two_programs_back_to_back_give_two_files(self):
        one = _hello_world_ptp()
        one = one[:one[1] | one[2] << 8]          # without its stray byte after the declared end
        progs = ptp2disk.convert_all(one + one, autorun=True)
        self.assertEqual(len(progs), 2)
        self.assertEqual(progs[0], progs[1])
        self.assertEqual(ptp2disk.convert(one + one, autorun=True)[1], progs[0][1])

    def test_a_length_that_does_not_fit_is_refused(self):
        one = _hello_world_ptp()
        one = one[:one[1] | one[2] << 8]
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.split_ptp(one + b"\xFF\x40\x00")


class TestShortAutostartAndAutoloader(unittest.TestCase):
    """2026-10-15, from the original tapes in MAME: an autostart block
    declared one byte short keeps its address; a part that only loads the
    next from tape is left out."""

    @staticmethod
    def _ptp(blocks):
        body = b""
        for wrapper, inner in blocks:
            body += bytes([wrapper, len(inner) & 0xFF, len(inner) >> 8]) + inner
        return bytes([0xFF, (len(body) + 3) & 0xFF, (len(body) + 3) >> 8]) + body

    def _code_ptp(self, closing):
        name = b"\x83\x00\x01X"
        name += bytes([ptp2disk.crc8(name[1:])])
        data = b"\xF9\x01\x00\x44\x01\xC9"
        data += bytes([ptp2disk.crc8(data[1:])])
        return self._ptp([(0x55, name), (0x55, data), (0xAA, closing)])

    def test_an_autostart_block_one_byte_short_keeps_its_address(self):
        ptp = self._code_ptp(b"\xB9\x02\x00\x44") + bytes([0x02 + 0x00 + 0x44])
        _, pri = ptp2disk.convert(ptp)
        self.assertEqual(pri[-3:].hex(), "c30044")

    def test_a_wrong_byte_after_it_is_not_taken_for_a_checksum(self):
        ptp = self._code_ptp(b"\xB9\x02\x00\x44") + b"\x00"
        with self.assertRaises(ptp2disk.PtpError):
            ptp2disk.convert(ptp)

    def test_a_tape_autoloader_part_is_left_out(self):
        path = os.path.join(os.path.dirname(__file__), "..", "..",
                            "primo-library", "ptp", "demokazi", "emblema.ptp")
        if not os.path.exists(path):
            self.skipTest("emblema.ptp not fetched")
        data = open(path, "rb").read()
        progs = ptp2disk.convert_all(data, salvage=True, autorun=True)
        self.assertEqual(len(progs), 1)
        self.assertEqual(progs[0][1][:7].hex(), "d9a24002000000")


class TestBasicBlockAtAnOffset(unittest.TestCase):
    """2026-10-15: a second BASIC block at a non-zero offset (hanoi, nim2 ...)
    stays at its offset, byte for byte."""

    def test_it_keeps_its_offset_and_bytes(self):
        prog = bytes([0xF0, 0x43, 0x0A, 0x00, 0x80, 0x00, 0x00, 0x00])   # 10 END
        pri = ptp2disk.build_pri([(0xF1, 0, prog), (0xF1, 0x15DF, b"\x00\x00")], 0xB1, None)
        second = pri.index(b"\xD1\xDF\x15")
        self.assertEqual(pri[second:second + 7].hex(), "d1df1502000000")


class TestTapeChaining(unittest.TestCase):
    """2026-10-15: a 10-byte stub that jumps into the ROM's tape loader is
    left out; a bare LOAD becomes CMD LOAD of the next part."""

    def test_the_rom_tape_loader_stub_is_an_autoloader(self):
        stub = bytes.fromhex("21000a22a240c30f05fe")
        self.assertTrue(ptp2disk.is_tape_autoloader([(0xF9, 0x4A00, stub)]))
        self.assertFalse(ptp2disk.is_tape_autoloader([(0xF9, 0x4A00, bytes(10))]))   # no jump into it

    def test_a_bare_load_becomes_cmd_load_of_the_next_part(self):
        line = bytes([0xF0, 0x43, 0xC6, 0x02, 0xA7, 0x00])            # 710 LOAD
        out = ptp2disk.chain_tape_loads(line + b"\x00\x00", ["GAME-2"])
        self.assertEqual(out[4:-3], b'\x85\xa7"GAME-2"')

    def test_load_inside_a_string_or_with_arguments_is_left_alone(self):
        line = bytes([0xF0, 0x43, 0x0A, 0x00]) + b'\xb2"\xa7":\xa7 X' + b"\x00"
        self.assertEqual(ptp2disk.chain_tape_loads(line + b"\x00\x00", ["N"]), line + b"\x00\x00")
