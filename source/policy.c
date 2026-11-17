/* ns-autoverify: pure decision logic (no libnx), host-testable. */
#include "policy.h"

/* Note: the per-crash decision is av_clear_gate(); verify is only the optional manual `verify-now` action. */

#include <stddef.h>

bool av_fw_supported(uint32_t major, uint32_t minor, uint32_t micro) {
    (void)minor;
    (void)micro;
    /* Validated against switchbrew NS services rev 15466 (markers through 23.5.0); the console runs 23.5.0. */
    return major == 23;
}

bool av_title_allowed(uint64_t program_id) {
    /* AV_TEST_EXTRA_PROGRAM_ID exists only for test builds against a disposable title; never leave one installed. */
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

uint64_t av_clear_key(uint32_t term, uint32_t raw) {
    return ((uint64_t)term << 32) | raw;
}

av_clear_t av_clear_gate(const av_clear_in_t *in, uint64_t program_id) {
    if (!av_title_allowed(program_id) || in->clears_done >= AV_MAX_CLEARS_PER_RUN) {
        return AV_CLR_REFUSE;
    }
    if (in->app_running) {
        return AV_CLR_DEFER;
    }
    if (!in->term_ok || !in->raw_ok) {
        return AV_CLR_UNKNOWN;
    }
    if (in->term == 0 && in->raw == 0) {
        return AV_CLR_NOTHING;
    }
    if (in->failed_key != 0 && in->failed_key == av_clear_key(in->term, in->raw)) {
        return AV_CLR_REFUSE;
    }
    /* ns must positively say "not data corruption" for every non-zero result it reports. */
    if (in->term != 0) {
        if (!in->term_class_ok) {
            return AV_CLR_UNKNOWN;
        }
        if (in->term_corrupt) {
            return AV_CLR_CORRUPT;
        }
    }
    if (in->raw != 0) {
        if (!in->raw_class_ok) {
            return AV_CLR_UNKNOWN;
        }
        if (in->raw_corrupt) {
            return AV_CLR_CORRUPT;
        }
    }
    if (in->observe_only) {
        return AV_CLR_OBSERVE;
    }
    return AV_CLR_GO;
}

const char *av_clear_name(av_clear_t c) {
    switch (c) {
        case AV_CLR_GO: return "clear";
        case AV_CLR_OBSERVE: return "observe";
        case AV_CLR_NOTHING: return "nothing";
        case AV_CLR_CORRUPT: return "corrupt-keep";
        case AV_CLR_UNKNOWN: return "unknown-keep";
        case AV_CLR_DEFER: return "defer";
        case AV_CLR_REFUSE: return "refuse";
    }
    return "?";
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
