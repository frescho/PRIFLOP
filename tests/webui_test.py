#!/usr/bin/env python3
"""Regression tests for the web page the device serves (PAGE_HTML in
src/pico/httpd.c).

    python3 tests/webui_test.py

Written after "the Settings tab reopens on every reload":
the page forced the Settings tab open while the default admin password was set,
and also remembered the last tab in localStorage, so removing only the first
would still have reopened on Settings for anyone whose browser had stored it.

Two layers:
  * static checks on the page text, which run everywhere;
  * a real-browser check: the page is served verbatim by a mock device
    (/api/wifi, /api/stats, /api/list) to headless Firefox, and a small script
    appended to the page reports which tab is visible, whether the
    default-password banner shows, and what localStorage holds. Skipped when
    Firefox is not installed or PRIFLOP_SKIP_BROWSER=1. Nothing here touches
    a real device, and the browser profile lives in a temporary directory.
"""
import http.server
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

HTTPD_C = Path(__file__).resolve().parent.parent / "src" / "pico" / "httpd.c"


def page_html() -> str:
    """PAGE_HTML exactly as the device would send it (C string literal, unescaped)."""
    text = HTTPD_C.read_text(encoding="utf-8")
    start = text.index("static const char PAGE_HTML[] =")
    end = text.index(";\n", text.index('</body></html>\\n"', start))
    lines = re.findall(r'^"(.*)"\s*$', text[start:end], re.M)

    def unescape(s):
        def rep(m):
            g = m.group(1)
            return {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}[g] if len(g) == 1 else chr(int(g[1:], 16))
        return re.sub(r'\\(n|t|r|"|\\|u[0-9a-fA-F]{4})', rep, s)

    return "".join(unescape(l) for l in lines)


class TestPageText(unittest.TestCase):
    def setUp(self):
        self.html = page_html()

    def test_extraction_found_the_page(self):
        self.assertTrue(self.html.lstrip().lower().startswith("<!doctype html>"))
        self.assertIn("</body></html>", self.html)

    def test_showtab_is_only_ever_called_by_the_two_tab_buttons(self):
        calls = [m.start() for m in re.finditer(r"showTab\(", self.html)]
        definition = self.html.index("function showTab(")
        allowed = {definition + len("function ")}
        for handler in ("onclick=\"showTab('files')\"", "onclick=\"showTab('settings')\""):
            i = self.html.index(handler)
            allowed.add(i + len('onclick="'))
        self.assertEqual(set(calls), allowed, "something other than a button calls showTab()")

    def test_the_favicon_is_the_orange_p_keycap_inline(self):
        head = self.html[:self.html.index("</head>")]
        m = re.search(r'<link rel="icon" href="data:image/svg\+xml,([^"]*)">', head)
        self.assertIsNotNone(m, "no inline SVG favicon in <head>")
        svg = m.group(1).replace("%23", "#")
        import xml.etree.ElementTree as ET
        root = ET.fromstring(svg)                  # well-formed, or this raises
        self.assertTrue(root.tag.endswith("svg"))
        self.assertIn("fill='#cf880c'", svg)       # --primo-orange, as the header's PRI
        self.assertIn(">P</text>", svg)
        self.assertNotIn("#", m.group(1), "a raw # ends a data: URL early")

    def test_no_tab_is_remembered(self):
        self.assertNotIn("priflop.tab", self.html)
        self.assertNotRegex(self.html, r"localStorage[^;\n]*tab", "the page stores or reads a tab")

    def test_the_default_password_banner_says_where_to_change_it(self):
        banner = self.html[self.html.index('id="defwarn"'):]
        banner = banner[:banner.index("</div>")]
        self.assertIn("Settings", banner)
        self.assertIn("Admin password", banner)


class TestStatsEndpoint(unittest.TestCase):
    """/api/stats is one snprintf() into a fixed buffer. The buffer was 1200 bytes
    and the worst case (every counter ten digits wide) was already 1267 before the
    health counters were added; the length is used for Content-Length and for the
    copy, so a truncated result is not just bad JSON but a read past the buffer."""

    @staticmethod
    def _stats_source():
        text = HTTPD_C.read_text(encoding="utf-8")
        start = text.index('if (!strcmp(c->raw_path, "/api/stats")) {')
        call = text[text.index("snprintf(body", start):text.index("(unsigned long)stack_high_water()", start)]
        fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', call)).replace('\\"', '"')
        size = int(re.search(r"(?:static )?char body\[(\d+)\]", text[start:]).group(1))
        return fmt, size

    def _render(self, fmt, number, hexnum):
        fmt = fmt.replace("%08lx", hexnum).replace("%s", "ff ff ff ff ff ff ff ff")
        return re.sub(r"%l?[ud]", number, fmt)

    def test_the_output_is_valid_json_and_fits_the_buffer_even_at_its_widest(self):
        fmt, size = self._stats_source()
        for number, hexnum in (("7", "00000007"), ("4294967295", "ffffffff"), ("-2147483648", "ffffffff")):
            out = self._render(fmt, number, hexnum)
            self.assertLess(len(out), size, f"{len(out)} bytes into a {size}-byte buffer")
            json.loads(out)

    def test_every_field_the_page_reads_is_sent(self):
        fmt, _ = self._stats_source()
        sent = set(json.loads(self._render(fmt, "1", "00000001")))
        html = page_html()
        body = html[html.index("async function loadStats"):]
        body = body[:body.index("async function loadNet")]
        used = set(re.findall(r"\bd\.([a-z_0-9]+)", body))
        self.assertTrue(used, "found no fields in loadStats()")
        self.assertEqual(sorted(used - sent), [], "the page reads fields /api/stats does not send")

    def test_the_health_fields_are_there_for_when_the_device_next_wedges(self):
        fmt, _ = self._stats_source()
        sent = set(json.loads(self._render(fmt, "1", "00000001")))
        for key in ("c1_loops", "c1_gap_max_ms", "c1_gate_timeouts", "c1_stack_used", "c1_stack_size",
                    "wifi_link", "wifi_link_drops", "wifi_rx", "wifi_rx_age_ms", "fsb_wait_max_ms"):
            self.assertIn(key, sent)

    def test_the_long_transmit_waits_are_counted(self):
        """Decision 11b (2026-10-15): a hung listener is waited for with the
        watchdog fed; tx_long_waits is how a bench test sees that happen."""
        fmt, _ = self._stats_source()
        self.assertIn("tx_long_waits", set(json.loads(self._render(fmt, "1", "00000001"))))


class TestExtensionHiding(unittest.TestCase):
    """2026-10-02: extension hiding can be set from the Settings tab, because
    every reflash reset it and restoring it meant two DOS commands on the C64."""

    def test_the_page_posts_what_the_server_accepts(self):
        html = page_html()
        self.assertIn("/api/settings?exthide=", html)
        self.assertIn('id="wexthide"', html)
        src = HTTPD_C.read_text(encoding="utf-8")
        handler = src[src.index('"/api/settings"'):]
        handler = handler[:handler.index("return;\n  }\n")]
        self.assertIn('"exthide"', handler)
        self.assertIn("pending_ext_hiding", handler)

    def test_the_server_queues_it_for_core_0_and_does_not_apply_it_itself(self):
        src = HTTPD_C.read_text(encoding="utf-8")
        self.assertNotRegex(src, r"globalflags\s*[|&]?=", "core 1 must not write globalflags")
        iec = (HTTPD_C.parent.parent / "iec.c").read_text(encoding="utf-8")
        apply = iec[iec.index("if (pending_ext_hiding >= 0)"):]
        apply = apply[:apply.index("write_configuration();") + 1]
        self.assertIn("EXTENSION_HIDING", apply)

    def test_the_page_shows_the_current_state(self):
        fmt, _ = TestStatsEndpoint._stats_source()
        sent = json.loads(TestStatsEndpoint()._render(fmt, "1", "00000001"))
        self.assertIn("ext_hiding", sent)


class TestPrimoTurboSetting(unittest.TestCase):
    """2026-10-07: the Primo turbo can be switched on and saved from the
    Settings tab, because our own Primo DOS needs it and a drive restart used
    to turn it off."""

    def test_the_page_posts_what_the_server_accepts(self):
        html = page_html()
        self.assertIn("/api/settings?primoturbo=", html)
        self.assertIn('id="wpturbo"', html)
        src = HTTPD_C.read_text(encoding="utf-8")
        handler = src[src.index('"/api/settings"'):]
        handler = handler[:handler.index("return;\n  }\n")]
        self.assertIn('"primoturbo"', handler)
        self.assertIn("pending_primo_turbo", handler)

    def test_core_0_applies_and_saves_it(self):
        iec = (HTTPD_C.parent.parent / "iec.c").read_text(encoding="utf-8")
        apply = iec[iec.index("if (pending_primo_turbo >= 0)"):]
        apply = apply[:apply.index("write_configuration();") + 1]
        self.assertIn("PRIMO_TURBO", apply)
        self.assertIn("primo_turbo_apply_setting()", apply)
        conf = (HTTPD_C.parent.parent / "eeprom-conf.c").read_text(encoding="utf-8")
        self.assertEqual(conf.count("PRIMO_TURBO"), 2, "read and written with the saved flags")
        main = (HTTPD_C.parent.parent / "main.c").read_text(encoding="utf-8")
        self.assertLess(main.index("read_configuration();"), main.index("primo_turbo_apply_setting();"))

    def test_the_page_shows_the_current_state(self):
        fmt, _ = TestStatsEndpoint._stats_source()
        sent = json.loads(TestStatsEndpoint()._render(fmt, "1", "00000001"))
        self.assertIn("pt_on", sent)


# ---------------------------------------------------------------- browser layer

FIREFOX = shutil.which("firefox")

GIF = bytes.fromhex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b")

REPORTER = """<img src="/slow.gif" width="1" height="1">
<script>
if(location.hash==="#click") setTimeout(function(){showTab("settings")},900);
setTimeout(function(){
 var st=null; try{st=localStorage.getItem('priflop.tab')}catch(e){}
 fetch('/report',{method:'POST',body:JSON.stringify({
   files_hidden:document.getElementById('tab-files').hidden,
   settings_hidden:document.getElementById('tab-settings').hidden,
   banner_hidden:document.getElementById('defwarn').hidden,
   stored_tab:st})});
},1500);
</script>"""

SEED = """<!doctype html><script>localStorage.setItem('priflop.tab','settings')</script>seeded"""


class MockDevice(http.server.ThreadingHTTPServer):
    def __init__(self, html, default_admin):
        self.reports = []
        wifi = {"mode": "AP", "ssid": "", "ap_ssid": "PRIFLOP", "mdns": "priflop", "country": "",
                "has_password": False, "has_ap_password": False, "has_admin": not default_admin,
                "default_admin": default_admin}
        stats = {"stack_used": 3000, "stack_total": 8192, "faulted": 0, "fault_pc": "0x0", "flash_ops": 100,
                 "flash_op_max_us": 39000, "atn_irqs": 0, "boots": 1, "wdog": "0x40000000", "reformatted": 0,
                 "atn_bails": 0, "rx": 1000, "tx": 2000, "getc_max": 10, "putc_max": 10, "gap_max": 10,
                 "busy_ms": 100, "pio": 1, "pio_ok": 5, "pio_bad": 0, "pio_owns": 1, "pio_tx": 1, "pio_txb": 5,
                 "pio_txa": 0, "pio_txn": 0, "jf_rx": 0, "jf_tx": 0, "jf_us": 0, "device_address": 8}
        listing = {"path": "/", "entries": [{"name": "HELLO.PRG", "dir": False, "size": 123}],
                   "free": 2800000, "total": 3141632}
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, body, ctype):
                data = body if isinstance(body, bytes) else body.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                path = self.path.split("?")[0]
                if path == "/":
                    self._send(html.replace("</body>", REPORTER + "</body>"), "text/html; charset=utf-8")
                elif path == "/seed":
                    self._send(SEED, "text/html")
                elif path == "/slow.gif":
                    time.sleep(2.5)          # keeps the load event waiting until the page's own fetches are done
                    self._send(GIF, "image/gif")
                elif path == "/api/wifi":
                    self._send(json.dumps(wifi), "application/json")
                elif path == "/api/stats":
                    self._send(json.dumps(stats), "application/json")
                elif path == "/api/list":
                    self._send(json.dumps(listing), "application/json")
                else:
                    self.send_error(404)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
                if self.path == "/report":
                    outer.reports.append(json.loads(body))
                self._send("{}", "application/json")

        super().__init__(("127.0.0.1", 0), Handler)


@unittest.skipUnless(FIREFOX and os.environ.get("PRIFLOP_SKIP_BROWSER") != "1",
                     "Firefox not installed (or PRIFLOP_SKIP_BROWSER=1)")
class TestInABrowser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = page_html()

    def visit(self, default_admin=True, stored_tab=None, click=False):
        """Load the page in a fresh headless Firefox profile; returns the page's own report."""
        device = MockDevice(self.html, default_admin)
        threading.Thread(target=device.serve_forever, daemon=True).start()
        base = f"http://127.0.0.1:{device.server_address[1]}"
        profile = tempfile.mkdtemp(prefix="priflop-ff-")
        try:
            def shoot(url):
                return subprocess.run(
                    [FIREFOX, "--headless", "--profile", profile, "--screenshot",
                     os.path.join(profile, "shot.png"), "--window-size=1150,1050", url],
                    timeout=120, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if stored_tab:
                shoot(base + "/seed")
            r = shoot(base + "/" + ("#click" if click else ""))
            for _ in range(30):
                if device.reports:
                    break
                time.sleep(0.1)
            if not device.reports:
                if r.returncode != 0:
                    self.skipTest(f"Firefox could not run headless here (exit {r.returncode})")
                self.fail("the page never reported: it did not finish loading")
            return device.reports[-1]
        finally:
            device.shutdown()
            device.server_close()
            shutil.rmtree(profile, ignore_errors=True)

    def test_opens_on_files_with_the_default_password(self):
        r = self.visit(default_admin=True)
        self.assertEqual((r["files_hidden"], r["settings_hidden"]), (False, True), r)
        self.assertFalse(r["banner_hidden"], "the default-password banner should be showing")

    def test_a_remembered_settings_tab_does_not_reopen_it(self):
        # the second half of the original bug: a stored tab used to be restored on load
        r = self.visit(default_admin=True, stored_tab="settings")
        self.assertEqual((r["files_hidden"], r["settings_hidden"]), (False, True), r)

    def test_opens_on_files_with_a_custom_password(self):
        r = self.visit(default_admin=False)
        self.assertEqual((r["files_hidden"], r["settings_hidden"]), (False, True), r)
        self.assertTrue(r["banner_hidden"], "no banner once the password is set")

    def test_the_settings_tab_still_opens_when_asked_and_nothing_is_remembered(self):
        r = self.visit(default_admin=True, click=True)
        self.assertEqual((r["files_hidden"], r["settings_hidden"]), (True, False), r)
        self.assertIsNone(r["stored_tab"])


if __name__ == "__main__":
    unittest.main()
