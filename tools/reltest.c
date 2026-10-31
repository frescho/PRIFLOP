/* Drive reliability test -- sd2iec_pico2w
 * C rewrite of tools/reltest.bas, compiled with cc65 (cl65 -t c64).
 *
 * Why C instead of BASIC: the BASIC version needed a hand-rolled trick to
 * get SAVE to write a whole memory block at once (redirecting BASIC's own
 * TXTTAB/VARTAB pointers), which broke variable lookup for anything
 * referenced afterward -- a real, subtle bug that cost a long debugging
 * session. A compiled C program has no BASIC interpreter state to
 * protect: cbm_save()/cbm_load() are direct KERNAL wrappers with no side
 * effects on program state, so the whole workaround is unnecessary. The
 * buffer fill also moves from an interpreted BASIC loop (minutes) to
 * compiled code (effectively instant).
 */
#include <cbm.h>
#include <c64.h>
#include <conio.h>
#include <peekpoke.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Configuration, changeable via the menu ---- */
static unsigned char device = 9;          /* F1: 8 or 9 */
static unsigned char size_mode = 1;       /* F3: 0=8K 1=16K 2=24K 3=RND */
static unsigned char num_files = 3;       /* F5: 1..5 */
static int repeat_count = 1;              /* F7: 1, 3, 5, or -1 = infinite */

static const unsigned int size_table[3] = { 8192, 16384, 24576 };
static const char *size_label[4] = { "8K", "16K", "24K", "RND" };

/* One 24K buffer, reused for both directions: filled with the pattern
   and saved, then overwritten by the read-back. Since the pattern is
   the deterministic byte(i)=i formula, verifying the read-back needs
   no second reference buffer -- just compare the (now-overwritten)
   buffer against the same formula. Two full-size buffers overflowed
   the default linker config's BSS area by several KB; one comfortably
   fits with room to spare. */
static unsigned char buf[24576];

/* One 256-byte page holding the pattern once. cc65 has no fast path for
   a 16-bit array index or a 16-bit pointer increment -- both compile to
   a full address recompute plus a JSR to a runtime add/increment helper
   on every iteration (confirmed by inspecting the -O output). An 8-bit
   index does get the native INC/BNE loop, so the pattern is built once
   per file into this page with an 8-bit loop, then replicated across
   the real buffer with memcpy() in 256-byte chunks -- cutting the
   expensive 16-bit address math from one execution per byte to one per
   page. */
static unsigned char page0[256];

static unsigned long fail_count = 0;

/* Non-blocking: returns 1 if RUN/STOP has been pressed since the last
   check. RUN/STOP (pressed alone, no shift) pushes PETSCII 3 into the
   normal keyboard buffer exactly like any other key -- the same
   mechanism BASIC's own "GET A$: IF A$=CHR$(3)" idiom relies on. Using
   that instead of a hand-rolled call to the KERNAL STOP vector avoids
   inline assembly and its own register-convention risk. */
static unsigned char stop_pressed(void) {
    return (cbm_k_getin() == 3);
}

/* ---- Screen helpers ---- */
static void border(unsigned char c) {
    POKE(0xD020, c);
}

/* Busy-waits for the given number of jiffies (1/60s each on the C64's
   KERNAL clock, regardless of PAL/NTSC). */
static void wait_jiffies(unsigned char n) {
    clock_t start = clock();
    while ((clock_t)(clock() - start) < n) {}
}

static void log_line(const char *s, unsigned char is_fail) {
    if (is_fail) {
        textcolor(COLOR_RED);
        cprintf("*** %s\r\n", s);
        textcolor(COLOR_WHITE);
    } else {
        cprintf("%s\r\n", s);
    }
}

/* ---- Menu ---- */
static void draw_menu(void) {
    clrscr();
    cprintf("DRIVE RELIABILITY TEST (C)\r\n\r\n");
    cprintf("F1 DRIVE ...... %u\r\n", device);
    cprintf("F3 FILE SIZE .. %s\r\n", size_label[size_mode]);
    cprintf("F5 NUM FILES .. %u\r\n", num_files);
    if (repeat_count == -1)
        cprintf("F7 REPEAT ..... INFINITE\r\n");
    else
        cprintf("F7 REPEAT ..... %d\r\n", repeat_count);
    cprintf("\r\nRETURN TO START, RUN/STOP TO ABORT\r\n");
}

static void run_menu(void) {
    unsigned char k;
    draw_menu();
    for (;;) {
        k = cbm_k_getin();
        if (k == 0) continue;
        if (k == 133) { device = (device == 8) ? 9 : 8; draw_menu(); continue; }
        if (k == 134) { size_mode = (size_mode + 1) & 3; draw_menu(); continue; }
        if (k == 135) { num_files++; if (num_files > 5) num_files = 1; draw_menu(); continue; }
        if (k == 136) {
            if (repeat_count == 1) repeat_count = 3;
            else if (repeat_count == 3) repeat_count = 5;
            else if (repeat_count == 5) repeat_count = -1;
            else repeat_count = 1;
            draw_menu();
            continue;
        }
        if (k == 13) return;
    }
}

/* Deletes a file via the command channel's SCRATCH command. Ignoring
   the result is deliberate -- the common case is "nothing to delete
   yet", which isn't a real error. */
static void delete_file(const char *name) {
    char cmd[24];
    sprintf(cmd, "s0:%s", name);
    cbm_open(15, device, 15, cmd);
    cbm_close(15);
}

/* Fills page0 with the pattern via an 8-bit loop, then replicates it
   across buf[0..fs) in 256-byte chunks via memcpy(). */
static void fill_pattern(unsigned int fs) {
    unsigned char v = 0;
    unsigned int off;

    do {
        page0[v] = v;
        v++;
    } while (v != 0);

    for (off = 0; off < fs; off += 256) {
        unsigned int chunk = (fs - off < 256) ? (fs - off) : 256;
        memcpy(buf + off, page0, chunk);
    }
}

/* ---- One file: fill / save / load / compare ---- */
/* Returns 1 on pass, 0 on fail. Fills `msg` with a status line either way. */
static unsigned char test_one_file(const char *name, unsigned int fs, char *msg) {
    unsigned int i;
    unsigned char rc;
    unsigned int mismatch_at = 0;
    unsigned char found_mismatch;
    unsigned int loaded_len;

    delete_file(name);

    fill_pattern(fs);

    border(2); /* red = writing */
    cprintf("%s: writing...\r\n", name);
    wait_jiffies(3); /* ~50ms -- give the screen a moment to actually
        render this line before the KERNAL's serial-bus transfer starts;
        real hardware can leave the display looking frozen for the whole
        transfer, so anything printed only after cbm_save()/cbm_load()
        return isn't visible until the round finishes. */
    rc = cbm_save(name, device, buf, fs);
    if (rc != 0) {
        sprintf(msg, "%s WRITE FAIL, ERR=%u", name, rc);
        return 0;
    }

    border(5); /* green = reading -- overwrites buf with the read-back */
    cprintf("%s: reading...\r\n", name);
    wait_jiffies(3);
    memset(buf, 0, fs);
    loaded_len = cbm_load(name, device, buf);
    if (loaded_len != fs) {
        sprintf(msg, "%s SHORT READ %u/%u", name, loaded_len, fs);
        return 0;
    }

    border(7); /* yellow = comparing against page0, not a second full-size buffer */
    found_mismatch = 0;
    for (i = 0; i < fs; i += 256) {
        unsigned int chunk = (fs - i < 256) ? (fs - i) : 256;
        if (memcmp(buf + i, page0, chunk) != 0) {
            unsigned int j;
            for (j = 0; j < chunk; j++) {
                if (buf[i + j] != page0[j]) {
                    found_mismatch = 1;
                    mismatch_at = i + j;
                    break;
                }
            }
            break;
        }
    }
    if (found_mismatch) {
        sprintf(msg, "%s MISMATCH AT %u EXP=%u GOT=%u",
                name, mismatch_at, (unsigned char)mismatch_at, buf[mismatch_at]);
        return 0;
    }

    sprintf(msg, "%s %uB OK", name, fs);
    return 1;
}

/* ---- Main test loop ---- */
static void run_test(void) {
    int round = 0;
    unsigned char fi;
    unsigned int fs;
    char name[8];
    char msg[80];
    unsigned char ok_count;
    unsigned char pass;
    unsigned char aborted = 0;

    clrscr();
    fail_count = 0;

    for (;;) {
        round++;
        if (repeat_count != -1 && round > repeat_count) break;
        if (stop_pressed()) { aborted = 1; break; }

        ok_count = 0;
        for (fi = 1; fi <= num_files; fi++) {
            sprintf(name, "RT%u", fi);
            if (size_mode == 3)
                fs = size_table[(unsigned char)(rand() % 3)];
            else
                fs = size_table[size_mode];

            pass = test_one_file(name, fs, msg);
            if (pass) ok_count++; else fail_count++;
            log_line(msg, !pass);

            if (stop_pressed()) { aborted = 1; break; }
        }

        {
            char rmsg[40];
            sprintf(rmsg, "ROUND %d: %u/%u OK, TOTAL FAIL %lu",
                    round, ok_count, num_files, fail_count);
            log_line(rmsg, ok_count < num_files);
        }

        for (fi = 1; fi <= num_files; fi++) {
            sprintf(name, "RT%u", fi);
            delete_file(name);
        }

        if (aborted) break;
    }

    border(14);
    {
        char fmsg[40];
        if (aborted)
            sprintf(fmsg, "ABORTED. FAILURES: %lu", fail_count);
        else
            sprintf(fmsg, "TEST COMPLETE. FAILURES: %lu", fail_count);
        log_line(fmsg, fail_count > 0);
    }
}

int main(void) {
    for (;;) {
        run_menu();
        run_test();
        cprintf("\r\nPRESS RETURN FOR MENU\r\n");
        while (cbm_k_getin() != 13) {}
    }
    return 0;
}
