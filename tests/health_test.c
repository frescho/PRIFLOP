/* health_test.c: host-side unit tests for src/pico/health.c -- the observer
   that core 0 runs to say, in the boot log, whether core 1 is alive and the
   Wi-Fi link is carrying traffic. See health.h for why it exists.

     gcc -I ../src/pico -o health_test health_test.c ../src/pico/health.c
     ./health_test

   The scenarios are the ones the 2026-09-20 wedge could have been (core 1
   stuck; link "up" but silent; link really down) and the ones that must NOT
   raise an alarm (a slow rejoin, a quiet start, AP mode, the clock
   wrapping, core 1 stamping a frame a moment after core 0 read the clock).
*/
#include <stdio.h>

#include "health.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
      printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++; \
    } \
  } while (0)

#define S 1000u

typedef struct {
  health_watch_t w;
  health_snapshot_t s;
  uint32_t now;
  uint32_t detail;
} rig_t;

static void rig_init(rig_t *r, int32_t link) {
  *r = (rig_t){0};
  r->s.wifi_link_status = link;
  r->now = 100 * S;
  r->s.core1_loops = 5;
  (void)health_watch_step(&r->w, &r->s, r->now, &r->detail);   /* the first call only records */
}

/* Advance the clock `secs` seconds, one call a second, optionally making
   core 1 progress and the radio deliver frames. Returns the first event
   seen (and stops there), or HEALTH_NONE. */
static health_event_t run(rig_t *r, unsigned secs, int core1_runs, int frames_arrive) {
  for (unsigned i = 0; i < secs; i++) {
    r->now += S;
    if (core1_runs)
      r->s.core1_loops++;
    if (frames_arrive)
      r->s.wifi_rx_frames++;
    health_event_t e = health_watch_step(&r->w, &r->s, r->now, &r->detail);
    if (e != HEALTH_NONE)
      return e;
  }
  return HEALTH_NONE;
}

static void test_a_healthy_device_says_nothing(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 3600, 1, 1) == HEALTH_NONE, "an hour of progress and traffic is silent");
}

static void test_core1_stall_is_reported_once_with_its_duration(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 44, 0, 1) == HEALTH_NONE, "44 s without a pass is still a slow rejoin, not a stall");
  CHECK(run(&r, 2, 0, 1) == HEALTH_CORE1_STALLED, "45 s without a pass is a stall");
  CHECK(r.detail >= 45 && r.detail <= 46, "the report says how long");
  CHECK(run(&r, 600, 0, 1) == HEALTH_NONE, "a persisting stall is not repeated every second");
}

static void test_a_persisting_stall_is_repeated_rarely(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 60, 0, 0) == HEALTH_CORE1_STALLED, "first report");
  int repeats = 0;
  for (int i = 0; i < 4 * 3600; i++) {                        /* four hours */
    r.now += S;
    if (health_watch_step(&r.w, &r.s, r.now, &r.detail) == HEALTH_CORE1_STALLED)
      repeats++;
  }
  CHECK(repeats == 8, "repeated every 30 minutes, so a wedged device fills the log slowly");
}

static void test_recovery_is_reported(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 60, 0, 0) == HEALTH_CORE1_STALLED, "stall");
  run(&r, 20, 0, 0);
  r.s.core1_loops++;
  r.now += S;
  CHECK(health_watch_step(&r.w, &r.s, r.now, &r.detail) == HEALTH_CORE1_RESUMED, "progress after a stall is reported");
  CHECK(r.detail >= 60, "with how long it was stalled");
}

static void test_the_time_core1_was_stalled_is_not_blamed_on_the_link(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 60, 0, 0) == HEALTH_CORE1_STALLED, "stall");
  CHECK(run(&r, 150, 0, 0) == HEALTH_NONE, "a long stall, no traffic delivered because nothing was polling");
  r.s.core1_loops++;
  r.now += S;
  CHECK(health_watch_step(&r.w, &r.s, r.now, &r.detail) == HEALTH_CORE1_RESUMED, "resumed");
  CHECK(run(&r, 30, 1, 0) == HEALTH_NONE, "the stalled minutes are not counted as a silent link");
}

static void test_link_up_but_silent_is_reported(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 179, 1, 0) == HEALTH_NONE, "under three minutes of quiet is normal");
  CHECK(run(&r, 2, 1, 0) == HEALTH_WIFI_SILENT, "three minutes of nothing while UP is not");
  CHECK(r.detail >= 180 && r.detail <= 181, "the report says how long");
  CHECK(run(&r, 900, 1, 0) == HEALTH_NONE, "not repeated every second");
  r.s.wifi_rx_frames++;
  r.now += S;
  CHECK(health_watch_step(&r.w, &r.s, r.now, &r.detail) == HEALTH_WIFI_HEARD, "traffic returning is reported");
}

static void test_a_silent_link_that_is_down_is_the_rejoin_logics_business(void) {
  rig_t r; rig_init(&r, 0);                                    /* CYW43_LINK_DOWN */
  CHECK(run(&r, 3600, 1, 0) == HEALTH_NONE, "no silence alarm while the link is not UP");
}

static void test_ap_mode_and_unknown_are_never_reported(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UNKNOWN);
  CHECK(run(&r, 3600, 1, 0) == HEALTH_NONE, "an access point has no upstream to be silent");
}

static void test_silence_only_counts_time_spent_up(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 150, 1, 0) == HEALTH_NONE, "150 s quiet while up");
  r.s.wifi_link_status = 0;
  CHECK(run(&r, 300, 1, 0) == HEALTH_NONE, "then the link drops for five minutes");
  r.s.wifi_link_status = HEALTH_LINK_UP;
  CHECK(run(&r, 150, 1, 0) == HEALTH_NONE, "the time down does not count, and the clock restarted");
  CHECK(run(&r, 60, 1, 0) == HEALTH_WIFI_SILENT, "but a fresh three quiet minutes do");
}

static void test_a_slow_boot_join_is_not_a_stall(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UNKNOWN);
  r.s.core1_loops = 0;
  r.w.seen_loops = 0;
  CHECK(run(&r, 299, 0, 0) == HEALTH_NONE, "core 1 has not reached its loop after 299 s: still joining");
  CHECK(run(&r, 3, 0, 0) == HEALTH_CORE1_STALLED, "after five minutes it has not");
}

static void test_the_clock_wrapping_is_harmless(void) {
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  r.now = 0xFFFFFFFFu - 100 * S;                               /* 49 days in: about to wrap */
  r.w.seen_loops_at_ms = r.now;
  r.w.frames_changed_at_ms = r.now;
  CHECK(run(&r, 3600, 1, 1) == HEALTH_NONE, "no alarm across the wrap while all is well");
  CHECK(run(&r, 60, 0, 1) == HEALTH_CORE1_STALLED, "and a stall across the wrap is still seen");
}

static void test_a_frame_stamped_just_after_the_clock_was_read_cannot_alarm(void) {
  /* The observer never compares its clock with a timestamp written by core 1,
     so a frame stamped a few milliseconds "in the future" cannot look like a
     fifty-day silence. Prove the counters alone drive it. */
  rig_t r; rig_init(&r, HEALTH_LINK_UP);
  CHECK(run(&r, 600, 1, 1) == HEALTH_NONE, "traffic and progress");
  health.wifi_rx_last_ms = r.now + 5;                          /* what core 1 might have written */
  CHECK(run(&r, 10, 1, 1) == HEALTH_NONE, "no alarm");
}

/* The firmware's clock, as health_poll() reads it: to_ms_since_boot() of a
   64-bit microsecond counter, truncated to 32 bits of milliseconds. */
static uint32_t firmware_ms(uint64_t us) {
  return (uint32_t)(us / 1000u);
}

/* Drive the gate and the observer the way health_poll() does, calling it every
   10 ms of the firmware clock from `start_us` for `hours`, with core 1 making
   progress until `stall_after_h` hours (never, if negative). Returns how often
   the gate opened; *stalls counts the stall reports. */
static unsigned long run_poll(uint64_t start_us, unsigned hours, int stall_after_h, int *stalls) {
  health_watch_t w = {0};
  health_snapshot_t s = { .core1_loops = 1, .wifi_link_status = HEALTH_LINK_UNKNOWN };
  uint32_t next_ms = 0, detail;
  unsigned long due = 0;
  *stalls = 0;
  for (uint64_t us = start_us; us < start_us + (uint64_t)hours * 3600000000u; us += 10000u) {
    if (!health_tick_due(&next_ms, firmware_ms(us)))
      continue;
    due++;
    if (stall_after_h < 0 || us - start_us < (uint64_t)stall_after_h * 3600000000u)
      s.core1_loops++;
    if (health_watch_step(&w, &s, firmware_ms(us), &detail) == HEALTH_CORE1_STALLED)
      (*stalls)++;
  }
  return due;
}

static void test_the_gate_keeps_opening_long_after_72_minutes(void) {
  /* 2026-09-30: with the clock at time_us_32() / 1000 the gate opened for the
     first 71.6 minutes and never again. Six hours must give one tick a second. */
  int stalls;
  unsigned long due = run_poll(0, 6, -1, &stalls);
  CHECK(due >= 6 * 3600 - 1 && due <= 6 * 3600 + 1, "one tick a second for six hours");
  CHECK(stalls == 0, "and no alarm while core 1 runs");
}

static void test_a_stall_hours_after_boot_is_still_reported(void) {
  int stalls;
  run_poll(0, 4, 3, &stalls);                                  /* core 1 stops at 3 h */
  CHECK(stalls == 2, "a stall three hours in -- well past the old 72-minute limit -- is reported, "
                     "and repeated once in its hour");
}

static void test_the_gate_survives_the_49_day_wrap(void) {
  int stalls;
  uint64_t us = (uint64_t)0xFFFFFFFFu * 1000u - 1800000000u;  /* 30 minutes before the ms wrap */
  unsigned long due = run_poll(us, 1, -1, &stalls);
  CHECK(due >= 3599 && due <= 3601, "ticks keep coming across the wrap");
  CHECK(stalls == 0, "and it is not mistaken for a stall");
}

static void test_the_shared_counters_start_in_a_known_state(void) {
  CHECK(health.wifi_link_status == HEALTH_LINK_UNKNOWN, "link status starts unknown");
  CHECK(health.core1_loops == 0 && health.fsb_wait_max_ms == 0, "counters start at zero");
}

int main(void) {
  test_the_shared_counters_start_in_a_known_state();
  test_a_healthy_device_says_nothing();
  test_core1_stall_is_reported_once_with_its_duration();
  test_a_persisting_stall_is_repeated_rarely();
  test_recovery_is_reported();
  test_the_time_core1_was_stalled_is_not_blamed_on_the_link();
  test_link_up_but_silent_is_reported();
  test_a_silent_link_that_is_down_is_the_rejoin_logics_business();
  test_ap_mode_and_unknown_are_never_reported();
  test_silence_only_counts_time_spent_up();
  test_a_slow_boot_join_is_not_a_stall();
  test_the_clock_wrapping_is_harmless();
  test_a_frame_stamped_just_after_the_clock_was_read_cannot_alarm();
  test_the_gate_keeps_opening_long_after_72_minutes();
  test_a_stall_hours_after_boot_is_still_reported();
  test_the_gate_survives_the_49_day_wrap();

  if (failures == 0) {
    printf("All health tests passed.\n");
    return 0;
  }
  printf("%d health test(s) FAILED.\n", failures);
  return 1;
}
