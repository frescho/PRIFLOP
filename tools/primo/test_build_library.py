"""build_library.py: names, folders, bin/ left out, and a cut that collides."""
import os
import tempfile
import unittest

import build_library


def make(root, files):
    for path, data in files.items():
        p = os.path.join(root, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)


class TestBuildLibrary(unittest.TestCase):
    def test_prg_files_keep_their_folder_and_bytes_under_an_upper_case_name(self):
        with tempfile.TemporaryDirectory() as d:
            make(d, {"ptp/cdos/copy-87.prg": b"\x01\x02", "bin/cdos/boot80.prg": b"x"})
            files, _ = build_library.build(d)
            self.assertEqual(files, {"CDOS/COPY-87.PRG": b"\x01\x02"})     # bin/ is not shipped

    def test_names_are_cut_to_sixteen(self):
        self.assertEqual(build_library.drive_name("rajzolo(eletjatek)"), "RAJZOLO(ELETJATE.PRG")

    def test_two_names_cut_to_the_same_sixteen_stop_the_build(self):
        with tempfile.TemporaryDirectory() as d:
            make(d, {"ptp/x/abcdefghijklmnop1.prg": b"1", "ptp/x/abcdefghijklmnop2.prg": b"2"})
            with self.assertRaises(ValueError):
                build_library.build(d)

    def test_the_same_name_in_two_folders_is_fine(self):
        with tempfile.TemporaryDirectory() as d:
            make(d, {"ptp/a/dallam.prg": b"1", "ptp/b/dallam.prg": b"2"})
            files, _ = build_library.build(d)
            self.assertEqual(sorted(files), ["A/DALLAM.PRG", "B/DALLAM.PRG"])


if __name__ == "__main__":
    unittest.main()
