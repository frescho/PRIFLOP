"""HUNGTEST (tools/hungtest.bas): the DATA bytes are what ca65 makes of
tools/hungtest.s, and the program tokenizes. Negative control: a changed
byte must be caught. Needs ca65/ld65 (cc65 package); skipped without them.
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import basic_tokenize  # noqa: E402


def data_bytes(src):
    out = []
    for line in src.splitlines():
        m = re.match(r"\d+ DATA (.*)", line)
        if m:
            out += [int(v) for v in m.group(1).split(",")]
    return bytes(out)


def assemble():
    with tempfile.TemporaryDirectory() as d:
        obj, binf = pathlib.Path(d, "h.o"), pathlib.Path(d, "h.bin")
        subprocess.run(["ca65", "-o", obj, ROOT / "tools" / "hungtest.s"], check=True)
        subprocess.run(["ld65", "-t", "none", "-o", binf, obj], check=True)
        return binf.read_bytes()


@unittest.skipUnless(shutil.which("ca65") and shutil.which("ld65"), "no cc65")
class HungTest(unittest.TestCase):
    def setUp(self):
        self.src = (ROOT / "tools" / "hungtest.bas").read_text()

    def test_data_matches_assembly(self):
        code = assemble()
        self.assertEqual(data_bytes(self.src), code)
        self.assertIn(f"FOR I=0 TO {len(code) - 1}:", self.src)

    def test_changed_byte_is_caught(self):
        bad = self.src.replace("DATA 162,2,", "DATA 162,3,")
        self.assertNotEqual(data_bytes(bad), assemble())

    def test_tokenizes(self):
        self.assertGreater(len(basic_tokenize.build(self.src)), 100)


if __name__ == "__main__":
    unittest.main()
