#include "filed/exec_linux_lpr.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pacha/ipc.h"

#ifndef FILED_LPR_TRACE
#define FILED_LPR_TRACE 0
#endif

// /lib/ld-musl-x86_64.so.1はPachaOS Native muslなため/linuxへ.
static const char *lpr_exec_interpreter_namespace_path(const char *interp_path)
{
    if (strcmp(interp_path, "/lib/ld-musl-x86_64.so.1") == 0) {
        return "/lib/linux/ld-musl-x86_64.so.1";
    }
    return interp_path;
}

typedef struct lpr_runtime_image_cache {
    lpr_exec_image_t image;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t size;
    uint64_t syscall_entry_offset;
    int valid;
} lpr_runtime_image_cache_t;

static lpr_runtime_image_cache_t lpr_runtime_image_cache;

typedef struct lpr_interpreter_cache {
    lpr_exec_file_t file;
    lpr_exec_meta_t meta;
    char path[LPR_EXEC_MAX_INTERP_BYTES];
    int valid;
} lpr_interpreter_cache_t;

static lpr_interpreter_cache_t lpr_interpreter_cache;

static void lpr_exec_clear_interpreter_cache(filed_runtime_t *runtime)
{
    if (!lpr_interpreter_cache.valid) {
        return;
    }
    lpr_exec_free_meta(&lpr_interpreter_cache.meta);
    if (runtime != NULL) {
        lpr_exec_close_file(runtime, &lpr_interpreter_cache.file);
    } else {
        memset(&lpr_interpreter_cache.file, 0, sizeof(lpr_interpreter_cache.file));
    }
    memset(&lpr_interpreter_cache, 0, sizeof(lpr_interpreter_cache));
}

static int lpr_exec_get_interpreter_file(
    filed_runtime_t *runtime,
    const char *path,
    const lpr_exec_file_t **out_file,
    const lpr_exec_meta_t **out_meta)
{
    if (runtime == NULL || path == NULL || out_file == NULL || out_meta == NULL || path[0] == '\0') {
        return -22;
    }
    *out_file = NULL;
    *out_meta = NULL;
    if (lpr_interpreter_cache.valid && strcmp(lpr_interpreter_cache.path, path) == 0) {
        const uint64_t now = lpr_exec_now_ns();
        lpr_exec_metric("interpreter_cache_hit", now, now);
        *out_file = &lpr_interpreter_cache.file;
        *out_meta = &lpr_interpreter_cache.meta;
        return 0;
    }

    {
        const uint64_t now = lpr_exec_now_ns();
        lpr_exec_metric("interpreter_cache_miss", now, now);
    }

    lpr_exec_file_t file;
    lpr_exec_meta_t meta;
    memset(&file, 0, sizeof(file));
    memset(&meta, 0, sizeof(meta));
    int status = lpr_exec_open_absolute_file(runtime, path, &file);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_read_meta(runtime, &file, &meta);
    if (status != 0) {
        lpr_exec_close_file(runtime, &file);
        return status;
    }

    lpr_exec_clear_interpreter_cache(runtime);
    lpr_interpreter_cache.file = file;
    lpr_interpreter_cache.meta = meta;
    strncpy(lpr_interpreter_cache.path, path, sizeof(lpr_interpreter_cache.path) - 1u);
    lpr_interpreter_cache.path[sizeof(lpr_interpreter_cache.path) - 1u] = '\0';
    lpr_interpreter_cache.valid = 1;
    *out_file = &lpr_interpreter_cache.file;
    *out_meta = &lpr_interpreter_cache.meta;
    return 0;
}

static int lpr_exec_get_runtime_image(
    filed_runtime_t *runtime,
    const lpr_exec_image_t **out_image,
    uint64_t *out_syscall_entry_offset)
{
    lpr_exec_file_t file;

    if (runtime == NULL || out_image == NULL || out_syscall_entry_offset == NULL) {
        return -22;
    }
    *out_image = NULL;
    *out_syscall_entry_offset = 0;
    memset(&file, 0, sizeof(file));

    if (lpr_runtime_image_cache.valid) {
        *out_image = &lpr_runtime_image_cache.image;
        *out_syscall_entry_offset = lpr_runtime_image_cache.syscall_entry_offset;
        return 0;
    }

    int status = lpr_exec_open_absolute_file(runtime, LPR_EXEC_RUNTIME_PATH, &file);
    if (status != 0) {
        return status;
    }
    if (lpr_runtime_image_cache.valid &&
        lpr_runtime_image_cache.backend_object == file.backend_object &&
        lpr_runtime_image_cache.object_generation == file.object_generation &&
        lpr_runtime_image_cache.size == file.size)
    {
        lpr_exec_close_file(runtime, &file);
        *out_image = &lpr_runtime_image_cache.image;
        *out_syscall_entry_offset = lpr_runtime_image_cache.syscall_entry_offset;
        return 0;
    }

    lpr_exec_image_t image;
    memset(&image, 0, sizeof(image));
    status = lpr_exec_read_full_file_image(runtime, &file, &image);
    if (status != 0) {
        lpr_exec_close_file(runtime, &file);
        return status;
    }
    uint64_t syscall_entry_offset = 0;
    status = lpr_exec_image_find_symbol(&image, "lpr_syscall_entry", &syscall_entry_offset);
    if (status != 0 || syscall_entry_offset == 0) {
        free(image.bytes);
        lpr_exec_close_file(runtime, &file);
        return status != 0 ? status : -8;
    }

    free(lpr_runtime_image_cache.image.bytes);
    memset(&lpr_runtime_image_cache, 0, sizeof(lpr_runtime_image_cache));
    lpr_runtime_image_cache.image = image;
    lpr_runtime_image_cache.backend_object = file.backend_object;
    lpr_runtime_image_cache.object_generation = file.object_generation;
    lpr_runtime_image_cache.size = file.size;
    lpr_runtime_image_cache.syscall_entry_offset = syscall_entry_offset;
    lpr_runtime_image_cache.valid = 1;
    lpr_exec_close_file(runtime, &file);

    *out_image = &lpr_runtime_image_cache.image;
    *out_syscall_entry_offset = syscall_entry_offset;
    return 0;
}

void lpr_exec_invalidate_runtime_image_cache(uint64_t backend_object)
{
    if (backend_object == 0 ||
        !lpr_runtime_image_cache.valid ||
        lpr_runtime_image_cache.backend_object != backend_object)
    {
        return;
    }
    free(lpr_runtime_image_cache.image.bytes);
    memset(&lpr_runtime_image_cache, 0, sizeof(lpr_runtime_image_cache));
}

void lpr_exec_invalidate_interpreter_cache(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (backend_object == 0 ||
        !lpr_interpreter_cache.valid ||
        lpr_interpreter_cache.file.backend_object != backend_object)
    {
        return;
    }
    lpr_exec_clear_interpreter_cache(runtime);
}

typedef enum lpr_exec_stage_metric_id {
    LPR_EXEC_STAGE_PREPARE_INHERIT_FDS,
    LPR_EXEC_STAGE_INIT_MAIN_FILE,
    LPR_EXEC_STAGE_READ_MAIN_META,
    LPR_EXEC_STAGE_GET_INTERP,
    LPR_EXEC_STAGE_GET_LPR_RUNTIME,
    LPR_EXEC_STAGE_PROCESS_CREATE,
    LPR_EXEC_STAGE_LOAD_LPR_RUNTIME,
    LPR_EXEC_STAGE_INSTALL_LOW_LAYOUT,
    LPR_EXEC_STAGE_LOAD_MAIN,
    LPR_EXEC_STAGE_INTERPRETER_CACHE_HIT,
    LPR_EXEC_STAGE_INTERPRETER_CACHE_MISS,
    LPR_EXEC_STAGE_OPEN_INTERPRETER,
    LPR_EXEC_STAGE_READ_INTERPRETER_META,
    LPR_EXEC_STAGE_LOAD_INTERPRETER,
    LPR_EXEC_STAGE_COMMIT_FILE_MAPS,
    LPR_EXEC_STAGE_LOAD_PLAN,
    LPR_EXEC_STAGE_START_PLAN,
    LPR_EXEC_STAGE_START_STACK_VMO,
    LPR_EXEC_STAGE_START_STACK_MMAP,
    LPR_EXEC_STAGE_START_STACK_MAP_CHILD,
    LPR_EXEC_STAGE_START_STACK_BUILD,
    LPR_EXEC_STAGE_START_STACK_UNMAP,
    LPR_EXEC_STAGE_START_THREAD_CREATE,
    LPR_EXEC_STAGE_START_THREAD_START,
    LPR_EXEC_STAGE_TOTAL_BEFORE_REPLY,
    LPR_EXEC_STAGE_MAX,
} lpr_exec_stage_metric_id_t;

typedef struct lpr_exec_stage_metric {
    const char *name;
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
} lpr_exec_stage_metric_t;

static lpr_exec_stage_metric_t lpr_exec_stage_metrics[LPR_EXEC_STAGE_MAX] = {
    [LPR_EXEC_STAGE_PREPARE_INHERIT_FDS] = { .name = "prepare_inherit_fds" },
    [LPR_EXEC_STAGE_INIT_MAIN_FILE] = { .name = "init_main_file" },
    [LPR_EXEC_STAGE_READ_MAIN_META] = { .name = "read_main_meta" },
    [LPR_EXEC_STAGE_GET_INTERP] = { .name = "get_interp" },
    [LPR_EXEC_STAGE_GET_LPR_RUNTIME] = { .name = "get_lpr_runtime" },
    [LPR_EXEC_STAGE_PROCESS_CREATE] = { .name = "process_create" },
    [LPR_EXEC_STAGE_LOAD_LPR_RUNTIME] = { .name = "load_lpr_runtime" },
    [LPR_EXEC_STAGE_INSTALL_LOW_LAYOUT] = { .name = "install_low_layout" },
    [LPR_EXEC_STAGE_LOAD_MAIN] = { .name = "load_main" },
    [LPR_EXEC_STAGE_INTERPRETER_CACHE_HIT] = { .name = "interpreter_cache_hit" },
    [LPR_EXEC_STAGE_INTERPRETER_CACHE_MISS] = { .name = "interpreter_cache_miss" },
    [LPR_EXEC_STAGE_OPEN_INTERPRETER] = { .name = "open_interpreter" },
    [LPR_EXEC_STAGE_READ_INTERPRETER_META] = { .name = "read_interpreter_meta" },
    [LPR_EXEC_STAGE_LOAD_INTERPRETER] = { .name = "load_interpreter" },
    [LPR_EXEC_STAGE_COMMIT_FILE_MAPS] = { .name = "commit_file_maps" },
    [LPR_EXEC_STAGE_LOAD_PLAN] = { .name = "load_plan" },
    [LPR_EXEC_STAGE_START_PLAN] = { .name = "start_plan" },
    [LPR_EXEC_STAGE_START_STACK_VMO] = { .name = "start_stack_vmo" },
    [LPR_EXEC_STAGE_START_STACK_MMAP] = { .name = "start_stack_mmap" },
    [LPR_EXEC_STAGE_START_STACK_MAP_CHILD] = { .name = "start_stack_map_child" },
    [LPR_EXEC_STAGE_START_STACK_BUILD] = { .name = "start_stack_build" },
    [LPR_EXEC_STAGE_START_STACK_UNMAP] = { .name = "start_stack_unmap" },
    [LPR_EXEC_STAGE_START_THREAD_CREATE] = { .name = "start_thread_create" },
    [LPR_EXEC_STAGE_START_THREAD_START] = { .name = "start_thread_start" },
    [LPR_EXEC_STAGE_TOTAL_BEFORE_REPLY] = { .name = "total_before_reply" },
};

uint64_t lpr_exec_now_ns(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t lpr_exec_now_cycles(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int lpr_exec_stage_metric_index(const char *label)
{
    if (label == NULL) {
        return -1;
    }
    for (int i = 0; i < LPR_EXEC_STAGE_MAX; ++i) {
        if (lpr_exec_stage_metrics[i].name != NULL &&
            strcmp(lpr_exec_stage_metrics[i].name, label) == 0)
        {
            return i;
        }
    }
    return -1;
}

void lpr_exec_metric(const char *label, uint64_t start_ns, uint64_t end_ns)
{
    if (label == NULL || start_ns == 0 || end_ns < start_ns) {
        return;
    }
    const uint64_t elapsed_ns = end_ns - start_ns;
    const int index = lpr_exec_stage_metric_index(label);
    if (index < 0) {
        return;
    }
    lpr_exec_stage_metric_t *metric = &lpr_exec_stage_metrics[index];
    metric->count++;
    metric->total_ns += elapsed_ns;
    if (elapsed_ns > metric->max_ns) {
        metric->max_ns = elapsed_ns;
    }
}

void lpr_exec_metric_cycles(const char *label, uint64_t start_cycles, uint64_t end_cycles)
{
    if (label == NULL || start_cycles == 0 || end_cycles < start_cycles) {
        return;
    }
    const int index = lpr_exec_stage_metric_index(label);
    if (index < 0) {
        return;
    }
    const uint64_t elapsed_cycles = end_cycles - start_cycles;
    lpr_exec_stage_metric_t *metric = &lpr_exec_stage_metrics[index];
    metric->total_cycles += elapsed_cycles;
    if (elapsed_cycles > metric->max_cycles) {
        metric->max_cycles = elapsed_cycles;
    }
}

static void lpr_exec_metric_span(
    const char *label,
    uint64_t start_ns,
    uint64_t end_ns,
    uint64_t start_cycles,
    uint64_t end_cycles)
{
    lpr_exec_metric(label, start_ns, end_ns);
    lpr_exec_metric_cycles(label, start_cycles, end_cycles);
}

#define LPR_EXEC_STAGE_BEGIN(ns_var, cycles_var) \
    do { \
        (ns_var) = lpr_exec_now_ns(); \
        (cycles_var) = lpr_exec_now_cycles(); \
    } while (0)

#define LPR_EXEC_STAGE_RECORD(label, ns_var, cycles_var) \
    do { \
        const uint64_t lpr_exec_stage_end_ns__ = lpr_exec_now_ns(); \
        const uint64_t lpr_exec_stage_end_cycles__ = lpr_exec_now_cycles(); \
        lpr_exec_metric_span((label), (ns_var), lpr_exec_stage_end_ns__, (cycles_var), lpr_exec_stage_end_cycles__); \
    } while (0)

void filed_exec_linux_lpr_dump_metrics(void)
{
    for (int i = 0; i < LPR_EXEC_STAGE_MAX; ++i) {
        const lpr_exec_stage_metric_t *metric = &lpr_exec_stage_metrics[i];
        if (metric->count == 0 || metric->name == NULL) {
            continue;
        }
        printf(
            "[filed] metric scope=lpr_exec_stage op=%s count=%llu avg_ns=%llu max_ns=%llu avg_cycles=%llu max_cycles=%llu\n",
            metric->name,
            (unsigned long long)metric->count,
            (unsigned long long)(metric->total_ns / metric->count),
            (unsigned long long)metric->max_ns,
            (unsigned long long)(metric->count == 0 ? 0 : metric->total_cycles / metric->count),
            (unsigned long long)metric->max_cycles);
    }
    lpr_exec_image_dump_metrics();
    lpr_exec_map_dump_metrics();
}

static int load_plan(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *main_file,
    const lpr_exec_meta_t *main_meta,
    lpr_exec_plan_t *plan)
{
    const lpr_exec_image_t *lpr_image = NULL;
    const lpr_exec_file_t *interp_file = NULL;
    const lpr_exec_meta_t *interp_meta = NULL;
    lpr_exec_loaded_t lpr_loaded;
    lpr_exec_loaded_t main_loaded;
    lpr_exec_loaded_t interp_loaded;
    lpr_exec_pending_map_batch_t file_map_batch;
    char interp_path[LPR_EXEC_MAX_INTERP_BYTES];
    uint64_t syscall_entry_offset = 0;

    if (runtime == NULL || main_file == NULL || main_meta == NULL || plan == NULL) {
        return -22;
    }
    memset(plan, 0, sizeof(*plan));
    lpr_exec_pending_map_batch_init(&file_map_batch);
    plan->process_fd = -1;
    plan->thread_fd = -1;

    uint64_t stage_start = 0;
    uint64_t stage_start_cycles = 0;
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    int status = lpr_exec_meta_get_interp_path(runtime, main_file, main_meta, interp_path, sizeof(interp_path));
    LPR_EXEC_STAGE_RECORD("get_interp", stage_start, stage_start_cycles);
    if (status != 0) {
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_get_runtime_image(runtime, &lpr_image, &syscall_entry_offset);
    LPR_EXEC_STAGE_RECORD("get_lpr_runtime", stage_start, stage_start_cycles);
    if (status != 0) {
        fprintf(stderr, "[filed] linux-lpr: runtime missing path=%s status=%d\n", LPR_EXEC_RUNTIME_PATH, status);
        return status;
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
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    const int process_fd = pacha_process_create(process_rights, 0);
    LPR_EXEC_STAGE_RECORD("process_create", stage_start, stage_start_cycles);
    if (process_fd < 16) {
        return -12;
    }
    plan->process_fd = process_fd;

    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_load_image_with_low_layout_into_process(
        process_fd,
        lpr_image,
        LPR_EXEC_LPR_BASE,
        0,
        syscall_entry_offset,
        &lpr_loaded);
    LPR_EXEC_STAGE_RECORD("load_lpr_runtime", stage_start, stage_start_cycles);
    if (status != 0) {
        lpr_exec_discard_process_fd(process_fd);
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = 0;
    LPR_EXEC_STAGE_RECORD("install_low_layout", stage_start, stage_start_cycles);
    if (status != 0) {
        lpr_exec_discard_process_fd(process_fd);
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_prepare_file_into_map_batch(
        runtime,
        main_file,
        main_meta,
        LPR_EXEC_MAIN_DYN_BASE,
        1,
        &file_map_batch,
        &main_loaded);
    LPR_EXEC_STAGE_RECORD("load_main", stage_start, stage_start_cycles);
    if (status != 0) {
        lpr_exec_pending_map_batch_discard(&file_map_batch);
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
        const char *load_interp_path = lpr_exec_interpreter_namespace_path(interp_path);
        LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
        status = lpr_exec_get_interpreter_file(runtime, load_interp_path, &interp_file, &interp_meta);
        LPR_EXEC_STAGE_RECORD("open_interpreter", stage_start, stage_start_cycles);
        if (status != 0) {
            fprintf(stderr,
                "[filed] linux-lpr: interpreter missing path=%s load_path=%s status=%d\n",
                interp_path,
                load_interp_path,
                status);
            lpr_exec_pending_map_batch_discard(&file_map_batch);
            lpr_exec_discard_process_fd(process_fd);
            return status;
        }
        lpr_exec_metric_span("read_interpreter_meta", stage_start, stage_start, stage_start_cycles, stage_start_cycles);
        LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
        status = lpr_exec_prepare_file_into_map_batch(
            runtime,
            interp_file,
            interp_meta,
            LPR_EXEC_INTERP_DYN_BASE,
            1,
            &file_map_batch,
            &interp_loaded);
        LPR_EXEC_STAGE_RECORD("load_interpreter", stage_start, stage_start_cycles);
        if (status != 0) {
            lpr_exec_pending_map_batch_discard(&file_map_batch);
            lpr_exec_discard_process_fd(process_fd);
            return status;
        }
        plan->runtime_entry = interp_loaded.entry;
        plan->interpreter_base = interp_loaded.base;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_pending_map_batch_commit(process_fd, &file_map_batch);
    LPR_EXEC_STAGE_RECORD("commit_file_maps", stage_start, stage_start_cycles);
    if (status != 0) {
        lpr_exec_pending_map_batch_discard(&file_map_batch);
        lpr_exec_discard_process_fd(process_fd);
        return status;
    }
    return 0;
}

static int lpr_exec_prewarm_runtime(filed_runtime_t *runtime)
{
    const lpr_exec_image_t *lpr_image = NULL;
    uint64_t syscall_entry_offset = 0;
    lpr_exec_loaded_t loaded;

    memset(&loaded, 0, sizeof(loaded));
    int status = lpr_exec_get_runtime_image(runtime, &lpr_image, &syscall_entry_offset);
    if (status != 0) {
        return status;
    }

    const uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_MAP_INTO;
    const int process_fd = pacha_process_create(process_rights, 0);
    if (process_fd < 16) {
        return -12;
    }
    status = lpr_exec_load_image_with_low_layout_into_process(
        process_fd,
        lpr_image,
        LPR_EXEC_LPR_BASE,
        0,
        syscall_entry_offset,
        &loaded);
    lpr_exec_discard_process_fd(process_fd);
    return status;
}

static int lpr_exec_prewarm_interpreter(filed_runtime_t *runtime)
{
    const lpr_exec_file_t *interp_file = NULL;
    const lpr_exec_meta_t *interp_meta = NULL;
    lpr_exec_pending_map_batch_t batch;
    lpr_exec_loaded_t loaded;

    lpr_exec_pending_map_batch_init(&batch);
    memset(&loaded, 0, sizeof(loaded));
    int status = lpr_exec_get_interpreter_file(
        runtime,
        "/lib/linux/ld-musl-x86_64.so.1",
        &interp_file,
        &interp_meta);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_prepare_file_into_map_batch(
        runtime,
        interp_file,
        interp_meta,
        LPR_EXEC_INTERP_DYN_BASE,
        1,
        &batch,
        &loaded);
    lpr_exec_pending_map_batch_discard(&batch);
    return status;
}

int filed_exec_linux_lpr_prewarm(struct filed_runtime *runtime)
{
    if (runtime == NULL) {
        return -22;
    }
    int status = lpr_exec_prewarm_runtime(runtime);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_prewarm_interpreter(runtime);
    if (status != 0) {
        return status;
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
    lpr_exec_file_t file;
    lpr_exec_meta_t meta;
    lpr_exec_plan_t plan;
    int prepared[FILED_WIRE_EXEC_MAX_INHERIT_FDS + 1];
    uint64_t prepared_count = 0;

    if (out_process_fd != NULL) *out_process_fd = -1;
    if (out_thread_fd != NULL) *out_thread_fd = -1;
    if (runtime == NULL || request == NULL || out_process_fd == NULL || out_thread_fd == NULL) {
        return -22;
    }
    memset(&file, 0, sizeof(file));
    memset(&meta, 0, sizeof(meta));
    memset(&plan, 0, sizeof(plan));
    memset(prepared, 0, sizeof(prepared));
    plan.process_fd = -1;
    plan.thread_fd = -1;

#if FILED_LPR_TRACE
    memset(&lpr_exec_trace_sample, 0, sizeof(lpr_exec_trace_sample));
#endif
    uint64_t total_start = lpr_exec_now_ns();
    uint64_t total_start_cycles = lpr_exec_now_cycles();
    uint64_t stage_start = total_start;
    uint64_t stage_start_cycles = total_start_cycles;
    int status = lpr_exec_prepare_inherit_fds(request, inherit_fds, inherit_fd_count, bootstrap_fd, prepared, &prepared_count);
    LPR_EXEC_STAGE_RECORD("prepare_inherit_fds", stage_start, stage_start_cycles);
    if (status != 0) {
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_init_file_from_handle(runtime, handle_id, &file);
    LPR_EXEC_STAGE_RECORD("init_main_file", stage_start, stage_start_cycles);
    if (status != 0) {
        lpr_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_read_meta(runtime, &file, &meta);
    LPR_EXEC_STAGE_RECORD("read_main_meta", stage_start, stage_start_cycles);
    if (status != 0) {
        lpr_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = load_plan(runtime, &file, &meta, &plan);
    LPR_EXEC_STAGE_RECORD("load_plan", stage_start, stage_start_cycles);
    lpr_exec_free_meta(&meta);
    if (status != 0) {
        lpr_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }
    LPR_EXEC_STAGE_BEGIN(stage_start, stage_start_cycles);
    status = lpr_exec_start_plan(&plan, request, bootstrap_fd);
    LPR_EXEC_STAGE_RECORD("start_plan", stage_start, stage_start_cycles);
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
    lpr_exec_metric_span("total_before_reply", total_start, lpr_exec_now_ns(), total_start_cycles, lpr_exec_now_cycles());
    return 0;
}
