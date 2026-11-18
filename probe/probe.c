/*
 * nsav-probe: find out exactly which ns state drives SSBU's "Corrupted data has been detected" dialog, and whether
 * ClearApplicationTerminateResult (ns cmd 57) removes it. Run from hbmenu (Album / applet mode).
 *
 *   A  dump (read-only): ApplicationView raw + flags, terminate result (2100), raw terminate result (2101),
 *      IsDataCorruptedResult on both (ns:ro 2), the application record; plus a reference classification table.
 *   X  SET: SetApplicationTerminateResult(SSBU, 0x4A8) (cmd 56), only if the current terminate result is 0; then dump.
 *   Y  CLEAR: ClearApplicationTerminateResult(SSBU) (cmd 57); then dump.
 *   ZL SET: SetApplicationTerminateResult(SSBU, 0x1FD602) (the hw6 Rom hash-tree value), only if 2100 and 2101 both
 *      read 0; then dump. Test-only (it raises the corrupt-data state); needs its own go.
 *   +  exit.
 *
 * Every line also goes to sdmc:/atmosphere/logs/nsav-probe.log (appended; never rotated, a run is ~10 lines).
 * The probe writes ns state only on X, ZL and Y, only for SSBU, and only the terminate result.
 */
#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nsx.h"
#include "policy.h"

#define PROBE_VERSION "0.4.0"
#define LOG_DIR "sdmc:/atmosphere/logs"
#define LOG_PATH LOG_DIR "/nsav-probe.log"

static u64 g_seq;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* Screen: wrap long lines at 78 columns. */
    const size_t n = strlen(line);
    for (size_t i = 0; i < n; i += 78) {
        printf("%.78s\n", line + i);
    }

    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
        fprintf(f, "[%llu.%03llu #%llu] %s\n", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000),
                (unsigned long long)g_seq, line);
        fclose(f);
    }
}

static nsx_snap_t dump(const char *tag) {
    nsx_snap_t s;
    nsx_snapshot(AV_SSBU_PROGRAM_ID, &s);
    char line[640];
    nsx_format(&s, AV_SSBU_PROGRAM_ID, line, sizeof(line));
    out("dump[%s] %s", tag, line);
    return s;
}

/* ZR: one bounded experiment. RequestVerifyApplication with the u32 scope argument 0 instead of qlaunch's 0x7: does a
 * zero-scope verify finish quickly AND clear the post-crash state? Read-only verification; B cancels; 20 min cap. */
static void verify_experiment(PadState *pad) {
    out("VERIFY-EXP RequestVerifyApplication(SSBU, scope=0x0, tmem=0x%x); B cancels", AV_VERIFY_TMEM_SIZE);
    void *buf = aligned_alloc(0x1000, AV_VERIFY_TMEM_SIZE);
    if (buf == NULL) {
        out("VERIFY-EXP no memory for the 0x%x buffer", AV_VERIFY_TMEM_SIZE);
        return;
    }
    NsProgressAsyncResult a;
    memset(&a, 0, sizeof(a));
    const u64 start = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    Result rc = nsRequestVerifyApplication(&a, AV_SSBU_PROGRAM_ID, 0x0, buf, AV_VERIFY_TMEM_SIZE);
    out("VERIFY-EXP request rc=0x%x", rc);
    if (R_FAILED(rc)) {
        free(buf);
        return;
    }
    bool cancelled = false;
    u64 last = start;
    for (;;) {
        if (R_SUCCEEDED(nsProgressAsyncResultWait(&a, 100000000ULL))) {
            break;
        }
        const u64 t = armTicksToNs(armGetSystemTick()) / 1000000ULL;
        padUpdate(pad);
        const bool b = (padGetButtonsDown(pad) & HidNpadButton_B) != 0;
        if (!cancelled && (b || !appletMainLoop() || t - start > 20ULL * 60 * 1000)) {
            nsProgressAsyncResultCancel(&a);
            cancelled = true;
            out("VERIFY-EXP cancel requested after %llu ms", (unsigned long long)(t - start));
        }
        if (t - last >= 5000) {
            last = t;
            NsSystemUpdateProgress p;
            memset(&p, 0, sizeof(p));
            nsProgressAsyncResultGetProgress(&a, &p, sizeof(p));
            out("VERIFY-EXP progress %lld/%lld after %llu ms", (long long)p.current_size, (long long)p.total_size,
                (unsigned long long)(t - start));
        }
        consoleUpdate(NULL);
    }
    const Result get_rc = nsProgressAsyncResultGet(&a);
    const Result detail_rc = nsProgressAsyncResultGetDetailResult(&a);
    nsProgressAsyncResultClose(&a);
    free(buf);
    const u64 t = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    out("VERIFY-EXP done get_rc=0x%x detail_rc=0x%x cancelled=%d elapsed_ms=%llu", get_rc, detail_rc,
        cancelled ? 1 : 0, (unsigned long long)(t - start));
}

static void reference_table(void) {
    /* How ns classifies known results: 0, SSBU's Data Abort, and three fs data-corruption results
     * (module 2, descriptions 4000, 4602 and 4075). Validates the IsDataCorruptedResult request layout. */
    static const u32 refs[] = {0x0u, AV_PROBE_RESULT, 0x1F4002u, 0x23F402u, AV_FAST_PROBE_RESULT};
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        bool corrupt = false;
        Result rc = nsx_is_data_corrupted(refs[i], &corrupt);
        out("classify result=0x%x (%04u-%04u) rc=0x%x corrupt=%d", refs[i], 2000 + R_MODULE(refs[i]),
            R_DESCRIPTION(refs[i]), rc, R_SUCCEEDED(rc) ? (corrupt ? 1 : 0) : -1);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    mkdir("sdmc:/atmosphere", 0777);
    mkdir(LOG_DIR, 0777);

    out("nsav-probe %s  SSBU %016llx", PROBE_VERSION, (unsigned long long)AV_SSBU_PROGRAM_ID);
    out("A=dump  X=set 0x%x (only if 0)  ZL=set 0x%x (only if both 0)  Y=clear (cmd 57)  ZR=verify scope 0 (B "
        "cancels)  +=exit",
        AV_PROBE_RESULT, AV_FAST_PROBE_RESULT);

    Result rc = nsInitialize();
    out("hos=%u.%u.%u nsInitialize rc=0x%x", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
        HOSVER_MICRO(hosversionGet()), rc);
    const bool ns_ok = R_SUCCEEDED(rc);
    if (ns_ok) {
        reference_table();
        dump("start");
    }
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) {
            break;
        }
        if (ns_ok && (down & HidNpadButton_A)) {
            g_seq++;
            dump("A");
        } else if (ns_ok && (down & HidNpadButton_X)) {
            g_seq++;
            Result cur = 0;
            Result grc = nsGetApplicationTerminateResult(AV_SSBU_PROGRAM_ID, &cur);
            if (R_FAILED(grc)) {
                out("SET refused: GetApplicationTerminateResult rc=0x%x", grc);
            } else if (cur != 0) {
                out("SET refused: terminate result is already 0x%x (the real state is present; use Y)", cur);
            } else {
                Result src = nsSetApplicationTerminateResult(AV_SSBU_PROGRAM_ID, AV_PROBE_RESULT);
                out("SET SetApplicationTerminateResult(0x%x) rc=0x%x", AV_PROBE_RESULT, src);
                dump("after-set");
            }
        } else if (ns_ok && (down & HidNpadButton_ZL)) {
            g_seq++;
            Result cur = 0;
            u32 raw = 0;
            Result grc = nsGetApplicationTerminateResult(AV_SSBU_PROGRAM_ID, &cur);
            Result rrc = nsx_get_raw_terminate_result(AV_SSBU_PROGRAM_ID, &raw);
            if (R_FAILED(grc) || R_FAILED(rrc) || cur != 0 || raw != 0) {
                out("SET-ZL refused: term rc=0x%x 0x%x raw rc=0x%x 0x%x (both must read 0)", grc, cur, rrc, raw);
            } else {
                Result src = nsSetApplicationTerminateResult(AV_SSBU_PROGRAM_ID, AV_FAST_PROBE_RESULT);
                out("SET-ZL SetApplicationTerminateResult(0x%x) rc=0x%x", AV_FAST_PROBE_RESULT, src);
                dump("after-set-zl");
            }
        } else if (ns_ok && (down & HidNpadButton_Y)) {
            g_seq++;
            Result crc = nsClearApplicationTerminateResult(AV_SSBU_PROGRAM_ID);
            out("CLEAR ClearApplicationTerminateResult rc=0x%x", crc);
            dump("after-clear");
        } else if (ns_ok && (down & HidNpadButton_ZR)) {
            g_seq++;
            verify_experiment(&pad);
            dump("after-verify-exp");
        }
        consoleUpdate(NULL);
    }

    out("exit");
    if (ns_ok) {
        nsExit();
    }
    consoleExit(NULL);
    return 0;
}
