/* ns-autoverify: pure decision logic (no libnx), host-testable. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The only title acted on in this release. */
#define AV_SSBU_PROGRAM_ID 0x01006A800016E000ULL

/* The crash result logged for SSBU's Data Abort (2168-0002); the probe uses it to reproduce the state. */
#define AV_PROBE_RESULT 0x4A8u

/* Optional manual verify (`verify-now`): RequestVerifyApplication (ns cmd 1003) with qlaunch's arguments. */
#define AV_VERIFY_FLAGS     0x7u
#define AV_VERIFY_TMEM_SIZE 0x100000u
#define AV_HEAP_GRANULE     0x200000u /* svcSetHeapSize granularity; taken only during a manual verify */

#define AV_BOOT_DELAY_MS        10000u
#define AV_BOOT_WINDOW_MS       60000u
#define AV_POLL_MS              60000u  /* idle wake-up; no busy loop */
#define AV_FAST_WINDOW_MS       20000u  /* after an application exit, re-check every second for this long */
#define AV_FAST_SLICE_MS         1000u
#define AV_VERIFY_SLICE_MS       5000u
#define AV_PROGRESS_LOG_MS      60000u
#define AV_VERIFY_MAX_MS      2700000u
#define AV_CANCEL_WAIT_MS       60000u
#define AV_MAX_VERIFIES_PER_RUN     4u
#define AV_MAX_CLEARS_PER_RUN      32u
#define AV_MAX_EVENTS_PER_WAKE     32u
#define AV_LOG_CAP_BYTES        65536L

bool av_fw_supported(uint32_t major, uint32_t minor, uint32_t micro);
bool av_title_allowed(uint64_t program_id);

typedef enum {
    AV_BOOT_RUN = 0,
    AV_BOOT_EXIT_DISABLED,
    AV_BOOT_EXIT_STALE_MARKER,
} av_boot_t;
av_boot_t av_boot_decision(bool disabled_present, bool busy_marker_present);

/* ---- Fast clear ---------------------------------------------------------------------------------------------- */

/* Everything the clear decision needs, read from ns just before deciding. *_ok = the IPC succeeded. */
typedef struct {
    bool observe_only;
    bool app_running;
    uint32_t clears_done;
    bool term_ok;            /* GetApplicationTerminateResult (2100) */
    uint32_t term;
    bool term_class_ok;      /* IsDataCorruptedResult(term) (ns:ro getter -> IReadOnlyApplicationRecordInterface 2) */
    bool term_corrupt;
    bool raw_ok;             /* GetRawApplicationTerminateResult (2101) */
    uint32_t raw;
    bool raw_class_ok;       /* IsDataCorruptedResult(raw), only needed when raw != 0 */
    bool raw_corrupt;
    uint64_t failed_key;     /* av_clear_key() of a state whose clear already failed this run (0 = none) */
} av_clear_in_t;

/* Identifies one (term, raw) state. Hardware 2026-11-17: SetApplicationTerminateResult(0x4A8) changed only the raw
 * value (2101) while 2100 kept reading 0, so both are part of the state. */
uint64_t av_clear_key(uint32_t term, uint32_t raw);

typedef enum {
    AV_CLR_GO = 0,      /* call ClearApplicationTerminateResult (57) */
    AV_CLR_OBSERVE,     /* would clear; observe-only */
    AV_CLR_NOTHING,     /* terminate result and raw result are both 0: nothing to clear */
    AV_CLR_CORRUPT,     /* ns classifies the result as data corruption: keep the dialog */
    AV_CLR_UNKNOWN,     /* an ns query failed: fail closed, keep the dialog */
    AV_CLR_DEFER,       /* an application is running */
    AV_CLR_REFUSE,      /* not allowlisted, per-run limit, or this value already failed to clear */
} av_clear_t;

av_clear_t av_clear_gate(const av_clear_in_t *in, uint64_t program_id);
const char *av_clear_name(av_clear_t c);

/* ---- Optional manual verify ---------------------------------------------------------------------------------- */

typedef enum {
    AV_V_PASS = 0,
    AV_V_FAIL,
    AV_V_CANCELLED,
    AV_V_TIMEOUT,
    AV_V_ERROR,
} av_verdict_t;
av_verdict_t av_verdict(bool request_ok, uint32_t get_rc, uint32_t detail_rc, bool cancelled, bool timed_out);
const char *av_verdict_name(av_verdict_t v);

/* `verify-now` content: optional 16-hex-digit program id on the first line; otherwise SSBU. */
uint64_t av_parse_trigger_id(const char *text);

int av_progress_permille(int64_t current, int64_t total);
bool av_log_needs_rotate(long size);
