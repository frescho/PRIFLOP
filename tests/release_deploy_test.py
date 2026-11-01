"""scripts/release/deploy.py against a fake drive on localhost: the boot file
goes up under the single byte 0x85, a different boot file is kept unless
asked, a wrong password and a full drive stop it; a software package goes
up in its folders."""
import base64
import http.server
import importlib.util
import json
import pathlib
import shutil
import tempfile
import threading
import unittest
import urllib.parse
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
AUTH = "Basic " + base64.b64encode(b"admin:secret").decode()


class FakeDrive(http.server.BaseHTTPRequestHandler):
    files = {}
    dirs = set()
    free = 1_000_000

    def log_message(self, *a):
        pass

    def _ok(self, body=b""):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth(self):
        if self.headers.get("Authorization") == AUTH:
            return True
        self.send_response(401)
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def do_GET(self):
        if not self._auth():
            return
        if self.path.startswith("/api/list"):
            entries = [{"name": n, "size": len(d)} for n, d in self.files.items() if "/" not in n]
            entries += [{"name": d, "size": 0, "dir": True} for d in self.dirs]
            return self._ok(json.dumps({"entries": entries, "free": self.free}).encode("latin-1"))
        name = urllib.parse.unquote(self.path[len("/files/"):], encoding="latin-1")
        self._ok(self.files[name])

    def do_POST(self):
        if not self._auth():
            return
        if self.path.startswith("/api/mkdir?dir=/"):
            d = urllib.parse.unquote(self.path[len("/api/mkdir?dir=/"):], encoding="latin-1")
            if d in self.dirs:
                self.send_response(409); self.send_header("Content-Length", "0"); self.end_headers()
                return
            self.dirs.add(d)
            return self._ok()
        name = urllib.parse.unquote(self.path[len("/files/"):], encoding="latin-1")
        if "/" in name and name.split("/")[0] not in self.dirs:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
            return
        self.files[name] = self.rfile.read(int(self.headers["Content-Length"]))
        self._ok()


class TestDeploy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.HTTPServer(("127.0.0.1", 0), FakeDrive)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.ip = f"127.0.0.1:{cls.server.server_port}"
        cls.tmp = tempfile.mkdtemp()
        rel = pathlib.Path(cls.tmp)
        shutil.copy(ROOT / "scripts" / "release" / "deploy.py", rel)
        (rel / "primo").mkdir()
        (rel / "primo" / "boot.prg").write_bytes(b"BOOT")
        (rel / "primo" / "PFM.PRG").write_bytes(b"PFM")
        (rel / "c64").mkdir()
        (rel / "c64" / "IECTEST.PRG").write_bytes(b"IEC")
        cls.zip = rel / "primo_software" / "primo-library-test.zip"
        cls.zip.parent.mkdir()
        with zipfile.ZipFile(cls.zip, "w") as z:
            z.writestr("CDOS/ASM.PRG", b"ASM")
            z.writestr("JATEK/JETPAC.PRG", b"JET")
            z.writestr("LIST.TXT", "CDOS/ASM.PRG\tworks\n")
        spec = importlib.util.spec_from_file_location("deploy", rel / "deploy.py")
        cls.deploy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.deploy)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        shutil.rmtree(cls.tmp)

    def setUp(self):
        FakeDrive.files = {}
        FakeDrive.dirs = set()
        FakeDrive.free = 1_000_000

    def run_deploy(self, *extra):
        return self.deploy.main(["--ip", self.ip, "--password", "secret", *extra])

    def test_the_boot_file_is_named_with_the_byte_0x85(self):
        self.assertEqual(self.run_deploy(), 0)
        self.assertEqual(FakeDrive.files, {"\x85": b"BOOT", "PFM.PRG": b"PFM"})

    def test_a_different_boot_file_is_kept_unless_asked(self):
        FakeDrive.files = {"\x85": b"CDOS"}
        self.assertEqual(self.run_deploy(), 0)
        self.assertEqual(FakeDrive.files["\x85"], b"CDOS")
        self.assertEqual(self.run_deploy("--replace-boot"), 0)
        self.assertEqual(FakeDrive.files["\x85"], b"BOOT")

    def test_the_c64_tests_come_with_the_empty_images(self):
        FakeDrive.free = 3_000_000          # the four empty images alone are 1.6 MB
        self.assertEqual(self.run_deploy("--c64-tests"), 0)
        self.assertEqual(FakeDrive.files["IECTEST.PRG"], b"IEC")
        self.assertEqual(len(FakeDrive.files["TEST81.D81"]), 819200)

    def test_a_bundle_goes_up_in_its_folders(self):
        self.assertEqual(self.run_deploy("--bundle", str(self.zip)), 0)
        self.assertEqual(FakeDrive.dirs, {"CDOS", "JATEK"})
        self.assertEqual(FakeDrive.files["CDOS/ASM.PRG"], b"ASM")
        self.assertEqual(FakeDrive.files["JATEK/JETPAC.PRG"], b"JET")
        self.assertNotIn("LIST.TXT", FakeDrive.files)

    def test_a_bundle_twice_keeps_the_folders(self):
        self.assertEqual(self.run_deploy("--bundle", str(self.zip)), 0)
        self.assertEqual(self.run_deploy("--bundle", str(self.zip)), 0)

    def test_a_bundle_that_does_not_fit_stops_before_any_upload(self):
        FakeDrive.free = 10                 # the boot file, PFM and the bundle need 16
        self.assertEqual(self.run_deploy("--bundle", str(self.zip)), 1)
        self.assertEqual((FakeDrive.files, FakeDrive.dirs), ({}, set()))

    def test_a_missing_bundle_stops_it(self):
        self.assertEqual(self.run_deploy("--bundle", "/nonexistent.zip"), 2)
        self.assertEqual(FakeDrive.files, {})

    def test_dry_run_uploads_nothing(self):
        self.assertEqual(self.run_deploy("--dry-run"), 0)
        self.assertEqual(FakeDrive.files, {})

    def test_a_wrong_password_stops_it(self):
        self.assertEqual(self.deploy.main(["--ip", self.ip, "--password", "wrong"]), 2)
        self.assertEqual(FakeDrive.files, {})

    def test_a_full_drive_stops_it_before_any_upload(self):
        FakeDrive.free = 5
        self.assertEqual(self.run_deploy(), 1)
        self.assertEqual(FakeDrive.files, {})


if __name__ == "__main__":
    unittest.main()
