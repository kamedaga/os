#include "common.h"

filed_page_dispatch_result_t filed_dispatch_statat_page(filed_runtime_t *runtime, void *page)
{
    filed_statat_t *request = page;
    if (runtime == NULL || request == NULL) return filed_page_result(-22, 0);
    memset(&request->stat, 0, sizeof(request->stat));
    if ((request->flags & ~((uint64_t)FILED_STATAT_NOFOLLOW)) != 0 ||
        !filed_name_is_terminated(request->name, sizeof(request->name)))
        return filed_page_result(-22, 0);
    if (request->name[0] == '\0') return filed_page_result(-2, 0);
    if (request->name[0] != '/' && request->dir_handle > UINT32_MAX)
        return filed_page_result(-9, 0);

    /* Reuse the authoritative path walker (mounts, dot/dotdot, symlinks and
     * directory capabilities). The walk's private handle never crosses IPC;
     * all cleanup completes before the single metadata response is sent. */
    filed_openat_t openat;
    filed_vfs_open_result_t opened;
    memset(&openat, 0, sizeof(openat));
    memset(&opened, 0, sizeof(opened));
    openat.dir_handle = request->dir_handle;
    openat.rights = FILED_RIGHT_STAT;
    openat.open_flags = (request->flags & FILED_STATAT_NOFOLLOW) ? FILED_OPEN_NOFOLLOW : 0;
    memcpy(openat.name, request->name, sizeof(openat.name));
    const int64_t status = filed_openat_path(runtime, &openat, &opened);
    if (status != 0) return filed_page_result(status, 0);
    request->stat.handle = opened.handle_id;
    filed_page_dispatch_result_t result = filed_dispatch_stat_page(runtime, &request->stat);
    const int64_t close_status = filed_close_handle_runtime(runtime, opened.handle_id);
    if (result.status == 0 && close_status != 0) result = filed_page_result(close_status, 0);
    if (result.status != 0) memset(&request->stat, 0, sizeof(request->stat));
    request->stat.handle = 0;
    return result;
}
