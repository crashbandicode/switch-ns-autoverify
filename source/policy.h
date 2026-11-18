/* ns-autoverify: pure decision logic (no libnx), host-testable. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The only title acted on in this release. */
#define AV_SSBU_PROGRAM_ID 0x01006A800016E000ULL

/* The crash result logged for SSBU's Data Abort (2168-0002); the probe uses it to reproduce the state. */
#define AV_PROBE_RESULT 0x4A8u

/* Optional manual verify (`verify-now`): RequestVerifyApplication (ns cmd 1003) with qlaunch's arguments. */
#define AV_VERIFY_FLAGS     0x7u
#define AV_VERIFY_TMEM_SIZE 0x100000u
#define AV_HEAP_GRANULE     0x200000u /* svcSetHeapSize granularity; taken only during a manual verify */

/* 0.2.2 fast path: fs 2002-4071..4079 (RomNcaHierarchicalSha256StorageCorrupted). Hardware 2026-11-18 02:09Z: SSBU
 * failed to start with 0x1FD602 = 2002-4075 RomHierarchicalSha256HashVerificationFailed and the system Check then
 * found no corrupted data (a transient bad read). The probe's ZL sets this value. */
#define AV_FS_MODULE            2u
#define AV_ROM_HS256_DESC_MIN   4071u
#define AV_ROM_HS256_DESC_MAX   4079u
#define AV_FAST_PROBE_RESULT    0x1FD602u

#define AV_BOOT_DELAY_MS        10000u
#define AV_BOOT_WINDOW_MS       60000u
#define AV_POLL_MS              10000u  /* idle wake-up; no busy loop */
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

/* True for fs results 2002-4071..4079 only. */
bool av_is_rom_hs256(uint32_t result);

/* The epoch file `config/ns-autoverify/strike`. Exactly "armed" or "used 0x<hex>" (optional trailing newline);
 * anything else, or no file, means no fast clear (fail closed). It is armed only on positive evidence (a clean SSBU
 * session or an external clear of a flagged state), never by a timer or a reboot. */
typedef enum {
    AV_STRIKE_MISSING = 0,
    AV_STRIKE_GARBLED,
    AV_STRIKE_ARMED,  /* the epoch's one fast clear is available */
    AV_STRIKE_USED,   /* a fast clear happened in this epoch: a repeat keeps the dialog */
} av_strike_t;
/* `text` NULL = no file. `len` = bytes read (an embedded NUL is garbled). *used_value is set for USED only. */
av_strike_t av_strike_parse(const char *text, size_t len, uint32_t *used_value);
/* ARMED or USED (value != 0) only; returns the length written, or -1. */
int av_strike_format(char *out, size_t size, av_strike_t s, uint32_t value);
const char *av_strike_name(av_strike_t s);

/* Epoch evidence. A session is clean when the SSBU process exited with no crash event, no flagged state was seen
 * during it or in the post-exit window, and 2100 and 2101 both read 0 at the end of that window. */
bool av_session_clean(bool crashed, bool flag_seen, bool read_ok, uint64_t key_after);
/* A flagged state went to 0 without our own clear: the system Check passed (or a redownload, or probe Y). */
bool av_external_clear(uint64_t prev_key, uint64_t key, bool cleared_by_us);

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
    av_strike_t strike;      /* the epoch file, read just before deciding */
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
    AV_CLR_FAST,        /* Rom hash-tree family, epoch armed: mark the epoch used, then call 57 */
    AV_CLR_OBSERVE_FAST, /* would fast-clear; observe-only */
    AV_CLR_STRIKE_KEEP, /* Rom hash-tree family again inside a used epoch: keep the dialog for the system Check */
    AV_CLR_UNARMED,     /* Rom hash-tree family, epoch file missing or garbled: keep the dialog (fail closed) */
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
