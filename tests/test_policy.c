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

static void test_firmware_allowlist_boot(void) {
    CHECK(av_fw_supported(23, 5, 0));
    CHECK(av_fw_supported(23, 0, 0));
    CHECK(!av_fw_supported(22, 1, 0));
    CHECK(!av_fw_supported(24, 0, 0));
    CHECK(av_title_allowed(0x01006A800016E000ULL));
    CHECK(!av_title_allowed(0x01006A800016E800ULL));
    CHECK(!av_title_allowed(0x0100000000001000ULL));
    CHECK(!av_title_allowed(0));
    CHECK(av_boot_decision(false, false) == AV_BOOT_RUN);
    CHECK(av_boot_decision(true, false) == AV_BOOT_EXIT_DISABLED);
    CHECK(av_boot_decision(false, true) == AV_BOOT_EXIT_STALE_MARKER);
    CHECK(av_boot_decision(true, true) == AV_BOOT_EXIT_DISABLED);
}

/* A crash state that ns positively classifies as not data corruption, raw result also benign. */
static av_clear_in_t benign(void) {
    av_clear_in_t in;
    memset(&in, 0, sizeof(in));
    in.term_ok = true;
    in.term = 0x4A8;
    in.term_class_ok = true;
    in.term_corrupt = false;
    in.raw_ok = true;
    in.raw = 0x4A8;
    in.raw_class_ok = true;
    in.raw_corrupt = false;
    return in;
}

static void test_clear_gate(void) {
    const uint64_t ssbu = AV_SSBU_PROGRAM_ID;
    av_clear_in_t in = benign();
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_GO);

    /* Only the allowlisted title. */
    CHECK(av_clear_gate(&in, 0x0100000000001000ULL) == AV_CLR_REFUSE);

    /* Nothing to clear only when both results are 0. */
    in = benign();
    in.term = 0;
    in.raw = 0;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_NOTHING);

    /* Hardware 2026-11-17: SetApplicationTerminateResult(0x4A8) left 2100 at 0 and set only the raw 2101 value.
     * A raw-only state is acted on, still gated by ns's classification of the raw value. */
    in = benign();
    in.term = 0;
    in.term_class_ok = false; /* a zero term needs no classification */
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_GO);
    in.raw_corrupt = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);
    in.raw_corrupt = false;
    in.raw_class_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    /* An unreadable raw result is unknown even when term is 0. */
    in = benign();
    in.term = 0;
    in.raw_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);

    /* ns says data corruption: keep the dialog (terminate result or raw result). */
    in = benign();
    in.term_corrupt = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);
    in = benign();
    in.raw = 0x1F4002;
    in.raw_corrupt = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);

    /* Fail closed on any failed query. */
    in = benign();
    in.term_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    in = benign();
    in.term_class_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    in = benign();
    in.raw_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    in = benign();
    in.raw_class_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    /* ...but a zero raw result needs no classification. */
    in = benign();
    in.raw = 0;
    in.raw_class_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_GO);

    /* Corruption outranks observe-only; observe-only outranks clearing. */
    in = benign();
    in.observe_only = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_OBSERVE);
    in.term_corrupt = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);

    /* A running application defers; refusal outranks deferral. */
    in = benign();
    in.app_running = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_DEFER);
    CHECK(av_clear_gate(&in, 0x0100000000001000ULL) == AV_CLR_REFUSE);

    /* Per-run limit and no retry of a value whose clear already failed. */
    in = benign();
    in.clears_done = AV_MAX_CLEARS_PER_RUN;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_REFUSE);
    in = benign();
    in.failed_key = av_clear_key(0x4A8, 0x4A8);
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_REFUSE);
    in.term = 0x4A9;
    in.raw = 0x4A9;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_GO);
    CHECK(av_clear_key(0, 0) == 0);
    CHECK(av_clear_key(0, 0x4A8) != av_clear_key(0x4A8, 0));
    CHECK(av_clear_key(0x4A8, 0x4A8) == 0x000004A8000004A8ULL);

    CHECK(strcmp(av_clear_name(AV_CLR_GO), "clear") == 0);
    CHECK(strcmp(av_clear_name(AV_CLR_CORRUPT), "corrupt-keep") == 0);
    CHECK(strcmp(av_clear_name(AV_CLR_UNKNOWN), "unknown-keep") == 0);
}

static void test_verdict(void) {
    CHECK(av_verdict(true, 0, 0, false, false) == AV_V_PASS);
    CHECK(av_verdict(true, 0x1234, 0, false, false) == AV_V_FAIL);
    CHECK(av_verdict(true, 0, 0x5678, false, false) == AV_V_FAIL);
    CHECK(av_verdict(true, 0, 0, true, false) == AV_V_CANCELLED);
    CHECK(av_verdict(true, 0, 0, false, true) == AV_V_TIMEOUT);
    CHECK(av_verdict(false, 0, 0, false, false) == AV_V_ERROR);
    CHECK(strcmp(av_verdict_name(AV_V_PASS), "pass") == 0);
}

static void test_trigger_parse(void) {
    CHECK(av_parse_trigger_id(NULL) == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("deferred\n") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("01006A800016E000") == 0x01006A800016E000ULL);
    CHECK(av_parse_trigger_id("0x0100abcdef012000\n") == 0x0100ABCDEF012000ULL);
    CHECK(av_parse_trigger_id("  0100ABCDEF012000\r\n") == 0x0100ABCDEF012000ULL);
    CHECK(av_parse_trigger_id("0100ABCDEF01200") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("0100ABCDEF0120001") == AV_SSBU_PROGRAM_ID);
    CHECK(av_parse_trigger_id("0100ABCDEF01200G") == AV_SSBU_PROGRAM_ID);
}

static void test_misc(void) {
    CHECK(av_progress_permille(0, 0) == -1);
    CHECK(av_progress_permille(50, 100) == 500);
    CHECK(av_progress_permille(150, 100) == 1000);
    CHECK(!av_log_needs_rotate(AV_LOG_CAP_BYTES - 1));
    CHECK(av_log_needs_rotate(AV_LOG_CAP_BYTES));
    CHECK(AV_PROBE_RESULT == 0x4A8u);
    CHECK(AV_POLL_MS >= 1000u && AV_FAST_SLICE_MS >= 1000u); /* no busy loop */
    CHECK(AV_FAST_WINDOW_MS / AV_FAST_SLICE_MS <= 30u);       /* bounded fast window */
}

int main(void) {
    test_firmware_allowlist_boot();
    test_clear_gate();
    test_verdict();
    test_trigger_parse();
    test_misc();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
