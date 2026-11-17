/* ns-autoverify: pure decision logic (no libnx), host-testable. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The only title acted on in this release. */
#define AV_SSBU_PROGRAM_ID 0x01006A800016E000ULL

/* RequestVerifyApplication (ns IApplicationManagerInterface cmd 1003) arguments, copied from what qlaunch's
 * "Check" passes (libnx ns.h nsRequestVerifyApplication doc; switchbrew NS services). */
#define AV_VERIFY_FLAGS     0x7u
#define AV_VERIFY_TMEM_SIZE 0x100000u
/* svcSetHeapSize granularity; the TransferMemory buffer is taken from the process heap only while verifying. */
#define AV_HEAP_GRANULE     0x200000u

#define AV_BOOT_DELAY_MS        10000u  /* only applied within the first minute after console boot */
#define AV_BOOT_WINDOW_MS       60000u
#define AV_POLL_MS              60000u  /* idle wake-up; no busy loop */
#define AV_SETTLE_MS            15000u  /* after the crashed app exited, let ns/am record the failure */
#define AV_VERIFY_SLICE_MS       5000u
#define AV_PROGRESS_LOG_MS      60000u
#define AV_VERIFY_MAX_MS      2700000u  /* 45 min hard ceiling (SSBU measured ~8 min 14 s) */
#define AV_CANCEL_WAIT_MS       60000u
#define AV_MAX_VERIFIES_PER_RUN     4u
#define AV_MAX_EVENTS_PER_WAKE     32u
#define AV_LOG_CAP_BYTES        65536L

bool av_fw_supported(uint32_t major, uint32_t minor, uint32_t micro);
bool av_title_allowed(uint64_t program_id);

typedef enum {
    AV_BOOT_RUN = 0,
    AV_BOOT_EXIT_DISABLED,      /* operator kill switch present */
    AV_BOOT_EXIT_STALE_MARKER,  /* previous run stopped inside init or a verify: disable, exit */
} av_boot_t;
av_boot_t av_boot_decision(bool disabled_present, bool busy_marker_present);

typedef enum {
    AV_V_PASS = 0,
    AV_V_FAIL,       /* ns completed and reported a non-zero result: treat as possible real corruption */
    AV_V_CANCELLED,  /* an application started during the verify */
    AV_V_TIMEOUT,
    AV_V_ERROR,      /* could not start the verify (IPC / memory) */
} av_verdict_t;
av_verdict_t av_verdict(bool request_ok, uint32_t get_rc, uint32_t detail_rc, bool cancelled, bool timed_out);
const char *av_verdict_name(av_verdict_t v);

/* ClearApplicationTerminateResult only after a PASS, only when the operator enabled it, only if non-zero. */
bool av_should_clear(av_verdict_t v, bool clear_enabled, uint32_t terminate_result_after);

typedef enum {
    AV_REASON_NONE = 0,
    AV_REASON_CRASH,             /* pgl reported an Exception for the tracked application, which then exited */
    AV_REASON_TERMINATE_RESULT,  /* start-up: ns GetApplicationTerminateResult != 0 */
    AV_REASON_TRIGGER_FILE,      /* operator one-shot file */
} av_reason_t;
const char *av_reason_name(av_reason_t r);

typedef struct {
    bool observe_only;
    bool poisoned;        /* a previous verify could not release its buffer: never verify again this run */
    bool app_running;
    uint32_t verifies_done;
} av_gate_in_t;

typedef enum {
    AV_GATE_GO = 0,
    AV_GATE_OBSERVE,  /* log what would be done, do nothing */
    AV_GATE_DEFER,    /* an application is running: try again later */
    AV_GATE_REFUSE,
} av_gate_t;
av_gate_t av_gate(const av_gate_in_t *in, av_reason_t reason, uint64_t program_id);
const char *av_gate_name(av_gate_t g);

/* Application crash tracker, fed from pgl shell events. */
typedef struct {
    bool app_running;
    uint64_t app_pid;
    uint64_t app_program_id;
    bool crash_seen;
    uint64_t crash_program_id;
    bool crash_exited;
    uint64_t crash_exit_ms;
} av_tracker_t;
void av_track_init(av_tracker_t *t);
void av_track_app_start(av_tracker_t *t, uint64_t pid, uint64_t program_id);
void av_track_crash(av_tracker_t *t, uint64_t pid);
void av_track_exit(av_tracker_t *t, uint64_t pid, uint64_t now_ms);
bool av_track_due(const av_tracker_t *t, uint64_t now_ms, uint64_t *out_program_id);
void av_track_consume(av_tracker_t *t);

/* `verify-now` content: optional 16-hex-digit program id (optionally 0x-prefixed) on the first line.
 * Empty / unparsable content means SSBU. The result is still subject to av_gate()'s allowlist. */
uint64_t av_parse_trigger_id(const char *text);

/* -1 when the total is unknown. */
int av_progress_permille(int64_t current, int64_t total);
bool av_log_needs_rotate(long size);
