#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include "pacha/ipc.h"
#include "pacha/syscall.h"

static int set_inherit(int fd, int enabled)
{
    if (fd < 16) {
        return -22;
    }
    const uint64_t flags = enabled ? PACHA_FD_FLAG_INHERIT : 0;
    const long status = pacha_fd_fcntl(fd, PACHA_FD_FCNTL_SET_FLAGS, flags, PACHA_FD_FLAG_INHERIT);
    return status == 0 ? 0 : -13;
}

int lpr_exec_prepare_inherit_fds(
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *prepared,
    uint64_t *out_prepared_count)
{
    uint64_t prepared_count = 0;
    if (request == NULL || prepared == NULL || out_prepared_count == NULL) {
        return -22;
    }
    *out_prepared_count = 0;
    if (inherit_fd_count > FILED_WIRE_EXEC_MAX_INHERIT_FDS) {
        return -22;
    }
    if ((request->flags & FILED_WIRE_EXEC_INHERIT_FDS) != 0) {
        if (inherit_fds == NULL && inherit_fd_count != 0) {
            return -22;
        }
        for (uint64_t i = 0; i < inherit_fd_count; ++i) {
            const int fd = inherit_fds[i];
            if (fd < 16 || set_inherit(fd, 1) != 0) {
                return -13;
            }
            prepared[prepared_count++] = fd;
        }
    }
    if ((request->flags & FILED_WIRE_EXEC_BOOTSTRAP_FD) != 0) {
        if (bootstrap_fd < 16 || set_inherit(bootstrap_fd, 1) != 0) {
            return -13;
        }
        prepared[prepared_count++] = bootstrap_fd;
    }
    *out_prepared_count = prepared_count;
    return 0;
}

void lpr_exec_clear_prepared_inherit_fds(const int *prepared, uint64_t count)
{
    if (prepared == NULL) {
        return;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (prepared[i] >= 16) {
            (void)set_inherit(prepared[i], 0);
        }
    }
}

static int push_u64(unsigned char *stack, uint64_t *sp, uint64_t value)
{
    if (stack == NULL || sp == NULL || *sp < 8) {
        return -22;
    }
    *sp -= 8;
    lpr_exec_wr64(stack + *sp, value);
    return 0;
}

static int copy_stack_string(
    unsigned char *stack,
    uint64_t *sp,
    uint64_t stack_base,
    const char *string,
    uint64_t *out_va)
{
    if (stack == NULL || sp == NULL || string == NULL || out_va == NULL) {
        return -22;
    }
    const uint64_t len = (uint64_t)strlen(string) + 1u;
    if (len > *sp) {
        return -7;
    }
    *sp -= len;
    memcpy(stack + *sp, string, (size_t)len);
    *out_va = stack_base + *sp;
    return 0;
}

static uint64_t request_argc(const filed_wire_exec_path_t *request)
{
    if (request == NULL || request->argc == 0) {
        return 1;
    }
    return request->argc;
}

static const char *request_arg(const filed_wire_exec_path_t *request, uint64_t index)
{
    if (request == NULL) {
        return "";
    }
    if (request->argc != 0 && index < request->argc) {
        return filed_wire_exec_string(request, request->argv[index]);
    }
    return request->path;
}

int lpr_exec_start_plan(lpr_exec_plan_t *plan, const filed_wire_exec_path_t *request, int bootstrap_fd)
{
    if (plan == NULL || request == NULL || plan->process_fd < 16) {
        return -22;
    }
    if (request->argc > FILED_WIRE_EXEC_MAX_ARGS || request->envc > FILED_WIRE_EXEC_MAX_ENVS) {
        return -22;
    }
    const uint64_t stack_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t stage_start = lpr_exec_now_ns();
    uint64_t stage_start_cycles = lpr_exec_now_cycles();
    const int stack_fd = pacha_vmo_create(PACHA_PROCESS_DEFAULT_STACK_SIZE, stack_rights, 0);
    lpr_exec_metric("start_stack_vmo", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_stack_vmo", stage_start_cycles, lpr_exec_now_cycles());
    if (stack_fd < 16) {
        return -12;
    }
    stage_start = lpr_exec_now_ns();
    stage_start_cycles = lpr_exec_now_cycles();
    unsigned char *stack = pacha_mmap(
        stack_fd,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    lpr_exec_metric("start_stack_mmap", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_stack_mmap", stage_start_cycles, lpr_exec_now_cycles());
    if (stack == NULL) {
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    stage_start = lpr_exec_now_ns();
    stage_start_cycles = lpr_exec_now_cycles();
    const long stack_map = pacha_process_map_flags(
        plan->process_fd,
        stack_fd,
        PACHA_PROCESS_MAP_ANYWHERE,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        0,
        PACHA_PROCESS_MAP_PRIVATE);
    lpr_exec_metric("start_stack_map_child", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_stack_map_child", stage_start_cycles, lpr_exec_now_cycles());
    if (stack_map < 4096) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    const uint64_t stack_base = (uint64_t)stack_map;
    uint64_t sp = PACHA_PROCESS_DEFAULT_STACK_SIZE;
    const uint64_t argc = request_argc(request);
    const uint64_t envc = request->envc;
    uint64_t argv_va[FILED_WIRE_EXEC_MAX_ARGS];
    uint64_t envp_va[FILED_WIRE_EXEC_MAX_ENVS];
    memset(argv_va, 0, sizeof(argv_va));
    memset(envp_va, 0, sizeof(envp_va));

    stage_start = lpr_exec_now_ns();
    stage_start_cycles = lpr_exec_now_cycles();
    for (uint64_t i = envc; i > 0; --i) {
        const int status = copy_stack_string(
            stack,
            &sp,
            stack_base,
            filed_wire_exec_string(request, request->envp[i - 1u]),
            &envp_va[i - 1u]);
        if (status != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return status;
        }
    }
    for (uint64_t i = argc; i > 0; --i) {
        const int status = copy_stack_string(stack, &sp, stack_base, request_arg(request, i - 1u), &argv_va[i - 1u]);
        if (status != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return status;
        }
    }
    const uint64_t argv0_va = argv_va[0];
    sp &= ~15ull;
    sp -= 16;
    const uint64_t random_va = stack_base + sp;
    if (pacha_getrandom(stack + sp, 16, 0) != 16) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -5;
    }
    sp &= ~15ull;

    const int has_bootstrap =
        (request->flags & FILED_WIRE_EXEC_BOOTSTRAP_FD) != 0 && bootstrap_fd >= 16;
    if (push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_NULL) != 0 ||
        (has_bootstrap &&
            (push_u64(stack, &sp, (uint64_t)(uint32_t)bootstrap_fd) != 0 ||
             push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        push_u64(stack, &sp, argv0_va) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_EXECFN) != 0 ||
        push_u64(stack, &sp, random_va) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_RANDOM) != 0 ||
        push_u64(stack, &sp, plan->main_entry) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_ENTRY) != 0 ||
        push_u64(stack, &sp, plan->interpreter_base) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_BASE) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_SECURE) != 0 ||
        push_u64(stack, &sp, 100) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_CLKTCK) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_HWCAP) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_EGID) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_GID) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_EUID) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_UID) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_PAGE_SIZE) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_PAGESZ) != 0 ||
        push_u64(stack, &sp, plan->phnum) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_PHNUM) != 0 ||
        push_u64(stack, &sp, plan->phent) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_PHENT) != 0 ||
        push_u64(stack, &sp, plan->phdr_va) != 0 ||
        push_u64(stack, &sp, LPR_EXEC_AT_PHDR) != 0)
    {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    if (push_u64(stack, &sp, 0) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    for (uint64_t i = envc; i > 0; --i) {
        if (push_u64(stack, &sp, envp_va[i - 1u]) != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return -12;
        }
    }
    if (push_u64(stack, &sp, 0) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    for (uint64_t i = argc; i > 0; --i) {
        if (push_u64(stack, &sp, argv_va[i - 1u]) != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return -12;
        }
    }
    if (push_u64(stack, &sp, argc) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    lpr_exec_metric("start_stack_build", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_stack_build", stage_start_cycles, lpr_exec_now_cycles());

    stage_start = lpr_exec_now_ns();
    stage_start_cycles = lpr_exec_now_cycles();
    (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
    (void)pacha_fd_close(stack_fd);
    lpr_exec_metric("start_stack_unmap", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_stack_unmap", stage_start_cycles, lpr_exec_now_cycles());

    const uint64_t thread_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_START |
        PACHA_FD_RIGHT_SET_CONTEXT;
    stage_start = lpr_exec_now_ns();
    stage_start_cycles = lpr_exec_now_cycles();
    const int thread_fd = pacha_thread_create(plan->process_fd, plan->runtime_entry, stack_base + sp, 0, 0, thread_rights);
    lpr_exec_metric("start_thread_create", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_thread_create", stage_start_cycles, lpr_exec_now_cycles());
    if (thread_fd < 16) {
        return -12;
    }
    stage_start = lpr_exec_now_ns();
    stage_start_cycles = lpr_exec_now_cycles();
    const int start_status = pacha_thread_start(thread_fd);
    lpr_exec_metric("start_thread_start", stage_start, lpr_exec_now_ns());
    lpr_exec_metric_cycles("start_thread_start", stage_start_cycles, lpr_exec_now_cycles());
    if (start_status != 0) {
        (void)pacha_fd_close(thread_fd);
        return -5;
    }
    plan->thread_fd = thread_fd;
    return 0;
}

void lpr_exec_discard_process_fd(int process_fd)
{
    if (process_fd >= 16) {
        (void)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            1);
        (void)pacha_fd_close(process_fd);
    }
}
