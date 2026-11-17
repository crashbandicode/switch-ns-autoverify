/* ns-autoverify: pure decision logic (no libnx), host-testable. */
#include "policy.h"

#include <stddef.h>

bool av_fw_supported(uint32_t major, uint32_t minor, uint32_t micro) {
    (void)minor;
    (void)micro;
    /* Validated against switchbrew NS services rev 15466 (markers through 23.5.0); the console runs 23.5.0.
     * Any other major is "unknown firmware": do nothing and exit. */
    return major == 23;
}

bool av_title_allowed(uint64_t program_id) {
    /* AV_TEST_EXTRA_PROGRAM_ID exists only for the corruption-negative test build (docs/ROLLOUT.md T4), which
     * targets a disposable title; such a build must never be left installed. */
    static const uint64_t allow[] = {
        AV_SSBU_PROGRAM_ID,
#ifdef AV_TEST_EXTRA_PROGRAM_ID
        AV_TEST_EXTRA_PROGRAM_ID,
#endif
    };
    for (size_t i = 0; i < sizeof(allow) / sizeof(allow[0]); i++) {
        if (allow[i] == program_id) {
            return true;
        }
    }
    return false;
}

av_boot_t av_boot_decision(bool disabled_present, bool busy_marker_present) {
    if (disabled_present) {
        return AV_BOOT_EXIT_DISABLED;
    }
    if (busy_marker_present) {
        return AV_BOOT_EXIT_STALE_MARKER;
    }
    return AV_BOOT_RUN;
}

av_verdict_t av_verdict(bool request_ok, uint32_t get_rc, uint32_t detail_rc, bool cancelled, bool timed_out) {
    if (!request_ok) {
        return AV_V_ERROR;
    }
    if (timed_out) {
        return AV_V_TIMEOUT;
    }
    if (cancelled) {
        return AV_V_CANCELLED;
    }
    if (get_rc != 0 || detail_rc != 0) {
        return AV_V_FAIL;
    }
    return AV_V_PASS;
}

const char *av_verdict_name(av_verdict_t v) {
    switch (v) {
        case AV_V_PASS: return "pass";
        case AV_V_FAIL: return "fail";
        case AV_V_CANCELLED: return "cancelled";
        case AV_V_TIMEOUT: return "timeout";
        case AV_V_ERROR: return "error";
    }
    return "?";
}

bool av_should_clear(av_verdict_t v, bool clear_enabled, uint32_t terminate_result_after) {
    return v == AV_V_PASS && clear_enabled && terminate_result_after != 0;
}

const char *av_reason_name(av_reason_t r) {
    switch (r) {
        case AV_REASON_NONE: return "none";
        case AV_REASON_CRASH: return "crash";
        case AV_REASON_TERMINATE_RESULT: return "terminate-result";
        case AV_REASON_TRIGGER_FILE: return "trigger-file";
    }
    return "?";
}

av_gate_t av_gate(const av_gate_in_t *in, av_reason_t reason, uint64_t program_id) {
    if (reason == AV_REASON_NONE || !av_title_allowed(program_id)) {
        return AV_GATE_REFUSE;
    }
    if (in->poisoned || in->verifies_done >= AV_MAX_VERIFIES_PER_RUN) {
        return AV_GATE_REFUSE;
    }
    if (in->app_running) {
        return AV_GATE_DEFER;
    }
    if (in->observe_only) {
        return AV_GATE_OBSERVE;
    }
    return AV_GATE_GO;
}

const char *av_gate_name(av_gate_t g) {
    switch (g) {
        case AV_GATE_GO: return "go";
        case AV_GATE_OBSERVE: return "observe";
        case AV_GATE_DEFER: return "defer";
        case AV_GATE_REFUSE: return "refuse";
    }
    return "?";
}

void av_track_init(av_tracker_t *t) {
    t->app_running = false;
    t->app_pid = 0;
    t->app_program_id = 0;
    t->crash_seen = false;
    t->crash_program_id = 0;
    t->crash_exited = false;
    t->crash_exit_ms = 0;
}

void av_track_app_start(av_tracker_t *t, uint64_t pid, uint64_t program_id) {
    t->app_running = true;
    t->app_pid = pid;
    t->app_program_id = program_id;
}

void av_track_crash(av_tracker_t *t, uint64_t pid) {
    if (!t->app_running || pid != t->app_pid) {
        return;
    }
    t->crash_seen = true;
    t->crash_program_id = t->app_program_id;
    t->crash_exited = false;
}

void av_track_exit(av_tracker_t *t, uint64_t pid, uint64_t now_ms) {
    if (!t->app_running || pid != t->app_pid) {
        return;
    }
    t->app_running = false;
    if (t->crash_seen) {
        t->crash_exited = true;
        t->crash_exit_ms = now_ms;
    }
}

bool av_track_due(const av_tracker_t *t, uint64_t now_ms, uint64_t *out_program_id) {
    if (!t->crash_seen || !t->crash_exited || t->app_running) {
        return false;
    }
    if (now_ms < t->crash_exit_ms || now_ms - t->crash_exit_ms < AV_SETTLE_MS) {
        return false;
    }
    *out_program_id = t->crash_program_id;
    return true;
}

void av_track_consume(av_tracker_t *t) {
    t->crash_seen = false;
    t->crash_exited = false;
    t->crash_program_id = 0;
    t->crash_exit_ms = 0;
}

uint64_t av_parse_trigger_id(const char *text) {
    if (text == NULL) {
        return AV_SSBU_PROGRAM_ID;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    uint64_t v = 0;
    int digits = 0;
    for (; digits < 17; digits++) {
        const char c = text[digits];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        v = (v << 4) | (uint64_t)d;
    }
    const char end = text[digits];
    if (digits != 16 || !(end == '\0' || end == '\n' || end == '\r' || end == ' ' || end == '\t')) {
        return AV_SSBU_PROGRAM_ID;
    }
    return v;
}

int av_progress_permille(int64_t current, int64_t total) {
    if (total <= 0 || current < 0) {
        return -1;
    }
    if (current >= total) {
        return 1000;
    }
    return (int)((current * 1000) / total);
}

bool av_log_needs_rotate(long size) {
    return size >= AV_LOG_CAP_BYTES;
}
