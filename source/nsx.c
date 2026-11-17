/* ns helpers shared by the sysmodule and the probe NRO (libnx). */
#include "nsx.h"

#include <stdio.h>
#include <string.h>

Result nsx_is_data_corrupted(Result value, bool *out) {
    Service srv;
    memset(&srv, 0, sizeof(srv));
    Result rc = nsGetReadOnlyApplicationRecordInterface(&srv);
    if (R_FAILED(rc)) {
        return rc;
    }
    u8 flag = 0;
    rc = serviceDispatchInOut(&srv, 2, value, flag);
    serviceClose(&srv);
    if (R_SUCCEEDED(rc)) {
        *out = flag != 0;
    }
    return rc;
}

Result nsx_get_raw_terminate_result(u64 application_id, u32 *out) {
    Service srv;
    memset(&srv, 0, sizeof(srv));
    Result rc = nsGetApplicationManagerInterface(&srv);
    if (R_FAILED(rc)) {
        return rc;
    }
    u32 value = 0;
    rc = serviceDispatchInOut(&srv, 2101, application_id, value);
    serviceClose(&srv);
    if (R_SUCCEEDED(rc)) {
        *out = value;
    }
    return rc;
}

void nsx_snapshot(u64 application_id, nsx_snap_t *s) {
    memset(s, 0, sizeof(*s));
    s->view_rc = nsGetApplicationView(&s->view, &application_id, 1);
    s->term_rc = nsGetApplicationTerminateResult(application_id, &s->term);
    s->raw_rc = nsx_get_raw_terminate_result(application_id, &s->raw);
    s->term_class_rc = R_SUCCEEDED(s->term_rc) ? nsx_is_data_corrupted(s->term, &s->term_corrupt) : s->term_rc;
    s->raw_class_rc = R_SUCCEEDED(s->raw_rc) ? nsx_is_data_corrupted(s->raw, &s->raw_corrupt) : s->raw_rc;

    /* Bounded scan of the application records (16 per call, at most 64 calls). */
    NsApplicationRecord recs[16];
    s32 offset = 0;
    for (int calls = 0; calls < 64 && !s->rec_found; calls++) {
        s32 count = 0;
        if (R_FAILED(nsListApplicationRecord(recs, 16, offset, &count)) || count <= 0) {
            break;
        }
        for (s32 i = 0; i < count; i++) {
            if (recs[i].application_id == application_id) {
                s->rec = recs[i];
                s->rec_found = true;
                break;
            }
        }
        offset += count;
    }
}

void nsx_format(const nsx_snap_t *s, u64 application_id, char *out, size_t out_size) {
    char hex[2 * sizeof(NsApplicationView) + 1];
    hex[0] = '\0';
    if (R_SUCCEEDED(s->view_rc)) {
        const u8 *b = (const u8 *)&s->view;
        for (size_t i = 0; i < sizeof(NsApplicationView); i++) {
            snprintf(hex + 2 * i, 3, "%02x", b[i]);
        }
    }
    snprintf(out, out_size,
             "id=%016llx view_rc=0x%x flags=0x%08x term_rc=0x%x term=0x%x term_class_rc=0x%x term_corrupt=%d "
             "raw_rc=0x%x raw=0x%x raw_class_rc=0x%x raw_corrupt=%d rec=%d last_event=%u attributes=0x%02x "
             "last_updated=%llu view=%s",
             (unsigned long long)application_id, s->view_rc, R_SUCCEEDED(s->view_rc) ? s->view.flags : 0, s->term_rc,
             R_SUCCEEDED(s->term_rc) ? s->term : 0, s->term_class_rc, s->term_corrupt ? 1 : 0, s->raw_rc,
             R_SUCCEEDED(s->raw_rc) ? s->raw : 0, s->raw_class_rc, s->raw_corrupt ? 1 : 0, s->rec_found ? 1 : 0,
             s->rec_found ? (unsigned)s->rec.last_event : 0u, s->rec_found ? (unsigned)s->rec.attributes : 0u,
             s->rec_found ? (unsigned long long)s->rec.last_updated : 0ULL, hex);
}
