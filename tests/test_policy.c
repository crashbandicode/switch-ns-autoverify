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

/* Hardware 2026-11-18 02:09Z (hw6): term = raw = 0x1FD602 (2002-4075), ns: corrupt. */
static av_clear_in_t rom_hs256(av_strike_t strike) {
    av_clear_in_t in = benign();
    in.term = AV_FAST_PROBE_RESULT;
    in.raw = AV_FAST_PROBE_RESULT;
    in.term_corrupt = true;
    in.raw_corrupt = true;
    in.strike = strike;
    return in;
}

static void test_rom_hs256_family(void) {
    CHECK(av_is_rom_hs256(0x1FD602u)); /* 2002-4075, the hw6 value */
    CHECK(av_is_rom_hs256((4071u << 9) | 2u));
    CHECK(av_is_rom_hs256((4079u << 9) | 2u));
    CHECK(!av_is_rom_hs256((4070u << 9) | 2u));
    CHECK(!av_is_rom_hs256((4080u << 9) | 2u));
    CHECK(!av_is_rom_hs256((4075u << 9) | 3u)); /* same description, another module */
    CHECK(!av_is_rom_hs256((4535u << 9) | 2u)); /* the unrenamed server result */
    CHECK(!av_is_rom_hs256(0x1F4002u));         /* 2002-4000 */
    CHECK(!av_is_rom_hs256(0x23F402u));         /* 2002-4602 */
    CHECK(!av_is_rom_hs256(AV_PROBE_RESULT));
    CHECK(!av_is_rom_hs256(0));
    CHECK(!av_is_rom_hs256(0x1FD602u | (1u << 22))); /* bits above a Result */
}

static void test_fast_gate(void) {
    const uint64_t ssbu = AV_SSBU_PROGRAM_ID;
    /* The first strike in an armed epoch clears. */
    av_clear_in_t in = rom_hs256(AV_STRIKE_ARMED);
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_FAST);
    /* A repeat inside the epoch keeps the dialog. */
    in = rom_hs256(AV_STRIKE_USED);
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_STRIKE_KEEP);
    /* A missing or garbled epoch file fails closed. */
    in = rom_hs256(AV_STRIKE_MISSING);
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNARMED);
    in = rom_hs256(AV_STRIKE_GARBLED);
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNARMED);
    /* A non-Rom corruption family keeps, even when armed. */
    in = rom_hs256(AV_STRIKE_ARMED);
    in.term = 0x1F4002u;
    in.raw = 0x1F4002u;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);
    in = rom_hs256(AV_STRIKE_ARMED);
    in.raw = 0x23F402u; /* term in the family, raw in another corrupt family */
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);
    in = rom_hs256(AV_STRIKE_ARMED);
    in.raw = AV_PROBE_RESULT; /* mixed with a non-corrupt value: not the observed shape */
    in.raw_corrupt = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_CORRUPT);
    /* raw-only (term 0) in the family is still the family. */
    in = rom_hs256(AV_STRIKE_ARMED);
    in.term = 0;
    in.term_corrupt = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_FAST);
    /* An application running means no action. */
    in = rom_hs256(AV_STRIKE_ARMED);
    in.app_running = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_DEFER);
    /* Observe-only logs only. */
    in = rom_hs256(AV_STRIKE_ARMED);
    in.observe_only = true;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_OBSERVE_FAST);
    in.strike = AV_STRIKE_USED;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_STRIKE_KEEP);
    /* Failed queries, limits and refusals outrank the fast path. */
    in = rom_hs256(AV_STRIKE_ARMED);
    in.term_class_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    in = rom_hs256(AV_STRIKE_ARMED);
    in.raw_ok = false;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_UNKNOWN);
    in = rom_hs256(AV_STRIKE_ARMED);
    in.clears_done = AV_MAX_CLEARS_PER_RUN;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_REFUSE);
    in = rom_hs256(AV_STRIKE_ARMED);
    in.failed_key = av_clear_key(AV_FAST_PROBE_RESULT, AV_FAST_PROBE_RESULT);
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_REFUSE);
    CHECK(av_clear_gate(&in, 0x0100000000001000ULL) == AV_CLR_REFUSE);
    /* The 0.2.1 non-corrupt path ignores the epoch entirely. */
    in = benign();
    in.strike = AV_STRIKE_MISSING;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_GO);
    in.strike = AV_STRIKE_USED;
    CHECK(av_clear_gate(&in, ssbu) == AV_CLR_GO);
    CHECK(strcmp(av_clear_name(AV_CLR_FAST), "fast-clear") == 0);
    CHECK(strcmp(av_clear_name(AV_CLR_STRIKE_KEEP), "strike-keep") == 0);
    CHECK(strcmp(av_clear_name(AV_CLR_UNARMED), "unarmed-keep") == 0);
}

static av_strike_t parse(const char *s, uint32_t *v) {
    return av_strike_parse(s, s ? strlen(s) : 0, v);
}

static void test_strike_file(void) {
    uint32_t v = 0;
    CHECK(av_strike_parse(NULL, 0, &v) == AV_STRIKE_MISSING);
    CHECK(parse("armed", &v) == AV_STRIKE_ARMED);
    CHECK(parse("armed\n", &v) == AV_STRIKE_ARMED);
    CHECK(parse("armed\r\n", &v) == AV_STRIKE_ARMED);
    CHECK(parse("used 0x1fd602\n", &v) == AV_STRIKE_USED && v == 0x1FD602u);
    CHECK(parse("used 0x1FD602", &v) == AV_STRIKE_USED && v == 0x1FD602u);
    /* Garbled: anything else fails closed. */
    CHECK(parse("", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("\n", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("Armed\n", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("armed\n\n", &v) == AV_STRIKE_GARBLED);
    CHECK(parse(" armed", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("armedx", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("used", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("used 0x", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("used 0x0", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("used 0x1fd60g", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("used 1fd602", &v) == AV_STRIKE_GARBLED);
    CHECK(parse("used 0x123456789", &v) == AV_STRIKE_GARBLED);
    CHECK(av_strike_parse("armed\0x", 7, &v) == AV_STRIKE_GARBLED);
    static const char nul_inside[5] = {'a', 'r', '\0', 'e', 'd'};
    CHECK(av_strike_parse(nul_inside, sizeof(nul_inside), &v) == AV_STRIKE_GARBLED);
    /* Format round-trips; only ARMED and a non-zero USED are writable. */
    char buf[32];
    CHECK(av_strike_format(buf, sizeof(buf), AV_STRIKE_ARMED, 0) == 6 && parse(buf, &v) == AV_STRIKE_ARMED);
    v = 0;
    CHECK(av_strike_format(buf, sizeof(buf), AV_STRIKE_USED, 0x1FD602u) > 0 && parse(buf, &v) == AV_STRIKE_USED &&
          v == 0x1FD602u);
    CHECK(av_strike_format(buf, sizeof(buf), AV_STRIKE_USED, 0) == -1);
    CHECK(av_strike_format(buf, sizeof(buf), AV_STRIKE_GARBLED, 1) == -1);
    CHECK(av_strike_format(buf, sizeof(buf), AV_STRIKE_MISSING, 1) == -1);
    CHECK(av_strike_format(buf, 4, AV_STRIKE_ARMED, 0) == -1);
    CHECK(strcmp(av_strike_name(AV_STRIKE_USED), "used") == 0);
}

static void test_epoch_evidence(void) {
    CHECK(av_session_clean(false, false, true, 0));
    CHECK(!av_session_clean(true, false, true, 0));  /* a crash event */
    CHECK(!av_session_clean(false, true, true, 0));  /* a flag seen during the session or the post-exit window */
    CHECK(!av_session_clean(false, false, false, 0)); /* the final read failed */
    CHECK(!av_session_clean(false, false, true, av_clear_key(0, AV_PROBE_RESULT)));
    const uint64_t k = av_clear_key(AV_FAST_PROBE_RESULT, AV_FAST_PROBE_RESULT);
    CHECK(av_external_clear(k, 0, false));
    CHECK(!av_external_clear(k, 0, true));  /* our own clear */
    CHECK(!av_external_clear(0, 0, false)); /* nothing was flagged */
    CHECK(!av_external_clear(k, k, false)); /* still flagged */
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
    test_rom_hs256_family();
    test_fast_gate();
    test_strike_file();
    test_epoch_evidence();
    test_verdict();
    test_trigger_parse();
    test_misc();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
