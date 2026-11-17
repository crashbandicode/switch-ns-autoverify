/* Host tests for source/policy.c (no libnx). Build: make test (system cc). */
#include <stdio.h>
#include <string.h>

#include "../source/policy.h"

static int g_checks, g_failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

static void test_firmware(void) {
    CHECK(av_fw_supported(23, 5, 0));
    CHECK(av_fw_supported(23, 0, 0));
    CHECK(!av_fw_supported(22, 1, 0));
    CHECK(!av_fw_supported(24, 0, 0));
    CHECK(!av_fw_supported(0, 0, 0));
}

static void test_allowlist(void) {
    CHECK(av_title_allowed(0x01006A800016E000ULL));
    CHECK(!av_title_allowed(0x01006A800016E800ULL)); /* SSBU update id: not an application id */
    CHECK(!av_title_allowed(0x0100000000001000ULL)); /* qlaunch */
    CHECK(!av_title_allowed(0));
}

static void test_boot(void) {
    CHECK(av_boot_decision(false, false) == AV_BOOT_RUN);
    CHECK(av_boot_decision(true, false) == AV_BOOT_EXIT_DISABLED);
    CHECK(av_boot_decision(false, true) == AV_BOOT_EXIT_STALE_MARKER);
    CHECK(av_boot_decision(true, true) == AV_BOOT_EXIT_DISABLED);
}

static void test_verdict(void) {
    CHECK(av_verdict(true, 0, 0, false, false) == AV_V_PASS);
    /* Any non-zero completion result is a failure: nothing is cleared, the dialog stays. */
    CHECK(av_verdict(true, 0x1234, 0, false, false) == AV_V_FAIL);
    CHECK(av_verdict(true, 0, 0x5678, false, false) == AV_V_FAIL);
    CHECK(av_verdict(true, 0, 0, true, false) == AV_V_CANCELLED);
    CHECK(av_verdict(true, 0, 0, false, true) == AV_V_TIMEOUT);
    CHECK(av_verdict(true, 0, 0, true, true) == AV_V_TIMEOUT);
    CHECK(av_verdict(false, 0, 0, false, false) == AV_V_ERROR);
    CHECK(strcmp(av_verdict_name(AV_V_PASS), "pass") == 0);
    CHECK(strcmp(av_verdict_name(AV_V_FAIL), "fail") == 0);
}

static void test_clear(void) {
    CHECK(av_should_clear(AV_V_PASS, true, 0x4a8));
    CHECK(!av_should_clear(AV_V_PASS, false, 0x4a8)); /* operator has not enabled it */
    CHECK(!av_should_clear(AV_V_PASS, true, 0));      /* nothing to clear */
    CHECK(!av_should_clear(AV_V_FAIL, true, 0x4a8));  /* never after a failed verify */
    CHECK(!av_should_clear(AV_V_CANCELLED, true, 0x4a8));
    CHECK(!av_should_clear(AV_V_TIMEOUT, true, 0x4a8));
    CHECK(!av_should_clear(AV_V_ERROR, true, 0x4a8));
}

static void test_gate(void) {
    av_gate_in_t in = {.observe_only = false, .poisoned = false, .app_running = false, .verifies_done = 0};
    CHECK(av_gate(&in, AV_REASON_CRASH, AV_SSBU_PROGRAM_ID) == AV_GATE_GO);
    CHECK(av_gate(&in, AV_REASON_NONE, AV_SSBU_PROGRAM_ID) == AV_GATE_REFUSE);
    CHECK(av_gate(&in, AV_REASON_CRASH, 0x0100000000001000ULL) == AV_GATE_REFUSE);
    in.app_running = true;
    CHECK(av_gate(&in, AV_REASON_CRASH, AV_SSBU_PROGRAM_ID) == AV_GATE_DEFER);
    in.app_running = false;
    in.observe_only = true;
    CHECK(av_gate(&in, AV_REASON_TRIGGER_FILE, AV_SSBU_PROGRAM_ID) == AV_GATE_OBSERVE);
    in.observe_only = false;
    in.poisoned = true;
    CHECK(av_gate(&in, AV_REASON_CRASH, AV_SSBU_PROGRAM_ID) == AV_GATE_REFUSE);
    in.poisoned = false;
    in.verifies_done = AV_MAX_VERIFIES_PER_RUN;
    CHECK(av_gate(&in, AV_REASON_CRASH, AV_SSBU_PROGRAM_ID) == AV_GATE_REFUSE);
    in.verifies_done = AV_MAX_VERIFIES_PER_RUN - 1;
    CHECK(av_gate(&in, AV_REASON_TERMINATE_RESULT, AV_SSBU_PROGRAM_ID) == AV_GATE_GO);
    /* Refusal outranks deferral: a non-allowlisted title is never queued. */
    in.app_running = true;
    CHECK(av_gate(&in, AV_REASON_CRASH, 0x0100000000001000ULL) == AV_GATE_REFUSE);
}

static void test_tracker(void) {
    av_tracker_t t;
    uint64_t id = 0;
    av_track_init(&t);
    CHECK(!av_track_due(&t, 100000, &id));

    /* Normal exit: never due. */
    av_track_app_start(&t, 77, AV_SSBU_PROGRAM_ID);
    av_track_exit(&t, 77, 1000);
    CHECK(!av_track_due(&t, 1000 + AV_SETTLE_MS + 1, &id));

    /* Crash of another pid is ignored. */
    av_track_app_start(&t, 80, AV_SSBU_PROGRAM_ID);
    av_track_crash(&t, 81);
    av_track_exit(&t, 80, 2000);
    CHECK(!av_track_due(&t, 2000 + AV_SETTLE_MS + 1, &id));

    /* Crash then exit: due only after the settle time. */
    av_track_app_start(&t, 90, AV_SSBU_PROGRAM_ID);
    av_track_crash(&t, 90);
    CHECK(!av_track_due(&t, 5000, &id)); /* not exited yet */
    av_track_exit(&t, 90, 5000);
    CHECK(!av_track_due(&t, 5000 + AV_SETTLE_MS - 1, &id));
    CHECK(av_track_due(&t, 5000 + AV_SETTLE_MS, &id));
    CHECK(id == AV_SSBU_PROGRAM_ID);

    /* A relaunch postpones it; the next exit makes it due again (same flag episode). */
    av_track_app_start(&t, 91, AV_SSBU_PROGRAM_ID);
    CHECK(!av_track_due(&t, 100000, &id));
    av_track_exit(&t, 91, 200000);
    CHECK(!av_track_due(&t, 200000, &id)); /* the settle time restarts from the latest exit */
    CHECK(av_track_due(&t, 200000 + AV_SETTLE_MS, &id));

    av_track_consume(&t);
    CHECK(!av_track_due(&t, 300000, &id));

    /* A clock that goes backwards never makes it due. */
    av_track_app_start(&t, 92, AV_SSBU_PROGRAM_ID);
    av_track_crash(&t, 92);
    av_track_exit(&t, 92, 50000);
    CHECK(!av_track_due(&t, 40000, &id));
}

static void test_trigger_parse(void) {
    CHECK(av_parse_trigger_id(NULL) == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("\n") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("deferred\n") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("01006A800016E000") == 0x01006A800016E000ULL);
    CHECK(av_parse_trigger_id("0x0100abcdef012000\n") == 0x0100ABCDEF012000ULL);
    CHECK(av_parse_trigger_id("  0100ABCDEF012000\r\n") == 0x0100ABCDEF012000ULL);
    CHECK(av_parse_trigger_id("0100ABCDEF01200") == AV_SSBU_PROGRAM_ID);   /* 15 digits */
    CHECK(av_parse_trigger_id("0100ABCDEF0120001") == AV_SSBU_PROGRAM_ID); /* 17 digits */
    CHECK(av_parse_trigger_id("0100ABCDEF01200G") == AV_SSBU_PROGRAM_ID);
    /* A parsed id is still refused unless allowlisted. */
    av_gate_in_t in = {.observe_only = false, .poisoned = false, .app_running = false, .verifies_done = 0};
    CHECK(av_gate(&in, AV_REASON_TRIGGER_FILE, av_parse_trigger_id("0100ABCDEF012000")) == AV_GATE_REFUSE);
}

static void test_progress_and_log(void) {
    CHECK(av_progress_permille(0, 0) == -1);
    CHECK(av_progress_permille(5, -1) == -1);
    CHECK(av_progress_permille(-1, 10) == -1);
    CHECK(av_progress_permille(0, 100) == 0);
    CHECK(av_progress_permille(50, 100) == 500);
    CHECK(av_progress_permille(150, 100) == 1000);
    CHECK(av_progress_permille(16LL * 1024 * 1024 * 1024, 32LL * 1024 * 1024 * 1024) == 500);
    CHECK(!av_log_needs_rotate(0));
    CHECK(!av_log_needs_rotate(AV_LOG_CAP_BYTES - 1));
    CHECK(av_log_needs_rotate(AV_LOG_CAP_BYTES));
}

static void test_constants(void) {
    CHECK(AV_VERIFY_FLAGS == 0x7u);
    CHECK(AV_VERIFY_TMEM_SIZE == 0x100000u);
    CHECK(AV_VERIFY_TMEM_SIZE <= AV_HEAP_GRANULE);
    CHECK((AV_VERIFY_TMEM_SIZE & 0xFFFu) == 0);
    CHECK(AV_POLL_MS >= 1000u); /* no busy loop */
}

int main(void) {
    test_firmware();
    test_allowlist();
    test_boot();
    test_verdict();
    test_clear();
    test_gate();
    test_tracker();
    test_trigger_parse();
    test_progress_and_log();
    test_constants();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
