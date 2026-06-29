#include "filed/exec_linux_lpr.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacha/ipc.h"

static int load_plan(filed_runtime_t *runtime, const lpr_exec_image_t *main_image, lpr_exec_plan_t *plan)
{
    lpr_exec_image_t lpr_image;
    lpr_exec_image_t interp_image;
    lpr_exec_loaded_t lpr_loaded;
    lpr_exec_loaded_t main_loaded;
    lpr_exec_loaded_t interp_loaded;
    char interp_path[LPR_EXEC_MAX_INTERP_BYTES];
    uint64_t syscall_entry_offset = 0;

    if (runtime == NULL || main_image == NULL || plan == NULL) {
        return -22;
    }
    memset(plan, 0, sizeof(*plan));
    memset(&lpr_image, 0, sizeof(lpr_image));
    memset(&interp_image, 0, sizeof(interp_image));
    plan->process_fd = -1;
    plan->thread_fd = -1;

    int status = lpr_exec_validate_elf(main_image);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_get_interp_path(main_image, interp_path, sizeof(interp_path));
    if (status != 0) {
        return status;
    }
    status = lpr_exec_read_absolute_image(runtime, LPR_EXEC_RUNTIME_PATH, &lpr_image);
    if (status != 0) {
        fprintf(stderr, "[filed] linux-lpr: runtime missing path=%s status=%d\n", LPR_EXEC_RUNTIME_PATH, status);
        return status;
    }
    status = lpr_exec_image_find_symbol(&lpr_image, "lpr_syscall_entry", &syscall_entry_offset);
    if (status != 0 || syscall_entry_offset == 0) {
        free(lpr_image.bytes);
        return status != 0 ? status : -8;
    }

    const uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_MAP_INTO |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int process_fd = pacha_process_create(process_rights, 0);
    if (process_fd < 16) {
        free(lpr_image.bytes);
        return -12;
    }
    plan->process_fd = process_fd;

    status = lpr_exec_load_image_into_process(process_fd, &lpr_image, LPR_EXEC_LPR_BASE, 0, &lpr_loaded);
    free(lpr_image.bytes);
    if (status != 0) {
        lpr_exec_discard_process_fd(process_fd);
        return status;
    }
    status = lpr_exec_install_low_layout(process_fd, lpr_loaded.base + syscall_entry_offset);
    if (status != 0) {
        lpr_exec_discard_process_fd(process_fd);
        return status;
    }
    status = lpr_exec_load_image_into_process(process_fd, main_image, LPR_EXEC_MAIN_DYN_BASE, 1, &main_loaded);
    if (status != 0) {
        lpr_exec_discard_process_fd(process_fd);
        return status;
    }

    plan->main_entry = main_loaded.entry;
    plan->runtime_entry = main_loaded.entry;
    plan->interpreter_base = 0;
    plan->phdr_va = main_loaded.phdr_va;
    plan->phent = main_loaded.phent;
    plan->phnum = main_loaded.phnum;

    if (interp_path[0] != '\0') {
        status = lpr_exec_read_absolute_image(runtime, interp_path, &interp_image);
        if (status != 0) {
            lpr_exec_discard_process_fd(process_fd);
            return status;
        }
        status = lpr_exec_load_image_into_process(process_fd, &interp_image, LPR_EXEC_INTERP_DYN_BASE, 1, &interp_loaded);
        free(interp_image.bytes);
        if (status != 0) {
            lpr_exec_discard_process_fd(process_fd);
            return status;
        }
        plan->runtime_entry = interp_loaded.entry;
        plan->interpreter_base = interp_loaded.base;
    }
    return 0;
}

int filed_exec_linux_lpr_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd)
{
    lpr_exec_image_t image;
    lpr_exec_plan_t plan;
    int prepared[FILED_WIRE_EXEC_MAX_INHERIT_FDS + 1];
    uint64_t prepared_count = 0;

    if (out_process_fd != NULL) *out_process_fd = -1;
    if (out_thread_fd != NULL) *out_thread_fd = -1;
    if (runtime == NULL || request == NULL || out_process_fd == NULL || out_thread_fd == NULL) {
        return -22;
    }
    memset(&image, 0, sizeof(image));
    memset(&plan, 0, sizeof(plan));
    memset(prepared, 0, sizeof(prepared));
    plan.process_fd = -1;
    plan.thread_fd = -1;

    int status = lpr_exec_prepare_inherit_fds(request, inherit_fds, inherit_fd_count, bootstrap_fd, prepared, &prepared_count);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_read_full_image(runtime, handle_id, &image);
    if (status != 0) {
        lpr_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }
    status = load_plan(runtime, &image, &plan);
    free(image.bytes);
    if (status != 0) {
        lpr_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }
    status = lpr_exec_start_plan(&plan, request, bootstrap_fd);
    lpr_exec_clear_prepared_inherit_fds(prepared, prepared_count);
    if (status != 0) {
        if (plan.thread_fd >= 16) {
            (void)pacha_fd_close(plan.thread_fd);
        }
        lpr_exec_discard_process_fd(plan.process_fd);
        return status;
    }
    *out_process_fd = plan.process_fd;
    *out_thread_fd = plan.thread_fd;
    return 0;
}
