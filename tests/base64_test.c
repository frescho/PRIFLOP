/* base64_test.c: host-side unit tests for src/pico/base64.c, used by
   httpd.c's HTTP Basic Auth. Build and run with a native compiler:

     gcc -I ../src/pico base64_test.c ../src/pico/base64.c -o base64_test
     ./base64_test
*/
#include <stdio.h>
#include <string.h>

#include "base64.h"

static int failures = 0;

#define CHECK_STR(actual, expected, msg) do { \
    if (strcmp((actual), (expected)) != 0) { \
      printf("FAIL: %s -- got \"%s\", expected \"%s\" (line %d)\n", \
             msg, (actual), (expected), __LINE__); \
      failures++; \
    } \
  } while (0)

static void test_known_vectors(void) {
  char out[64];
  /* "user:pass" -> base64, the canonical HTTP Basic Auth shape. */
  base64_decode("dXNlcjpwYXNz", out, sizeof(out));
  CHECK_STR(out, "user:pass", "known vector: user:pass");

  base64_decode("", out, sizeof(out));
  CHECK_STR(out, "", "known vector: empty input");

  /* RFC 4648 test vectors. */
  base64_decode("Zg==", out, sizeof(out));
  CHECK_STR(out, "f", "RFC 4648: f");
  base64_decode("Zm8=", out, sizeof(out));
  CHECK_STR(out, "fo", "RFC 4648: fo");
  base64_decode("Zm9v", out, sizeof(out));
  CHECK_STR(out, "foo", "RFC 4648: foo");
  base64_decode("Zm9vYg==", out, sizeof(out));
  CHECK_STR(out, "foob", "RFC 4648: foob");
  base64_decode("Zm9vYmE=", out, sizeof(out));
  CHECK_STR(out, "fooba", "RFC 4648: fooba");
  base64_decode("Zm9vYmFy", out, sizeof(out));
  CHECK_STR(out, "foobar", "RFC 4648: foobar");
}

static void test_malformed_input_is_safe(void) {
  char out[64];
  /* Not valid base64 at all -- must not crash, output just ends up
     empty or a harmless partial decode, whichever falls out of
     stopping at the first invalid character. */
  base64_decode("!!!not base64!!!", out, sizeof(out));
  /* No crash is the real assertion; also check it didn't overrun by
     confirming the string is still properly terminated somewhere
     within bounds (strlen() would hang/read OOB on missing NUL). */
  if (strlen(out) >= sizeof(out)) {
    printf("FAIL: malformed input produced an unterminated string\n");
    failures++;
  }

  base64_decode("Zm9v!!!!garbage", out, sizeof(out));
  CHECK_STR(out, "foo", "malformed: valid prefix decoded, garbage suffix stopped cleanly");
}

static void test_output_buffer_never_overruns(void) {
  /* A long, entirely valid base64 string decoded into a deliberately
     tiny buffer must truncate, not overrun -- this is the actual
     security property that matters (attacker controls `in` via an
     HTTP header; if `out` could overrun, this would be a stack
     buffer overflow in check_auth()). */
  char tiny[5]; /* room for 4 decoded bytes + NUL */
  base64_decode("Zm9vYmFyYmF6cXV1eA==", tiny, sizeof(tiny)); /* "foobarbazquux" */
  if (strlen(tiny) >= sizeof(tiny)) {
    printf("FAIL: output buffer overrun with a tiny destination\n");
    failures++;
  }

  char zero_sized[1];
  base64_decode("Zm9v", zero_sized, 0); /* outsize=0: must not touch the buffer at all */
  /* Nothing to assert on zero_sized's content (untouched by contract);
     the real check is that this didn't crash. */
  (void)zero_sized;

  char one_byte[1];
  base64_decode("Zm9v", one_byte, sizeof(one_byte));
  CHECK_STR(one_byte, "", "outsize=1: only room for the NUL terminator");
}

static void test_no_username_colon(void) {
  char out[64];
  /* A value with no ':' at all -- check_auth() in httpd.c treats the
     whole decoded string as the password in this case; just confirm
     decode itself is still correct and safe here. */
  base64_decode("cGFzc3dvcmQ=", out, sizeof(out)); /* "password", no colon */
  CHECK_STR(out, "password", "no colon: decodes cleanly regardless");
}

int main(void) {
  test_known_vectors();
  test_malformed_input_is_safe();
  test_output_buffer_never_overruns();
  test_no_username_colon();

  if (failures == 0) {
    printf("All base64 tests passed.\n");
    return 0;
  } else {
    printf("%d base64 test(s) FAILED.\n", failures);
    return 1;
  }
}
