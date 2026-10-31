#!/usr/bin/env python3
"""Tests for deploy.py's .PRI/.PRG builder -- synthetic inputs, no Primo
or assembler required.

Run: python3 tools/primo/test_deploy.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import deploy


class TestCbmPrgName(unittest.TestCase):
    def test_appends_prg_when_missing(self):
        self.assertEqual(deploy.cbm_prg_name("HELLO"), "HELLO.PRG")

    def test_leaves_an_existing_prg_extension_alone(self):
        self.assertEqual(deploy.cbm_prg_name("HELLO.PRG"), "HELLO.PRG")

    def test_is_case_insensitive_about_an_existing_extension(self):
        self.assertEqual(deploy.cbm_prg_name("hello.prg"), "hello.prg")


class TestBuildPrg(unittest.TestCase):
    def test_data_block_header(self):
        prg = deploy.build_prg(b"\x00" * 47, 0x7800)
        self.assertEqual(prg[0], 0xD9)                    # machine code
        self.assertEqual(prg[1] | (prg[2] << 8), 0x7800)   # load address, LE
        self.assertEqual(prg[3] | (prg[4] << 8), 47)       # byte count, LE

    def test_data_bytes_follow_the_five_byte_header(self):
        body = bytes(range(10))
        prg = deploy.build_prg(body, 0x1000)
        self.assertEqual(prg[5:15], body)

    def test_closing_block_defaults_start_to_load_address(self):
        prg = deploy.build_prg(b"\x00" * 5, 0x7800)
        self.assertEqual(prg[-3], 0xC3)
        self.assertEqual(prg[-2] | (prg[-1] << 8), 0x7800)

    def test_closing_block_honours_an_explicit_start_address(self):
        prg = deploy.build_prg(b"\x00" * 5, 0x7800, start_addr=0x7820)
        self.assertEqual(prg[-3], 0xC3)
        self.assertEqual(prg[-2] | (prg[-1] << 8), 0x7820)

    def test_total_length_is_header_plus_body_plus_closing_block(self):
        body = b"\x00" * 47
        prg = deploy.build_prg(body, 0x7800)
        self.assertEqual(len(prg), 5 + len(body) + 3)

    def test_matches_the_real_hello_build_byte_for_byte(self):
        # The exact bytes verified by hand against konvertfajlok.html's
        # documented format before this was ever uploaded to PRIFLOP.
        body = bytes.fromhex(
            "210e787eb72806cd15002318f6c9"
            "48454c4c4f2046524f4d2054484520505249464c4f5020444556204c4f4f500d00"
        )
        prg = deploy.build_prg(body, 0x7800)
        expected = bytes.fromhex(
            "d900782f00" +
            "210e787eb72806cd15002318f6c9"
            "48454c4c4f2046524f4d2054484520505249464c4f5020444556204c4f4f500d00" +
            "c30078"
        )
        self.assertEqual(prg, expected)


class TestReadOrg(unittest.TestCase):
    def test_reads_a_lowercase_hex_org(self):
        path = os.path.join(os.path.dirname(__file__), "hello.asm")
        if not os.path.exists(path):
            self.skipTest("hello.asm not present")
        self.assertEqual(deploy.read_org(path), 0x7800)


if __name__ == "__main__":
    unittest.main()
