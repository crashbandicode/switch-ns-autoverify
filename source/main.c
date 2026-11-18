/*
 * ns-autoverify 0.2 (fast clear). When Super Smash Bros. Ultimate terminates abnormally, ns records a terminate
 * result and qlaunch then shows "Corrupted data has been detected" at the next launch. This sysmodule asks ns itself
 * whether that result is a data-corruption result (IReadOnlyApplicationRecordInterface::IsDataCorruptedResult) and,
 * only when ns positively says it is NOT, clears it at once with ClearApplicationTerminateResult (ns cmd 57). A real
 * data-corruption result, or any failed query, keeps the dialog. No verify, no big buffer.
 *
 * 0.2.2 fast path: a Rom hash-tree result (fs 2002-4071..4079, e.g. 0x1FD602 after a transient bad read) is cleared
 * once per verified epoch (`strike` file "armed" -> "used 0x..." written before the clear). The next launch re-reads
 * the same data, so a real corruption fails again and is kept for the system Check. The epoch is re-armed only on
 * positive evidence: a clean SSBU session (start, exit without a crash event, no flag seen, 2100 = 2101 = 0 at the
 * end of the post-exit window) or an external clear of a flagged state (the system Check passing).
 *
 * Optional manual action: `verify-now` runs the official RequestVerifyApplication (as HOME "Check") once.
 *
 * Fail-safe rules: no fatalThrow / diagAbort; any init error or firmware other than 23.x -> log and exit; a stale
 * busy marker from an interrupted run -> create `disabled` and exit; `disabled` is honoured at start and every wake;
 * every wait is bounded.
 */
#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nsx.h"
#include "policy.h"

/* libnx 4.12.0 (console build) vs newer libnx sources (`make check-host`) spell the pgl observer API differently. */
#ifdef AV_LIBNX_NEW_PGL_NAMES
#define av_pglGetObserver       pglCreateShellEvent
#define av_pglObserverEvent     pglEventObserverGetShellEvent
#define av_pglObserverEventInfo pglEventObserverGetShellEventInfo
#else
#define av_pglGetObserver       pglGetEventObserver
#define av_pglObserverEvent     pglEventObserverGetProcessEvent
#define av_pglObserverEventInfo pglEventObserverGetProcessEventInfo
#endif

#define AV_VERSION "0.2.2"

#define CFG_DIR    "sdmc:/config/ns-autoverify"
#define LOG_DIR    "sdmc:/atmosphere/logs"
#define LOG_PATH   LOG_DIR "/ns-autoverify.log"
#define LOG_OLD    LOG_DIR "/ns-autoverify.log.1"
#define F_DISABLED CFG_DIR "/disabled"
#define F_OBSERVE  CFG_DIR "/observe-only"
#define F_TRIGGER  CFG_DIR "/verify-now"
#define F_BUSY     CFG_DIR "/busy"
#define F_STATUS   CFG_DIR "/status.txt"
#define F_STRIKE   CFG_DIR "/strike"

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x30000

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {}
void __appExit(void) {}

static bool g_sd;
static bool g_sm, g_pmdmnt, g_pgl, g_ns, g_observer, g_ns_event_ok;
static PglEventObserver g_obs;
static Event g_pgl_event;
static Event g_ns_event; /* ns ApplicationRecordUpdateSystemEvent (autoclear) */

/* The SSBU session being watched as epoch evidence (pid 0 = none). */
static u64 g_sess_pid;
static bool g_sess_crashed, g_sess_flag_seen;
static u64 g_sess_exit_ms; /* 0 = still running */

static u64 now_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static bool file_exists(const char *path) {
    struct stat st;
    return g_sd && stat(path, &st) == 0;
}

static void write_text(const char *path, const char *text) {
    if (!g_sd) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void remove_file(const char *path) {
    if (g_sd) {
        remove(path);
    }
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    if (!g_sd) {
        return;
    }
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && av_log_needs_rotate((long)st.st_size)) {
        remove(LOG_OLD);
        rename(LOG_PATH, LOG_OLD);
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        return;
    }
    const u64 ms = now_ms();
    fprintf(f, "[%llu.%03llu] ", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void status_(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status_(const char *state, const char *fmt, ...) {
    if (!g_sd) {
        return;
    }
    char extra[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    char line[512];
    snprintf(line, sizeof(line), "state=%s uptime_ms=%llu version=%s %s\n", state, (unsigned long long)now_ms(),
             AV_VERSION, extra);
    write_text(F_STATUS, line);
}

static void log_snapshot(const char *tag, u64 id, nsx_snap_t *s) {
    nsx_snapshot(id, s);
    char line[640];
    nsx_format(s, id, line, sizeof(line));
    logf_("snapshot[%s] %s", tag, line);
}

/* ---- Epoch file (`strike`) ----------------------------------------------------------------------------------- */

static av_strike_t read_strike(u32 *value) {
    *value = 0;
    struct stat st;
    if (!g_sd || stat(F_STRIKE, &st) != 0) {
        return AV_STRIKE_MISSING;
    }
    char buf[32];
    FILE *f = fopen(F_STRIKE, "r");
    if (!f) {
        return AV_STRIKE_GARBLED;
    }
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == sizeof(buf)) {
        return AV_STRIKE_GARBLED; /* longer than any valid content */
    }
    return av_strike_parse(buf, n, value);
}

/* Writes ARMED or USED and reads it back; false = not on the card as intended. */
static bool write_strike(av_strike_t s, u32 value) {
    char text[32];
    if (!g_sd || av_strike_format(text, sizeof(text), s, value) < 0) {
        return false;
    }
    FILE *f = fopen(F_STRIKE, "w");
    if (!f) {
        return false;
    }
    const bool put = fputs(text, f) >= 0;
    if (fclose(f) != 0 || !put) {
        return false;
    }
    u32 back = 0;
    return read_strike(&back) == s && (s != AV_STRIKE_USED || back == value);
}

static void strike_text(av_strike_t s, u32 value, char *out, size_t size) {
    if (s == AV_STRIKE_USED) {
        snprintf(out, size, "used:0x%x", value);
    } else {
        snprintf(out, size, "%s", av_strike_name(s));
    }
}

/* Re-arm the epoch on positive evidence only (`evidence` names it). */
static void epoch_arm(const char *evidence) {
    u32 v = 0;
    const av_strike_t cur = read_strike(&v);
    char was[32];
    strike_text(cur, v, was, sizeof(was));
    if (cur == AV_STRIKE_ARMED) {
        logf_("epoch: evidence=%s strike=armed (unchanged)", evidence);
        return;
    }
    const bool ok = write_strike(AV_STRIKE_ARMED, 0);
    logf_("epoch: evidence=%s strike %s -> armed write_ok=%d", evidence, was, ok ? 1 : 0);
}

static bool query_app_running(void) {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
}

static void session_start(u64 pid) {
    if (g_sess_pid != 0) {
        logf_("session pid=%llu superseded before its judgement; no epoch evidence", (unsigned long long)g_sess_pid);
    }
    g_sess_pid = pid;
    g_sess_crashed = false;
    g_sess_flag_seen = false;
    g_sess_exit_ms = 0;
    u32 v = 0;
    char st[32];
    strike_text(read_strike(&v), v, st, sizeof(st));
    logf_("session start pid=%llu id=%016llx strike=%s", (unsigned long long)pid,
          (unsigned long long)AV_SSBU_PROGRAM_ID, st);
}

/* End of the post-exit window: a clean SSBU session is positive evidence that its launch path read back intact. */
static void judge_session(void) {
    Result term = 0;
    u32 raw = 0;
    const bool read_ok = R_SUCCEEDED(nsGetApplicationTerminateResult(AV_SSBU_PROGRAM_ID, &term)) &&
                         R_SUCCEEDED(nsx_get_raw_terminate_result(AV_SSBU_PROGRAM_ID, &raw));
    const bool clean = av_session_clean(g_sess_crashed, g_sess_flag_seen, read_ok, av_clear_key(term, raw));
    logf_("session end pid=%llu crashed=%d flag_seen=%d read_ok=%d term=0x%x raw=0x%x clean=%d",
          (unsigned long long)g_sess_pid, g_sess_crashed, g_sess_flag_seen, read_ok, term, raw, clean);
    g_sess_pid = 0;
    g_sess_exit_ms = 0;
    if (clean) {
        epoch_arm("clean-session");
    }
}

/* Drain pgl events. Sets *exited when any process exited (an application exit is what matters; the next
 * evaluation re-reads ns either way) and *started when an application process started. */
static void drain_pgl(bool *exited, bool *started) {
    for (u32 n = 0; n < AV_MAX_EVENTS_PER_WAKE; n++) {
        PmProcessEventInfo info;
        memset(&info, 0, sizeof(info));
        if (R_FAILED(av_pglObserverEventInfo(&g_obs, &info)) || info.event == PmProcessEvent_None) {
            break;
        }
        if (info.event == PmProcessEvent_Exit) {
            *exited = true;
            if (g_sess_pid != 0 && info.process_id == g_sess_pid && g_sess_exit_ms == 0) {
                g_sess_exit_ms = now_ms() | 1;
                logf_("session exit pid=%llu crashed=%d flag_seen=%d", (unsigned long long)g_sess_pid,
                      g_sess_crashed, g_sess_flag_seen);
            }
        } else if (info.event == PmProcessEvent_Start) {
            u64 app_pid = 0;
            if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&app_pid)) && app_pid == info.process_id) {
                *started = true;
            }
            u64 program_id = 0;
            if (R_SUCCEEDED(pmdmntGetProgramId(&program_id, info.process_id)) && program_id == AV_SSBU_PROGRAM_ID) {
                session_start(info.process_id);
            }
        } else if (info.event == PmProcessEvent_Crash) {
            if (g_sess_pid != 0 && info.process_id == g_sess_pid) {
                g_sess_crashed = true;
            }
            u64 app_pid = 0;
            if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&app_pid)) && app_pid == info.process_id) {
                u64 program_id = 0;
                pmdmntGetProgramId(&program_id, info.process_id);
                logf_("event crash application pid=%llu id=%016llx", (unsigned long long)info.process_id,
                      (unsigned long long)program_id);
            }
        }
    }
}

/* ---- Fast clear ---------------------------------------------------------------------------------------------- */

static u32 g_clears;
static u64 g_failed_key;
static u64 g_last_logged_key = UINT64_MAX;
static av_clear_t g_last_logged_gate = AV_CLR_REFUSE;
static u64 g_prev_key; /* (term, raw) at the previous good read; 0 after our own clear */

static void evaluate_clear(u64 id) {
    /* Cheap first read of both results (2100 and the raw 2101): nothing to do while both are 0. */
    Result term = 0;
    u32 raw = 0;
    const Result trc = nsGetApplicationTerminateResult(id, &term);
    const Result rrc = nsx_get_raw_terminate_result(id, &raw);
    const bool read_ok = R_SUCCEEDED(trc) && R_SUCCEEDED(rrc);
    const u64 key = av_clear_key(term, raw);
    if (read_ok) {
        if (key != 0 && g_sess_pid != 0) {
            g_sess_flag_seen = true;
        }
        if (av_external_clear(g_prev_key, key, false)) {
            logf_("external clear: term 0x%x raw 0x%x -> 0 without our clear (system Check, redownload or probe Y)",
                  (u32)(g_prev_key >> 32), (u32)g_prev_key);
            epoch_arm("external-clear");
        }
        g_prev_key = key;
    }
    if (read_ok && key == 0) {
        g_last_logged_key = 0;
        return;
    }
    /* Final verdicts for this state: no repeated full snapshots. Unknown / deferred / observed are re-checked. The
     * epoch cannot change while the state stays flagged (it is re-armed only after the state reads 0). */
    if (read_ok && key == g_last_logged_key &&
        (g_last_logged_gate == AV_CLR_CORRUPT || g_last_logged_gate == AV_CLR_REFUSE ||
         g_last_logged_gate == AV_CLR_STRIKE_KEEP || g_last_logged_gate == AV_CLR_UNARMED)) {
        return;
    }

    nsx_snap_t s;
    nsx_snapshot(id, &s);
    u32 strike_value = 0;
    const av_strike_t strike = read_strike(&strike_value);
    char st[32];
    strike_text(strike, strike_value, st, sizeof(st));
    av_clear_in_t in = {
        .observe_only = file_exists(F_OBSERVE),
        .app_running = query_app_running(),
        .clears_done = g_clears,
        .term_ok = R_SUCCEEDED(s.term_rc),
        .term = s.term,
        .term_class_ok = R_SUCCEEDED(s.term_class_rc),
        .term_corrupt = s.term_corrupt,
        .raw_ok = R_SUCCEEDED(s.raw_rc),
        .raw = s.raw,
        .raw_class_ok = R_SUCCEEDED(s.raw_class_rc),
        .raw_corrupt = s.raw_corrupt,
        .failed_key = g_failed_key,
        .strike = strike,
    };
    const av_clear_t g = av_clear_gate(&in, id);
    const u64 skey = av_clear_key(s.term, s.raw);

    /* Log a decision once per ((term, raw), gate); deferral while a game runs is not re-logged. */
    if (skey != g_last_logged_key || g != g_last_logged_gate) {
        char line[704];
        nsx_format(&s, id, line, sizeof(line));
        logf_("decision gate=%s observe_only=%d app_running=%d clears=%u strike=%s rom_hs256=%d/%d %s",
              av_clear_name(g), in.observe_only, in.app_running, g_clears, st, av_is_rom_hs256(s.term),
              av_is_rom_hs256(s.raw), line);
        g_last_logged_key = skey;
        g_last_logged_gate = g;
        if (g == AV_CLR_CORRUPT || g == AV_CLR_UNKNOWN || g == AV_CLR_STRIKE_KEEP || g == AV_CLR_UNARMED) {
            status_(av_clear_name(g),
                    "id=%016llx term=0x%x raw=0x%x term_class_rc=0x%x raw_class_rc=0x%x flags=0x%08x strike=%s",
                    (unsigned long long)id, s.term, s.raw, s.term_class_rc, s.raw_class_rc, s.view.flags, st);
        } else if (g == AV_CLR_OBSERVE || g == AV_CLR_OBSERVE_FAST) {
            status_("observed", "id=%016llx term=0x%x raw=0x%x would=%s strike=%s", (unsigned long long)id, s.term,
                    s.raw, av_clear_name(g == AV_CLR_OBSERVE ? AV_CLR_GO : AV_CLR_FAST), st);
        }
    }
    if (g != AV_CLR_GO && g != AV_CLR_FAST) {
        return;
    }

    write_text(F_BUSY, "clear\n");
    if (g == AV_CLR_FAST) {
        /* The epoch is consumed before the clear; if that cannot be recorded, nothing is cleared. */
        const u32 v = s.raw != 0 ? s.raw : (u32)s.term;
        if (!write_strike(AV_STRIKE_USED, v)) {
            remove_file(F_BUSY);
            g_failed_key = skey;
            logf_("fast-clear: writing strike 'used 0x%x' failed or did not read back; keeping the dialog", v);
            status_("strike-write-failed", "id=%016llx term=0x%x raw=0x%x strike=%s", (unsigned long long)id,
                    s.term, s.raw, st);
            return;
        }
        logf_("fast-clear: strike %s -> used:0x%x (this epoch's one fast clear)", st, v);
    }
    g_clears++;
    const Result crc = nsClearApplicationTerminateResult(id);
    nsx_snap_t after;
    log_snapshot("after-clear", id, &after);
    remove_file(F_BUSY);
    const bool took = R_SUCCEEDED(crc) && R_SUCCEEDED(after.term_rc) && R_SUCCEEDED(after.raw_rc) &&
                      after.term == 0 && after.raw == 0;
    logf_("clear: path=%s ClearApplicationTerminateResult rc=0x%x term 0x%x -> 0x%x raw 0x%x -> 0x%x "
          "flags 0x%08x -> 0x%08x last_event %u -> %u took=%d",
          av_clear_name(g), crc, s.term, after.term, s.raw, after.raw, s.view.flags, after.view.flags,
          s.rec_found ? (unsigned)s.rec.last_event : 0u, after.rec_found ? (unsigned)after.rec.last_event : 0u,
          took ? 1 : 0);
    if (!took) {
        g_failed_key = skey; /* never retry the same state this run */
    }
    status_(took ? "cleared" : "clear-failed",
            "id=%016llx term_before=0x%x raw_before=0x%x clear_rc=0x%x term_after=0x%x raw_after=0x%x "
            "flags_before=0x%08x flags_after=0x%08x clears=%u path=%s",
            (unsigned long long)id, s.term, s.raw, crc, after.term, after.raw, s.view.flags, after.view.flags,
            g_clears, av_clear_name(g));
    g_last_logged_key = av_clear_key(after.term, after.raw);
    g_prev_key = 0; /* whatever changes next was caused by this clear, not by an external one */
}

/* ---- Optional manual verify (`verify-now`) ------------------------------------------------------------------- */

static bool g_poisoned;
static u32 g_verifies;

static void run_verify(u64 id) {
    nsx_snap_t before, after;
    log_snapshot("verify-before", id, &before);
    write_text(F_BUSY, "verify\n");
    status_("verifying", "id=%016llx permille=0", (unsigned long long)id);
    g_verifies++;

    void *heap = NULL;
    Result hrc = svcSetHeapSize(&heap, AV_HEAP_GRANULE);
    if (R_FAILED(hrc) || heap == NULL) {
        logf_("verify: heap allocation failed rc=0x%x; not verifying", hrc);
        remove_file(F_BUSY);
        status_("error", "id=%016llx detail=heap rc=0x%x", (unsigned long long)id, hrc);
        return;
    }

    NsProgressAsyncResult a;
    memset(&a, 0, sizeof(a));
    Result rrc = nsRequestVerifyApplication(&a, id, AV_VERIFY_FLAGS, heap, AV_VERIFY_TMEM_SIZE);
    logf_("verify: RequestVerifyApplication(id=%016llx, 0x%x, 0x%x) rc=0x%x", (unsigned long long)id,
          AV_VERIFY_FLAGS, AV_VERIFY_TMEM_SIZE, rrc);

    bool cancelled = false, timed_out = false, leaked = false, done = false;
    Result get_rc = 0, detail_rc = 0;
    if (R_SUCCEEDED(rrc)) {
        const u64 start = now_ms();
        u64 cancel_at = 0, last_progress = start;
        while (!done) {
            s32 idx = -1;
            Result wr = waitMulti(&idx, (u64)AV_VERIFY_SLICE_MS * 1000000ULL, waiterForEvent(&a.event),
                                  waiterForEvent(&g_pgl_event));
            const u64 t = now_ms();
            if (R_SUCCEEDED(wr) && idx == 0) {
                done = true;
                break;
            }
            if (R_SUCCEEDED(wr) && idx == 1) {
                bool exited = false, started = false;
                drain_pgl(&exited, &started);
                if (started && !cancelled && !timed_out) {
                    logf_("verify: an application started; cancelling");
                    nsProgressAsyncResultCancel(&a);
                    cancelled = true;
                    cancel_at = t;
                }
            } else if (R_FAILED(wr) && wr != KERNELRESULT(TimedOut)) {
                logf_("verify: wait failed rc=0x%x; cancelling", wr);
                if (!cancelled && !timed_out) {
                    nsProgressAsyncResultCancel(&a);
                    timed_out = true;
                    cancel_at = t;
                }
                svcSleepThread((u64)AV_VERIFY_SLICE_MS * 1000000ULL);
            }
            if (!cancelled && !timed_out && t - start > AV_VERIFY_MAX_MS) {
                nsProgressAsyncResultCancel(&a);
                timed_out = true;
                cancel_at = t;
            }
            if ((cancelled || timed_out) && t - cancel_at > AV_CANCEL_WAIT_MS) {
                leaked = true;
                break;
            }
            if (t - last_progress >= AV_PROGRESS_LOG_MS) {
                last_progress = t;
                NsSystemUpdateProgress p;
                memset(&p, 0, sizeof(p));
                nsProgressAsyncResultGetProgress(&a, &p, sizeof(p));
                const int pm = av_progress_permille(p.current_size, p.total_size);
                status_("verifying", "id=%016llx permille=%d", (unsigned long long)id, pm);
            }
        }
        if (done) {
            get_rc = nsProgressAsyncResultGet(&a);
            detail_rc = nsProgressAsyncResultGetDetailResult(&a);
            nsProgressAsyncResultClose(&a);
        } else {
            serviceClose(&a.s);
            eventClose(&a.event);
        }
    }

    if (leaked) {
        g_poisoned = true;
    } else {
        Result frc = 0;
        for (int attempt = 0; attempt < 5; attempt++) {
            void *dummy = NULL;
            frc = svcSetHeapSize(&dummy, 0);
            if (R_SUCCEEDED(frc)) {
                break;
            }
            svcSleepThread(1000000000ULL);
        }
        if (R_FAILED(frc)) {
            g_poisoned = true;
        }
    }

    const av_verdict_t v = av_verdict(R_SUCCEEDED(rrc), get_rc, detail_rc, cancelled, timed_out);
    log_snapshot("verify-after", id, &after);
    remove_file(F_BUSY);
    logf_("verify: verdict=%s get_rc=0x%x detail_rc=0x%x leaked=%d poisoned=%d", av_verdict_name(v), get_rc,
          detail_rc, leaked, g_poisoned);
    status_(av_verdict_name(v), "id=%016llx request_rc=0x%x get_rc=0x%x detail_rc=0x%x term_after=0x%x",
            (unsigned long long)id, rrc, get_rc, detail_rc, after.term);
}

/* ---- Init / main --------------------------------------------------------------------------------------------- */

static bool init_services(void) {
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        logf_("init: set:sys rc=0x%x", rc);
        return false;
    }
    SetSysFirmwareVersion fw;
    memset(&fw, 0, sizeof(fw));
    rc = setsysGetFirmwareVersion(&fw);
    setsysExit();
    if (R_FAILED(rc)) {
        logf_("init: GetFirmwareVersion rc=0x%x", rc);
        return false;
    }
    hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    logf_("init: firmware %u.%u.%u", fw.major, fw.minor, fw.micro);
    if (!av_fw_supported(fw.major, fw.minor, fw.micro)) {
        logf_("init: firmware %u.%u.%u is not validated; doing nothing", fw.major, fw.minor, fw.micro);
        status_("unsupported-firmware", "fw=%u.%u.%u", fw.major, fw.minor, fw.micro);
        return false;
    }
    if (R_FAILED(rc = pmdmntInitialize())) {
        logf_("init: pm:dmnt rc=0x%x", rc);
        return false;
    }
    g_pmdmnt = true;
    if (R_FAILED(rc = pglInitialize())) {
        logf_("init: pgl rc=0x%x", rc);
        return false;
    }
    g_pgl = true;
    if (R_FAILED(rc = nsInitialize())) {
        logf_("init: ns rc=0x%x", rc);
        return false;
    }
    g_ns = true;
    /* Optional: a failed launch has no process exit, so the record-update event is the prompt wake-up. */
    if (R_SUCCEEDED(rc = nsGetApplicationRecordUpdateSystemEvent(&g_ns_event))) {
        g_ns_event_ok = true;
    } else {
        logf_("init: ns record-update event rc=0x%x; polling every %u ms only", rc, AV_POLL_MS);
    }
    if (R_FAILED(rc = av_pglGetObserver(&g_obs))) {
        logf_("init: pgl observer rc=0x%x", rc);
        return false;
    }
    g_observer = true;
    if (R_FAILED(rc = av_pglObserverEvent(&g_obs, &g_pgl_event))) {
        logf_("init: pgl observer event rc=0x%x", rc);
        return false;
    }
    return true;
}

static void fini_services(void) {
    if (g_observer) {
        eventClose(&g_pgl_event);
        pglEventObserverClose(&g_obs);
    }
    if (g_ns_event_ok) {
        eventClose(&g_ns_event);
    }
    if (g_ns) {
        nsExit();
    }
    if (g_pgl) {
        pglExit();
    }
    if (g_pmdmnt) {
        pmdmntExit();
    }
    if (g_sm) {
        smExit();
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (now_ms() < AV_BOOT_WINDOW_MS) {
        svcSleepThread((u64)AV_BOOT_DELAY_MS * 1000000ULL);
    }

    if (R_FAILED(smInitialize())) {
        return 0;
    }
    g_sm = true;
    if (R_SUCCEEDED(fsInitialize())) {
        g_sd = R_SUCCEEDED(fsdevMountSdmc());
    }
    if (!g_sd) {
        fini_services();
        return 0;
    }
    mkdir("sdmc:/config", 0777);
    mkdir(CFG_DIR, 0777);
    mkdir("sdmc:/atmosphere", 0777);
    mkdir(LOG_DIR, 0777);
    logf_("ns-autoverify %s start", AV_VERSION);

    switch (av_boot_decision(file_exists(F_DISABLED), file_exists(F_BUSY))) {
        case AV_BOOT_EXIT_DISABLED:
            logf_("disabled file present; exiting");
            status_("disabled", "%s", "");
            goto out;
        case AV_BOOT_EXIT_STALE_MARKER:
            logf_("stale busy marker from an interrupted run; creating disabled and exiting");
            write_text(F_DISABLED, "auto: interrupted run\n");
            remove_file(F_BUSY);
            status_("disabled", "%s", "cause=stale-busy-marker");
            goto out;
        case AV_BOOT_RUN:
            break;
    }

    write_text(F_BUSY, "init\n");
    if (!init_services()) {
        logf_("init failed; exiting without acting");
        remove_file(F_BUSY);
        goto out;
    }
    nsx_snap_t s0;
    log_snapshot("start", AV_SSBU_PROGRAM_ID, &s0);
    if (R_FAILED(s0.term_rc)) {
        logf_("start: GetApplicationTerminateResult unavailable; exiting without acting");
        remove_file(F_BUSY);
        goto out;
    }
    remove_file(F_BUSY);
    u32 sv0 = 0;
    char st0[32];
    strike_text(read_strike(&sv0), sv0, st0, sizeof(st0));
    logf_("start: strike=%s ns_event=%d poll_ms=%u", st0, g_ns_event_ok, AV_POLL_MS);
    status_("idle", "id=%016llx term=0x%x flags=0x%08x strike=%s", (unsigned long long)AV_SSBU_PROGRAM_ID, s0.term,
            R_SUCCEEDED(s0.view_rc) ? s0.view.flags : 0, st0);

    bool trigger_pending = false;
    u64 trigger_id = AV_SSBU_PROGRAM_ID;
    u64 fast_until = 0;

    for (;;) {
        if (file_exists(F_DISABLED)) {
            logf_("disabled file appeared; exiting");
            status_("disabled", "%s", "cause=file");
            break;
        }

        evaluate_clear(AV_SSBU_PROGRAM_ID);
        if (g_sess_exit_ms != 0 && now_ms() - g_sess_exit_ms >= AV_FAST_WINDOW_MS) {
            judge_session();
        }

        if (!trigger_pending && file_exists(F_TRIGGER)) {
            char buf[64];
            memset(buf, 0, sizeof(buf));
            FILE *tf = fopen(F_TRIGGER, "r");
            if (tf) {
                if (fgets(buf, sizeof(buf), tf) == NULL) {
                    buf[0] = '\0';
                }
                fclose(tf);
            }
            remove_file(F_TRIGGER);
            trigger_id = av_parse_trigger_id(buf);
            trigger_pending = true;
            logf_("verify-now consumed id=%016llx", (unsigned long long)trigger_id);
        }
        if (trigger_pending) {
            if (!av_title_allowed(trigger_id) || g_poisoned || g_verifies >= AV_MAX_VERIFIES_PER_RUN) {
                logf_("verify-now refused id=%016llx poisoned=%d verifies=%u", (unsigned long long)trigger_id,
                      g_poisoned, g_verifies);
                trigger_pending = false;
            } else if (!query_app_running()) {
                trigger_pending = false;
                if (file_exists(F_OBSERVE)) {
                    logf_("verify-now: observe-only, not verifying");
                } else {
                    run_verify(trigger_id);
                }
            }
        }

        const u64 t = now_ms();
        const u64 wait_ms = t < fast_until ? AV_FAST_SLICE_MS : AV_POLL_MS;
        s32 idx = -1;
        Result wr = g_ns_event_ok ? waitMulti(&idx, wait_ms * 1000000ULL, waiterForEvent(&g_pgl_event),
                                              waiterForEvent(&g_ns_event))
                                  : waitMulti(&idx, wait_ms * 1000000ULL, waiterForEvent(&g_pgl_event));
        if (R_SUCCEEDED(wr) && idx == 0) {
            bool exited = false, started = false;
            drain_pgl(&exited, &started);
            if (exited) {
                fast_until = now_ms() + AV_FAST_WINDOW_MS; /* ns/am record the result shortly after the exit */
            }
        } else if (R_SUCCEEDED(wr) && idx == 1) {
            /* An ns record changed: let ns finish the update; this also bounds ns wake-ups to one per second. */
            svcSleepThread((u64)AV_FAST_SLICE_MS * 1000000ULL);
        } else if (R_FAILED(wr) && wr != KERNELRESULT(TimedOut)) {
            logf_("wait failed rc=0x%x; exiting", wr);
            break;
        }
    }

out:
    logf_("ns-autoverify exit");
    fini_services();
    fsdevUnmountAll();
    fsExit();
    return 0;
}
