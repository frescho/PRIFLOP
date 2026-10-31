/* netconfig_test.c: host-side (native, not ARM) unit tests for
   src/pico/netconfig.c's pure-function parsing logic -- see
   netconfig.h's design note. Compiled with a native compiler and run
   directly on the dev machine, no pico-sdk/hardware involved:

     gcc -I ../src/pico -DNETCONFIG_HOST_TEST \
         netconfig_test.c ../src/pico/netconfig.c -o netconfig_test
     ./netconfig_test

   This is the actual verification for "a missing or damaged config
   file falls back to defaults safely" -- real hardware testing can't
   easily exercise a corrupted-file case, this can.
*/
#include <stdio.h>
#include <string.h>

#include "netconfig.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
      printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++; \
    } \
  } while (0)

#define CHECK_STR(actual, expected, msg) do { \
    if (strcmp((actual), (expected)) != 0) { \
      printf("FAIL: %s -- got \"%s\", expected \"%s\" (line %d)\n", \
             msg, (actual), (expected), __LINE__); \
      failures++; \
    } \
  } while (0)

/* Parses `text` (a string literal/array, safely copyable) into a
   freshly-defaulted cfg. Copies into a local mutable buffer with a
   spare trailing byte, matching netconfig_load()'s real contract. */
static void parse(net_config_t *cfg, const char *text) {
  char buf[1024];
  unsigned int len = (unsigned int)strlen(text);
  memcpy(buf, text, len);
  netconfig_set_defaults(cfg);
  netconfig_parse_buffer(cfg, buf, len);
}

static void test_defaults(void) {
  net_config_t cfg;
  netconfig_set_defaults(&cfg);
  CHECK(cfg.ap_mode == true, "defaults: ap_mode should be true");
  CHECK_STR(cfg.ssid, "PRIFLOP", "defaults: ssid");
  CHECK_STR(cfg.mdns_name, "PRIFLOP", "defaults: mdns_name");
  CHECK_STR(cfg.password, "", "defaults: password");
  /* The web UI is protected out of the box (2026-09-04). */
  CHECK_STR(cfg.admin_password, "admin", "defaults: admin_password");
}

/* An empty ADMIN= line must NOT disable authentication. set_field()
   ignores empty values, so it falls back to the compiled-in default --
   which is now non-empty. This is deliberate: the config file holds the
   Wi-Fi password and is downloadable through the interface it protects,
   so auth is changeable but not switchable-off from the file. */
static void test_empty_admin_does_not_disable_auth(void) {
  net_config_t cfg;
  netconfig_set_defaults(&cfg);
  char buf[] = "MODE=AP\r\nADMIN=\r\n";
  netconfig_parse_buffer(&cfg, buf, (unsigned int)strlen(buf));
  CHECK_STR(cfg.admin_password, "admin", "empty ADMIN= keeps the default");
}

static void test_admin_password_can_be_changed(void) {
  net_config_t cfg;
  netconfig_set_defaults(&cfg);
  char buf[] = "ADMIN=hunter2\r\n";
  netconfig_parse_buffer(&cfg, buf, (unsigned int)strlen(buf));
  CHECK_STR(cfg.admin_password, "hunter2", "ADMIN= sets a new password");
}

static void test_empty_buffer(void) {
  net_config_t cfg;
  parse(&cfg, "");
  CHECK_STR(cfg.ssid, "PRIFLOP", "empty buffer: ssid stays default");
  CHECK(cfg.ap_mode == true, "empty buffer: ap_mode stays default");
}

static void test_full_valid_file(void) {
  net_config_t cfg;
  parse(&cfg,
        "MODE=STA\n"
        "SSID=myhomewifi\n"
        "PASSWORD=supersecret1\n"
        "MDNS=priflop2\n"
        "ADMIN=hunter2\n");
  CHECK(cfg.ap_mode == false, "full file: MODE=STA sets ap_mode false");
  CHECK_STR(cfg.ssid, "myhomewifi", "full file: ssid");
  CHECK_STR(cfg.password, "supersecret1", "full file: password");
  CHECK_STR(cfg.mdns_name, "priflop2", "full file: mdns");
  CHECK_STR(cfg.admin_password, "hunter2", "full file: admin");
}

static void test_case_insensitive_keys_and_mode_ap(void) {
  net_config_t cfg;
  parse(&cfg, "mode=ap\nssid=lowercasekey\n");
  CHECK(cfg.ap_mode == true, "case-insensitive: mode=ap");
  CHECK_STR(cfg.ssid, "lowercasekey", "case-insensitive: ssid key");
}

static void test_crlf_line_endings(void) {
  net_config_t cfg;
  parse(&cfg, "SSID=crlftest\r\nMDNS=crlfmdns\r\n");
  CHECK_STR(cfg.ssid, "crlftest", "CRLF: ssid");
  CHECK_STR(cfg.mdns_name, "crlfmdns", "CRLF: mdns");
}

static void test_no_trailing_newline(void) {
  net_config_t cfg;
  parse(&cfg, "SSID=lastline");
  CHECK_STR(cfg.ssid, "lastline", "no trailing newline: last line still parsed");
}

static void test_whitespace_around_key_and_value(void) {
  net_config_t cfg;
  parse(&cfg, "  SSID   =   spaced out  \n");
  CHECK_STR(cfg.ssid, "spaced out", "whitespace: trimmed on both sides");
}

static void test_empty_value_does_not_blank_field(void) {
  net_config_t cfg;
  parse(&cfg, "SSID=\n");
  CHECK_STR(cfg.ssid, "PRIFLOP", "empty value: ssid keeps default, not blanked");
}

static void test_unrecognised_mode_value_is_ignored(void) {
  net_config_t cfg;
  parse(&cfg, "MODE=banana\n");
  CHECK(cfg.ap_mode == true, "garbage MODE value: ap_mode stays at default (true)");
}

static void test_unknown_key_ignored(void) {
  net_config_t cfg;
  parse(&cfg, "NOTAREALKEY=whatever\nSSID=stillworks\n");
  CHECK_STR(cfg.ssid, "stillworks", "unknown key: doesn't stop later lines parsing");
}

static void test_value_containing_equals_sign(void) {
  net_config_t cfg;
  parse(&cfg, "PASSWORD=abc=def=ghi\n");
  CHECK_STR(cfg.password, "abc=def=ghi", "value with '=': only first '=' splits key/value");
}

static void test_malformed_line_no_equals(void) {
  net_config_t cfg;
  parse(&cfg, "THIS LINE HAS NO EQUALS SIGN\nSSID=survives\n");
  CHECK_STR(cfg.ssid, "survives", "malformed line: skipped safely, doesn't stop parsing");
}

static void test_overlong_value_truncated_safely(void) {
  net_config_t cfg;
  char buf[1024] = "SSID=";
  size_t prefix = strlen(buf);
  memset(buf + prefix, 'A', sizeof(buf) - prefix - 2);
  buf[sizeof(buf) - 2] = '\n';
  buf[sizeof(buf) - 1] = 0;
  netconfig_set_defaults(&cfg);
  netconfig_parse_buffer(&cfg, buf, (unsigned int)strlen(buf));
  CHECK(strlen(cfg.ssid) == NETCONFIG_SSID_MAX - 1,
        "overlong value: truncated to field size - 1");
  CHECK(cfg.ssid[NETCONFIG_SSID_MAX - 1] == 0,
        "overlong value: still NUL-terminated");
}

static void test_binary_garbage_content(void) {
  net_config_t cfg;
  netconfig_set_defaults(&cfg);
  /* Simulate a config file overwritten with random binary junk (e.g.
     a stray PRG landed at this path, or flash corruption) -- must not
     crash, and must leave every field at its default. */
  char buf[64];
  for (int i = 0; i < 63; i++) buf[i] = (char)((i * 37 + 11) & 0xFF);
  buf[63] = 0;
  netconfig_parse_buffer(&cfg, buf, 63);
  CHECK_STR(cfg.ssid, "PRIFLOP", "binary garbage: ssid stays default");
  CHECK_STR(cfg.mdns_name, "PRIFLOP", "binary garbage: mdns stays default");
  CHECK(cfg.ap_mode == true, "binary garbage: ap_mode stays default");
}

static void test_embedded_nul_byte(void) {
  net_config_t cfg;
  netconfig_set_defaults(&cfg);
  /* A NUL partway through a "line" (e.g. binary corruption mid-file)
     must not read past it or crash -- the line is just cut short. */
  char buf[] = "SSID=before\0garbage_after_nul\nMDNS=stillparsed\n";
  /* sizeof(buf) counts the array's trailing NUL too; the embedded NUL
     after "before" is just data in the middle, not the end. */
  unsigned int len = (unsigned int)(sizeof(buf) - 1);
  netconfig_parse_buffer(&cfg, buf, len);
  CHECK_STR(cfg.ssid, "before", "embedded NUL: value cut short at the NUL, not overrun");
  CHECK_STR(cfg.mdns_name, "stillparsed", "embedded NUL: parsing resumes on the next real line");
}

static void test_mixed_line_endings_and_blank_lines(void) {
  net_config_t cfg;
  parse(&cfg, "\n\nSSID=afterblanks\r\n\nMDNS=alsoworks\n\n");
  CHECK_STR(cfg.ssid, "afterblanks", "blank lines: skipped without issue");
  CHECK_STR(cfg.mdns_name, "alsoworks", "blank lines: later keys still parsed");
}


/* --- netconfig_serialize() round-trip ------------------------------- */

/* The guarantee the web UI's "save Wi-Fi settings" depends on: whatever
   we write must read back as the same config. Checked here rather than
   on hardware because it's a pure function -- exactly the split
   netconfig.h's design note describes. */
static void test_serialize_round_trip(void) {
  net_config_t out;
  netconfig_set_defaults(&out);
  out.ap_mode = false;
  strcpy(out.ssid, "My Home Net");
  strcpy(out.password, "hunter2hunter2");
  strcpy(out.ap_ssid, "PRIFLOP");
  strcpy(out.ap_password, "apsecret1");
  strcpy(out.mdns_name, "priflop");
  strcpy(out.admin_password, "s3cret");

  char buf[512];
  unsigned int len = netconfig_serialize(&out, buf, sizeof(buf));
  CHECK(len > 0, "serialize should succeed with a large buffer");
  CHECK(strlen(buf) == len, "returned length should match strlen");

  net_config_t back;
  netconfig_set_defaults(&back);
  netconfig_parse_buffer(&back, buf, len);

  CHECK(back.ap_mode == false, "round-trip should preserve STA mode");
  CHECK_STR(back.ssid, "My Home Net", "round-trip ssid (with spaces)");
  CHECK_STR(back.password, "hunter2hunter2", "round-trip password");
  CHECK_STR(back.ap_ssid, "PRIFLOP", "round-trip ap_ssid");
  CHECK_STR(back.ap_password, "apsecret1", "round-trip ap_password");
  CHECK_STR(back.mdns_name, "priflop", "round-trip mdns");
  CHECK_STR(back.admin_password, "s3cret", "round-trip admin");
}

static void test_serialize_ap_mode_round_trip(void) {
  net_config_t out;
  netconfig_set_defaults(&out);
  out.ap_mode = true;
  strcpy(out.ssid, "PRIFLOP");

  char buf[512];
  unsigned int len = netconfig_serialize(&out, buf, sizeof(buf));
  CHECK(len > 0, "serialize should succeed");

  net_config_t back;
  netconfig_set_defaults(&back);
  back.ap_mode = false;              /* start opposite, so we know it was set */
  netconfig_parse_buffer(&back, buf, len);
  CHECK(back.ap_mode == true, "round-trip should preserve AP mode");
}

/* Overflow must produce nothing at all, not a truncated file: a partial
   PRIFLOP.CFG would still parse and would silently drop whichever fields
   fell off the end. */
static void test_serialize_buffer_too_small(void) {
  net_config_t out;
  netconfig_set_defaults(&out);
  strcpy(out.ssid, "SomeReasonablyLongNetworkName");

  char buf[16];
  memset(buf, 0x7f, sizeof(buf));
  unsigned int len = netconfig_serialize(&out, buf, sizeof(buf));
  CHECK(len == 0, "serialize into a too-small buffer must return 0");
  CHECK(buf[0] == 0x7f, "failed serialize must not write a partial result");
}

/* An empty password is legitimate (open network). It must survive as
   empty rather than being dropped -- the parser's empty-value rule means
   the emitted "PASSWORD=" line is ignored on read, so the field keeps
   whatever set_defaults put there, which is also empty. Either way the
   round-tripped result must be empty, which is what actually matters. */
static void test_serialize_empty_password(void) {
  net_config_t out;
  netconfig_set_defaults(&out);
  out.password[0] = 0;
  strcpy(out.ssid, "OpenNet");

  char buf[512];
  unsigned int len = netconfig_serialize(&out, buf, sizeof(buf));
  CHECK(len > 0, "serialize with empty password should succeed");

  net_config_t back;
  netconfig_set_defaults(&back);
  back.password[0] = 0;
  netconfig_parse_buffer(&back, buf, len);
  CHECK_STR(back.password, "", "empty password round-trips as empty");
  CHECK_STR(back.ssid, "OpenNet", "ssid still round-trips alongside it");
}

/* The bug this split exists to prevent: an AP-mode fallback broadcasting
   the *home* network's name (and password). They must stay independent. */
static void test_ap_and_sta_ssids_are_independent(void) {
  net_config_t cfg;
  netconfig_set_defaults(&cfg);
  char buf[] = "MODE=STA\r\nSSID=HomeNet\r\nPASSWORD=homepass1\r\n"
               "AP_SSID=PRIFLOP\r\nAP_PASSWORD=\r\n";
  netconfig_parse_buffer(&cfg, buf, (unsigned int)strlen(buf));

  CHECK_STR(cfg.ssid, "HomeNet", "sta ssid parsed");
  CHECK_STR(cfg.password, "homepass1", "sta password parsed");
  CHECK_STR(cfg.ap_ssid, "PRIFLOP", "ap ssid stays separate");
  CHECK(strcmp(cfg.ap_ssid, cfg.ssid) != 0, "ap ssid must not become the sta ssid");
  CHECK(strcmp(cfg.ap_password, cfg.password) != 0,
        "home password must never end up on the broadcast AP");
}


/* --- admin password hashing ------------- */

static void test_plaintext_admin_still_authenticates(void) {
  /* A config written before hashing existed must keep working. */
  net_config_t c;
  netconfig_set_defaults(&c);
  parse(&c, "ADMIN=hunter2\n");
  CHECK(!netconfig_admin_is_hashed(&c), "plaintext stays plaintext on load");
  CHECK(netconfig_admin_matches(&c, "hunter2"), "plaintext accepts right password");
  CHECK(!netconfig_admin_matches(&c, "wrong"), "plaintext rejects wrong password");
}

static void test_hashed_admin_authenticates(void) {
  net_config_t c;
  netconfig_set_defaults(&c);
  netconfig_set_admin(&c, "s3cret");
  CHECK(netconfig_admin_is_hashed(&c), "set_admin stores a digest");
  CHECK(strstr(c.admin_password, "s3cret") == NULL,
         "plaintext does not survive anywhere in the stored form");
  CHECK(netconfig_admin_matches(&c, "s3cret"), "hash accepts right password");
  CHECK(!netconfig_admin_matches(&c, "s3cres"), "hash rejects wrong password");
  CHECK(!netconfig_admin_matches(&c, ""), "hash rejects empty password");
}

static void test_salt_differs_between_devices(void) {
  net_config_t a, b;
  netconfig_set_defaults(&a);
  netconfig_set_defaults(&b);
  netconfig_set_admin(&a, "same");
  netconfig_set_admin(&b, "same");
  CHECK(strcmp(a.admin_password, b.admin_password) != 0,
         "same password gives different digests (salt is doing its job)");
  CHECK(netconfig_admin_matches(&a, "same") && netconfig_admin_matches(&b, "same"),
         "both still verify");
}

static void test_hashed_admin_survives_round_trip(void) {
  net_config_t c, back;
  char buf[512];
  netconfig_set_defaults(&c);
  netconfig_set_admin(&c, "roundtrip");
  unsigned int len = netconfig_serialize(&c, buf, sizeof(buf));
  CHECK(len > 0, "serialize succeeds with a hashed admin password");
  netconfig_set_defaults(&back);
  netconfig_parse_buffer(&back, buf, len);
  CHECK(strcmp(back.admin_password, c.admin_password) == 0,
         "hashed line round-trips byte-for-byte");
  CHECK(netconfig_admin_matches(&back, "roundtrip"),
         "and still authenticates after the round trip");
}

static void test_rehash_migrates_plaintext(void) {
  net_config_t c;
  netconfig_set_defaults(&c);
  parse(&c, "ADMIN=oldplain\n");
  netconfig_rehash_admin(&c);
  CHECK(netconfig_admin_is_hashed(&c), "rehash converts plaintext");
  CHECK(netconfig_admin_matches(&c, "oldplain"),
         "the same password still works after migration");
  char before[NETCONFIG_ADMIN_MAX];
  snprintf(before, sizeof(before), "%s", c.admin_password);
  netconfig_rehash_admin(&c);
  CHECK(strcmp(before, c.admin_password) == 0,
         "rehashing an already-hashed password is a no-op");
}

static void test_is_default_works_for_both_forms(void) {
  net_config_t c;
  netconfig_set_defaults(&c);
  CHECK(netconfig_admin_is_default(&c), "fresh defaults report default");
  netconfig_rehash_admin(&c);
  CHECK(netconfig_admin_is_default(&c),
         "still reports default once hashed -- strcmp could not do this");
  netconfig_set_admin(&c, "mine");
  CHECK(!netconfig_admin_is_default(&c), "a real password is not default");
}

static void test_malformed_hash_fails_closed(void) {
  /* The important one. A parser that gave up halfway and left a zeroed
     digest would accept whatever hashed to zero -- or worse, everything.
     Every one of these must reject every password. */
  static const char *bad[] = {
    "ADMIN=sha256:",
    "ADMIN=sha256:zzzzzzzzzzzzzzzz:0000000000000000000000000000000000000000000000000000000000000000",
    "ADMIN=sha256:00112233445566:0000000000000000000000000000000000000000000000000000000000000000",
    "ADMIN=sha256:0011223344556677:00",
    "ADMIN=sha256:0011223344556677;0000000000000000000000000000000000000000000000000000000000000000",
    "ADMIN=sha256:0011223344556677:000000000000000000000000000000000000000000000000000000000000000g",
  };
  for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
    net_config_t c;
    char line[200];
    snprintf(line, sizeof(line), "%s\n", bad[i]);
    parse(&c, line);
    int open = netconfig_admin_matches(&c, "") ||
               netconfig_admin_matches(&c, "admin") ||
               netconfig_admin_matches(&c, "anything at all");
    CHECK(!open, "malformed stored hash rejects every password");
  }
}

/* --- PFM_AUTOSTART ------------------- */

/* Read as the Primo's boot loader and PFM read it: ON in any case is on,
   anything else off, the last line counts; no line: -1, nothing written. */
static void test_pfm_autostart_parsed(void) {
  net_config_t c;
  parse(&c, "MODE=AP\r\n");
  CHECK(c.pfm_autostart == -1, "no PFM_AUTOSTART line: -1");
  parse(&c, "PFM_AUTOSTART=ON\r\n");
  CHECK(c.pfm_autostart == 1, "PFM_AUTOSTART=ON: on");
  parse(&c, "  pfm_autostart  \t= oN \r\n");
  CHECK(c.pfm_autostart == 1, "any case, spaces around");
  parse(&c, "PFM_AUTOSTART     =ON\r\n");
  CHECK(c.pfm_autostart == 1, "spaces before '=' past 16 bytes of key");
  parse(&c, "PFM_AUTOSTART=ONE\r\n");
  CHECK(c.pfm_autostart == 0, "ONE: off");
  parse(&c, "PFM_AUTOSTART=\r\n");
  CHECK(c.pfm_autostart == 0, "empty: off");
  parse(&c, "PFM_AUTOSTART=ON\r\nPFM_AUTOSTART=OFF\r\n");
  CHECK(c.pfm_autostart == 0, "the last line counts");
}

static void test_pfm_autostart_serialized(void) {
  net_config_t c;
  char buf[512];
  parse(&c, "SSID=x\r\n");
  netconfig_serialize(&c, buf, sizeof(buf));
  CHECK(strstr(buf, "PFM_AUTOSTART") == NULL, "no line in, no line out");
  parse(&c, "pfm_autostart = on\r\n");
  unsigned int len = netconfig_serialize(&c, buf, sizeof(buf));
  CHECK(len > 18 && strcmp(buf + len - 18, "PFM_AUTOSTART=ON\r\n") == 0, "ON written last");
  net_config_t back;
  parse(&back, buf);
  CHECK(back.pfm_autostart == 1, "ON round-trips");
}

/* The web page's save keeps the value the file holds when it is saved */
static void test_pfm_line_replaced(void) {
  net_config_t c;
  char buf[512];
  parse(&c, "PFM_AUTOSTART=OFF\r\n");                 /* as read at boot */
  unsigned int len = netconfig_serialize(&c, buf, sizeof(buf));
  char before[512];
  strcpy(before, buf);
  unsigned int n = netconfig_set_pfm_line(buf, len, sizeof(buf), 1);
  CHECK(n == len - 1, "OFF replaced by ON: one byte shorter");
  CHECK(strncmp(buf, before, len - 19) == 0, "the lines before it unchanged");
  CHECK(strcmp(buf + n - 18, "PFM_AUTOSTART=ON\r\n") == 0, "ON now");
  n = netconfig_set_pfm_line(buf, n, sizeof(buf), -1);
  CHECK(n == len - 19 && strcmp(buf, "") != 0 && strstr(buf, "PFM_AUTOSTART") == NULL, "removed for -1");
  unsigned int m = netconfig_set_pfm_line(buf, n, sizeof(buf), 0);
  CHECK(m == len && strcmp(buf, before) == 0, "added back: as serialized");
  CHECK(netconfig_set_pfm_line(buf, m, m + 1, 0) == m && netconfig_set_pfm_line(buf, m, m, 0) == 0,
        "exactly fits; one byte less: 0");
  /* a file without the line: the page's save adds the file's value */
  parse(&c, "SSID=x\r\n");
  len = netconfig_serialize(&c, buf, sizeof(buf));
  n = netconfig_set_pfm_line(buf, len, sizeof(buf), 1);
  CHECK(n == len + 18, "appended");
}

/* The web page's save changes only the keys the user changed: the rest
   keep the file's values at the moment of the save, which PFM may have
   changed over the bus since the boot (open step 10) */
static void test_save_applies_only_what_changed(void) {
  net_config_t boot, now, back;
  char buf[1024], file[1024], old_way[1024];
  parse(&boot, "MODE=STA\r\nSSID=boot-net\r\nPASSWORD=boot-pw\r\nCOUNTRY=HU\r\n");
  /* the file now: PFM changed the network and switched the autostart on */
  const char *disk = "MODE=STA\r\nSSID=pfm-net\r\nPASSWORD=pfm-pw\r\nCOUNTRY=HU\r\nPFM_AUTOSTART=ON\r\n";
  now = boot;
  strcpy(now.country, "DE");                          /* the user changed only this */
  unsigned int n = netconfig_diff(&boot, &now, buf, sizeof(buf));
  CHECK_STR(buf, "COUNTRY=DE\r\n", "the diff holds the changed key only");
  strcpy(file, disk);
  n = netconfig_merge(file, (unsigned int)strlen(file), buf, n, sizeof(buf));
  CHECK(n > 0, "merged");
  parse(&back, buf);
  CHECK_STR(back.ssid, "pfm-net", "SSID: the file's, not the boot's");
  CHECK_STR(back.password, "pfm-pw", "PASSWORD: the file's");
  CHECK_STR(back.country, "DE", "COUNTRY: the user's change");
  CHECK(back.pfm_autostart == 1, "PFM_AUTOSTART: the file's");
  /* negative control: the old save (everything from the boot copy, only
     the PFM line from the file) loses PFM's network */
  unsigned int m = netconfig_serialize(&now, old_way, sizeof(old_way));
  m = netconfig_set_pfm_line(old_way, m, sizeof(old_way), 1);
  parse(&back, old_way);
  CHECK(strcmp(back.ssid, "pfm-net") != 0, "control: the old save would have undone PFM's SSID");
  /* nothing changed: the file as it is */
  n = netconfig_diff(&boot, &boot, buf, sizeof(buf));
  CHECK(n == 0 && buf[0] == 0, "no change: an empty diff");
  strcpy(file, disk);
  n = netconfig_merge(file, (unsigned int)strlen(file), buf, 0, sizeof(buf));
  parse(&back, buf);
  CHECK_STR(back.ssid, "pfm-net", "an empty diff keeps the file");
  /* a whole file sent: that file */
  parse(&now, "MODE=AP\r\nSSID=x\r\nAP_SSID=y\r\nCOUNTRY=US\r\n");
  netconfig_serialize(&now, buf, sizeof(buf));
  strcpy(file, disk);
  n = netconfig_merge(file, (unsigned int)strlen(file), buf, (unsigned int)strlen(buf), sizeof(buf));
  parse(&back, buf);
  CHECK(back.ap_mode && strcmp(back.ssid, "x") == 0 && strcmp(back.ap_ssid, "y") == 0 &&
        strcmp(back.country, "US") == 0 && back.pfm_autostart == 1,
        "a whole file wins key by key; PFM_AUTOSTART it lacks stays");
  /* a new admin password is in the diff */
  now = boot;
  netconfig_set_admin(&now, "secret");
  netconfig_diff(&boot, &now, buf, sizeof(buf));
  CHECK(strncmp(buf, "ADMIN=", 6) == 0 && strchr(buf + 6, '\n') == buf + strlen(buf) - 1,
        "a changed admin password: one ADMIN line");
  CHECK(netconfig_diff(&boot, &now, buf, 8) == 0, "too small: 0");
}

int main(void) {
  test_defaults();
  test_empty_buffer();
  test_full_valid_file();
  test_case_insensitive_keys_and_mode_ap();
  test_crlf_line_endings();
  test_no_trailing_newline();
  test_whitespace_around_key_and_value();
  test_empty_value_does_not_blank_field();
  test_unrecognised_mode_value_is_ignored();
  test_unknown_key_ignored();
  test_value_containing_equals_sign();
  test_malformed_line_no_equals();
  test_overlong_value_truncated_safely();
  test_binary_garbage_content();
  test_embedded_nul_byte();
  test_mixed_line_endings_and_blank_lines();
  test_serialize_round_trip();
  test_serialize_ap_mode_round_trip();
  test_serialize_buffer_too_small();
  test_serialize_empty_password();
  test_ap_and_sta_ssids_are_independent();
  test_empty_admin_does_not_disable_auth();
  test_admin_password_can_be_changed();
  test_plaintext_admin_still_authenticates();
  test_hashed_admin_authenticates();
  test_salt_differs_between_devices();
  test_hashed_admin_survives_round_trip();
  test_rehash_migrates_plaintext();
  test_is_default_works_for_both_forms();
  test_malformed_hash_fails_closed();
  test_pfm_autostart_parsed();
  test_pfm_autostart_serialized();
  test_pfm_line_replaced();
  test_save_applies_only_what_changed();


  if (failures == 0) {
    printf("All netconfig tests passed.\n");
    return 0;
  } else {
    printf("%d netconfig test(s) FAILED.\n", failures);
    return 1;
  }
}
