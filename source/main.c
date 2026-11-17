/*
 * ns-autoverify: after Super Smash Bros. Ultimate crashes, run the SAME official content verification that
 * qlaunch's "Check" button runs (ns IApplicationManagerInterface cmd 1003 RequestVerifyApplication, flags 0x7,
 * 0x100000-byte TransferMemory), so the post-crash "Corrupted data has been detected" state is cleared by ns
 * exactly as a manual check would clear it. Real corruption still surfaces: a failed verify changes nothing.
 *
 * Fail-safe rules: no fatalThrow / diagAbort anywhere; any init error or unknown firmware -> log (if the SD is
 * up) and exit; a stale busy marker from an interrupted run -> create `disabled` and exit; every wait is bounded.
 */
#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "policy.h"

/* The pgl shell-event observer API (pgl cmd GetShellEventObserver) has two spellings across libnx versions.
 * The console build uses libnx 4.12.0's names; newer libnx sources (e.g. b156b477, used by `make check-host`)
 * renamed them. Same IPC either way. */
#ifdef AV_LIBNX_NEW_PGL_NAMES
#define av_pglGetObserver      pglCreateShellEvent
#define av_pglObserverEvent    pglEventObserverGetShellEvent
#define av_pglObserverEventInfo pglEventObserverGetShellEventInfo
#else
#define av_pglGetObserver      pglGetEventObserver
#define av_pglObserverEvent    pglEventObserverGetProcessEvent
#define av_pglObserverEventInfo pglEventObserverGetProcessEventInfo
#endif

#define AV_VERSION "0.1.1"

#define CFG_DIR    "sdmc:/config/ns-autoverify"
#define LOG_DIR    "sdmc:/atmosphere/logs"
#define LOG_PATH   LOG_DIR "/ns-autoverify.log"
#define LOG_OLD    LOG_DIR "/ns-autoverify.log.1"
#define F_DISABLED CFG_DIR "/disabled"
#define F_OBSERVE  CFG_DIR "/observe-only"
#define F_CLEAR    CFG_DIR "/clear-terminate-result"
#define F_TRIGGER  CFG_DIR "/verify-now"
#define F_BUSY     CFG_DIR "/busy"
#define F_STATUS   CFG_DIR "/status.txt"

/* We are a background sysmodule, not an applet. Keep the process small: boot2 aborts the boot when an SD
 * sysmodule cannot be created for lack of memory. The 2 MiB verify buffer is taken with svcSetHeapSize only
 * while a verify runs. */
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

/* Nothing at process start: every service comes up in main(), where failures are handled without aborting. */
void __appInit(void) {}
void __appExit(void) {}

static bool g_sd;
static bool g_sm, g_pmdmnt, g_pgl, g_ns, g_observer;
static PglEventObserver g_obs;
static Event g_pgl_event;
static av_tracker_t g_track;

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

/* One line, overwritten on every state change; the hardware driver reads it to know when it may launch. */
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

typedef struct {
    bool view_ok;
    NsApplicationView view;
    bool term_ok;
    Result term;
    bool rec_ok;
    NsApplicationRecord rec;
} snap_t;

static void take_snapshot(u64 program_id, snap_t *s, const char *tag) {
    memset(s, 0, sizeof(*s));

    Result rc = nsGetApplicationView(&s->view, &program_id, 1);
    s->view_ok = R_SUCCEEDED(rc);
    char hex[2 * sizeof(NsApplicationView) + 1];
    hex[0] = '\0';
    if (s->view_ok) {
        const u8 *b = (const u8 *)&s->view;
        for (size_t i = 0; i < sizeof(NsApplicationView); i++) {
            snprintf(hex + 2 * i, 3, "%02x", b[i]);
        }
    }

    Result trc = nsGetApplicationTerminateResult(program_id, &s->term);
    s->term_ok = R_SUCCEEDED(trc);

    /* Bounded scan of the application records (16 per call, at most 64 calls). */
    NsApplicationRecord recs[16];
    for (s32 offset = 0, calls = 0; calls < 64 && !s->rec_ok; calls++) {
        s32 count = 0;
        if (R_FAILED(nsListApplicationRecord(recs, 16, offset, &count)) || count <= 0) {
            break;
        }
        for (s32 i = 0; i < count; i++) {
            if (recs[i].application_id == program_id) {
                s->rec = recs[i];
                s->rec_ok = true;
                break;
            }
        }
        offset += count;
    }

    logf_("snapshot[%s] id=%016llx view_rc=0x%x flags=0x%08x view=%s term_rc=0x%x term=0x%x rec=%d last_event=%u "
          "attributes=0x%02x",
          tag, (unsigned long long)program_id, rc, s->view_ok ? s->view.flags : 0, hex, trc,
          s->term_ok ? s->term : 0, s->rec_ok ? 1 : 0, s->rec_ok ? s->rec.last_event : 0,
          s->rec_ok ? s->rec.attributes : 0);
}

static bool query_app_running(u64 *out_pid) {
    u64 pid = 0;
    if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0) {
        if (out_pid) {
            *out_pid = pid;
        }
        return true;
    }
    return false;
}

/* Drain pgl shell events into the tracker. Returns true if an application process started. */
static bool drain_pgl(void) {
    bool app_started = false;
    for (u32 n = 0; n < AV_MAX_EVENTS_PER_WAKE; n++) {
        PmProcessEventInfo info;
        memset(&info, 0, sizeof(info));
        if (R_FAILED(av_pglObserverEventInfo(&g_obs, &info)) || info.event == PmProcessEvent_None) {
            break;
        }
        switch (info.event) {
            case PmProcessEvent_Start: {
                u64 app_pid = 0;
                if (query_app_running(&app_pid) && app_pid == info.process_id) {
                    u64 program_id = 0;
                    pmdmntGetProgramId(&program_id, info.process_id);
                    av_track_app_start(&g_track, info.process_id, program_id);
                    app_started = true;
                    logf_("event start application pid=%llu id=%016llx", (unsigned long long)info.process_id,
                          (unsigned long long)program_id);
                }
                break;
            }
            case PmProcessEvent_Crash:
                if (g_track.app_running && info.process_id == g_track.app_pid) {
                    logf_("event crash application pid=%llu id=%016llx", (unsigned long long)info.process_id,
                          (unsigned long long)g_track.app_program_id);
                }
                av_track_crash(&g_track, info.process_id);
                break;
            case PmProcessEvent_Exit:
                if (g_track.app_running && info.process_id == g_track.app_pid) {
                    logf_("event exit application pid=%llu crashed=%d", (unsigned long long)info.process_id,
                          g_track.crash_seen ? 1 : 0);
                }
                av_track_exit(&g_track, info.process_id, now_ms());
                break;
            default:
                break;
        }
    }
    return app_started;
}

static bool g_poisoned;
static u32 g_verifies;

static av_verdict_t run_verify(u64 program_id, av_reason_t reason) {
    snap_t before, after;
    take_snapshot(program_id, &before, "before");

    write_text(F_BUSY, "verify\n");
    status_("verifying", "id=%016llx reason=%s permille=0", (unsigned long long)program_id, av_reason_name(reason));
    g_verifies++;

    void *heap = NULL;
    Result hrc = svcSetHeapSize(&heap, AV_HEAP_GRANULE);
    if (R_FAILED(hrc) || heap == NULL) {
        logf_("verify: heap allocation failed rc=0x%x; not verifying", hrc);
        remove_file(F_BUSY);
        status_("error", "id=%016llx reason=%s detail=heap rc=0x%x", (unsigned long long)program_id,
                av_reason_name(reason), hrc);
        return AV_V_ERROR;
    }

    NsProgressAsyncResult a;
    memset(&a, 0, sizeof(a));
    Result rrc = nsRequestVerifyApplication(&a, program_id, AV_VERIFY_FLAGS, heap, AV_VERIFY_TMEM_SIZE);
    logf_("verify: RequestVerifyApplication(id=%016llx, 0x%x, 0x%x) rc=0x%x", (unsigned long long)program_id,
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
                if (drain_pgl() && !cancelled && !timed_out) {
                    logf_("verify: an application started; cancelling (the flag stays, which is safe)");
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
                svcSleepThread((u64)AV_VERIFY_SLICE_MS * 1000000ULL); /* never spin on a failing wait */
            }
            if (!cancelled && !timed_out && t - start > AV_VERIFY_MAX_MS) {
                logf_("verify: exceeded %u ms; cancelling", AV_VERIFY_MAX_MS);
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
                Result prc = nsProgressAsyncResultGetProgress(&a, &p, sizeof(p));
                const int pm = av_progress_permille(p.current_size, p.total_size);
                logf_("verify: progress rc=0x%x current=%lld total=%lld permille=%d", prc,
                      (long long)p.current_size, (long long)p.total_size, pm);
                status_("verifying", "id=%016llx reason=%s permille=%d", (unsigned long long)program_id,
                        av_reason_name(reason), pm);
            }
        }
        if (done) {
            get_rc = nsProgressAsyncResultGet(&a);
            detail_rc = nsProgressAsyncResultGetDetailResult(&a);
            nsProgressAsyncResultClose(&a);
        } else {
            /* ns never signalled after Cancel: do not block in nsProgressAsyncResultClose (it waits forever).
             * Drop our handles and never touch the buffer again. */
            serviceClose(&a.s);
            eventClose(&a.event);
        }
        logf_("verify: done=%d cancelled=%d timed_out=%d leaked=%d get_rc=0x%x detail_rc=0x%x elapsed_ms=%llu",
              done, cancelled, timed_out, leaked, get_rc, detail_rc, (unsigned long long)(now_ms() - start));
    }

    if (leaked) {
        g_poisoned = true;
        logf_("verify: buffer still owned by ns; no further verifies this run");
    } else {
        /* ns may still be unmapping its TransferMemory view for a moment after signalling. */
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
            logf_("verify: heap release failed rc=0x%x; no further verifies this run", frc);
        }
    }

    const av_verdict_t v = av_verdict(R_SUCCEEDED(rrc), get_rc, detail_rc, cancelled, timed_out);
    take_snapshot(program_id, &after, "after");

    bool cleared = false;
    if (av_should_clear(v, file_exists(F_CLEAR), after.term_ok ? after.term : 0)) {
        Result crc = nsClearApplicationTerminateResult(program_id);
        cleared = R_SUCCEEDED(crc);
        logf_("verify: ClearApplicationTerminateResult rc=0x%x", crc);
        snap_t cleared_snap;
        take_snapshot(program_id, &cleared_snap, "after-clear");
    }

    remove_file(F_BUSY);
    logf_("verify: verdict=%s (a failed verify changes nothing; the HOME dialog stays for real corruption)",
          av_verdict_name(v));
    status_(av_verdict_name(v),
            "id=%016llx reason=%s request_rc=0x%x get_rc=0x%x detail_rc=0x%x term_before=0x%x term_after=0x%x "
            "flags_before=0x%08x flags_after=0x%08x cleared=%d",
            (unsigned long long)program_id, av_reason_name(reason), rrc, get_rc, detail_rc,
            before.term_ok ? before.term : 0, after.term_ok ? after.term : 0, before.view_ok ? before.view.flags : 0,
            after.view_ok ? after.view.flags : 0, cleared ? 1 : 0);
    return v;
}

/* Returns true if the caller should keep the reason pending (deferred). */
static bool consider(av_reason_t reason, u64 program_id) {
    av_gate_in_t in = {
        .observe_only = file_exists(F_OBSERVE),
        .poisoned = g_poisoned,
        .app_running = query_app_running(NULL),
        .verifies_done = g_verifies,
    };
    const av_gate_t g = av_gate(&in, reason, program_id);
    static av_gate_t last_gate = AV_GATE_REFUSE;
    static av_reason_t last_reason = AV_REASON_NONE;
    if (!(g == AV_GATE_DEFER && last_gate == AV_GATE_DEFER && last_reason == reason)) {
        logf_("decision reason=%s id=%016llx gate=%s observe_only=%d app_running=%d verifies=%u poisoned=%d",
              av_reason_name(reason), (unsigned long long)program_id, av_gate_name(g), in.observe_only,
              in.app_running, in.verifies_done, in.poisoned);
    }
    last_gate = g;
    last_reason = reason;
    switch (g) {
        case AV_GATE_GO:
            run_verify(program_id, reason);
            return false;
        case AV_GATE_OBSERVE: {
            snap_t s;
            take_snapshot(program_id, &s, "observe");
            status_("observed", "id=%016llx reason=%s would_verify=1", (unsigned long long)program_id,
                    av_reason_name(reason));
            return false;
        }
        case AV_GATE_DEFER:
            return true;
        case AV_GATE_REFUSE:
            return false;
    }
    return false;
}

/* sm is already up (main() needs it for the SD). */
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

    /* SD first, so every later failure is logged. Without the SD we cannot honour the kill switch: exit. */
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
            /* The previous run stopped during init or a verify (crash, fatal or power loss). Never retry
             * automatically: that is how a boot loop starts. The operator deletes `disabled` to re-arm. */
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

    av_track_init(&g_track);
    u64 app_pid = 0;
    if (query_app_running(&app_pid)) {
        u64 program_id = 0;
        pmdmntGetProgramId(&program_id, app_pid);
        av_track_app_start(&g_track, app_pid, program_id);
        logf_("start: application already running pid=%llu id=%016llx", (unsigned long long)app_pid,
              (unsigned long long)program_id);
    }

    snap_t s0;
    take_snapshot(AV_SSBU_PROGRAM_ID, &s0, "start");
    if (!s0.term_ok) {
        logf_("start: ns GetApplicationTerminateResult unavailable; exiting without acting");
        remove_file(F_BUSY);
        goto out;
    }
    remove_file(F_BUSY); /* init survived: from here on, only a verify sets the marker */
    status_("idle", "id=%016llx term=0x%x flags=0x%08x", (unsigned long long)AV_SSBU_PROGRAM_ID, s0.term,
            s0.view_ok ? s0.view.flags : 0);

    /* Start-up pass: a flag may predate this run (crash before boot, or Stage 1 manual start). */
    bool start_pending = s0.term != 0;
    bool trigger_pending = false;
    u64 trigger_id = AV_SSBU_PROGRAM_ID;

    for (;;) {
        if (file_exists(F_DISABLED)) {
            logf_("disabled file appeared; exiting");
            status_("disabled", "%s", "cause=file");
            break;
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
            remove_file(F_TRIGGER); /* one-shot: consumed once, kept pending in memory while deferred */
            trigger_id = av_parse_trigger_id(buf);
            trigger_pending = true;
            logf_("trigger file consumed id=%016llx", (unsigned long long)trigger_id);
        }

        u64 due_id = 0;
        if (av_track_due(&g_track, now_ms(), &due_id)) {
            if (!consider(AV_REASON_CRASH, due_id)) {
                av_track_consume(&g_track);
                start_pending = false; /* the same flag episode */
                trigger_pending = false;
            }
        } else if (start_pending) {
            start_pending = consider(AV_REASON_TERMINATE_RESULT, AV_SSBU_PROGRAM_ID);
        } else if (trigger_pending) {
            trigger_pending = consider(AV_REASON_TRIGGER_FILE, trigger_id);
        }

        /* Sleep until a pgl event or the poll interval; shorter while a crash is settling. */
        u64 wait_ms = AV_POLL_MS;
        if (g_track.crash_seen && g_track.crash_exited) {
            wait_ms = AV_SETTLE_MS;
        }
        s32 idx = -1;
        Result wr = waitMulti(&idx, wait_ms * 1000000ULL, waiterForEvent(&g_pgl_event));
        if (R_SUCCEEDED(wr) && idx == 0) {
            drain_pgl();
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
