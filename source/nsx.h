/* ns helpers shared by the sysmodule and the probe NRO (libnx). Callers must have run nsInitialize(). */
#pragma once

#include <switch.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Result view_rc;
    NsApplicationView view;
    Result term_rc;          /* GetApplicationTerminateResult, IApplicationManagerInterface 2100 */
    Result term;
    Result raw_rc;           /* GetRawApplicationTerminateResult, IApplicationManagerInterface 2101 */
    u32 raw;
    Result term_class_rc;    /* IsDataCorruptedResult(term), IReadOnlyApplicationRecordInterface 2 */
    bool term_corrupt;
    Result raw_class_rc;     /* IsDataCorruptedResult(raw) */
    bool raw_corrupt;
    bool rec_found;
    NsApplicationRecord rec;
} nsx_snap_t;

/* IReadOnlyApplicationRecordInterface (ns getter 7991) cmd 2 [10.0.0+]: in u32 Result, out u8 bool. */
Result nsx_is_data_corrupted(Result value, bool *out);

/* IApplicationManagerInterface cmd 2101 [6.0.0+]: in u64 application id, out u32. */
Result nsx_get_raw_terminate_result(u64 application_id, u32 *out);

/* Reads everything above for one application. Only ns reads; never changes ns state. */
void nsx_snapshot(u64 application_id, nsx_snap_t *s);

/* One log line (no newline) describing a snapshot. */
void nsx_format(const nsx_snap_t *s, u64 application_id, char *out, size_t out_size);
