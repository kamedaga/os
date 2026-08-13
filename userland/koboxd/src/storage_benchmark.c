#include "storage_benchmark.h"

#include "storage/ext4_nvme_benchmark_spec.h"

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
#include "kobox/device_pachaos_capsule.h"
#include "kobox/shim.h"
#include "linux_personality/linux_block.h"
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
extern void kb_module_external_cross_call_profile_reset(void);
extern uint64_t kb_module_external_cross_call_profile_snapshot(void);
#endif

static uint64_t benchmark_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t benchmark_clock_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t benchmark_timer_base_ns;
static uint64_t benchmark_timer_base_tsc;
static double benchmark_timer_ns_per_cycle;

static int benchmark_timer_init(void)
{
    const uint64_t start_ns = benchmark_clock_ns();
    const uint64_t start_tsc = benchmark_read_tsc();
    uint64_t end_ns = start_ns;
    uint64_t end_tsc = start_tsc;
    while (end_ns - start_ns < 200000000ull) {
        end_ns = benchmark_clock_ns();
        end_tsc = benchmark_read_tsc();
    }
    if (start_ns == 0 || end_ns <= start_ns || end_tsc <= start_tsc) {
        return -1;
    }
    benchmark_timer_base_ns = end_ns;
    benchmark_timer_base_tsc = end_tsc;
    benchmark_timer_ns_per_cycle =
        (double)(end_ns - start_ns) / (double)(end_tsc - start_tsc);
    return 0;
}

static uint64_t benchmark_now_ns(void)
{
    const uint64_t now_tsc = benchmark_read_tsc();
    return benchmark_timer_base_ns +
        (uint64_t)((double)(now_tsc - benchmark_timer_base_tsc) *
            benchmark_timer_ns_per_cycle);
}

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
static kb_linux_block_profile_t benchmark_block_profile_begin(void)
{
    kb_linux_block_profile_t profile;
    kb_linux_block_profile_snapshot(&profile);
    return profile;
}

static kb_pachaos_capsule_irq_profile_t benchmark_irq_profile_begin(void)
{
    kb_pachaos_capsule_irq_profile_t profile;
    kb_pachaos_capsule_irq_profile_snapshot(&profile);
    return profile;
}

static void benchmark_irq_profile_end(
    const char *phase,
    const kb_pachaos_capsule_irq_profile_t *start)
{
    kb_pachaos_capsule_irq_profile_t end;
    kb_pachaos_capsule_irq_profile_snapshot(&end);
    printf("KOBOX_EXT4_NVME_IRQ_PROFILE phase=%s wait_calls=%llu wait_cycles=%llu "
           "fd_wait_calls=%llu fd_wait_cycles=%llu fd_wait_ready=%llu "
           "pre_poll_ready=%llu post_poll_ready=%llu handler_calls=%llu\n",
        phase,
        (unsigned long long)(end.wait_calls - start->wait_calls),
        (unsigned long long)(end.wait_cycles - start->wait_cycles),
        (unsigned long long)(end.fd_wait_calls - start->fd_wait_calls),
        (unsigned long long)(end.fd_wait_cycles - start->fd_wait_cycles),
        (unsigned long long)(end.fd_wait_ready - start->fd_wait_ready),
        (unsigned long long)(end.pre_poll_ready - start->pre_poll_ready),
        (unsigned long long)(end.post_poll_ready - start->post_poll_ready),
        (unsigned long long)(end.handler_calls - start->handler_calls));
}

static void benchmark_block_profile_end(
    const char *phase,
    const kb_linux_block_profile_t *start)
{
    kb_linux_block_profile_t end;
    kb_linux_block_profile_snapshot(&end);
    const unsigned stage = KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL;
    const unsigned flush_stage = KB_LINUX_BLOCK_PROFILE_NVME_FLUSH;
    const unsigned wait_stage = KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT;
    printf("KOBOX_EXT4_NVME_PROFILE phase=%s io_calls=%llu io_bytes=%llu io_cycles=%llu "
           "flush_calls=%llu flush_cycles=%llu wait_cycles=%llu native_fua=%llu\n",
        phase,
        (unsigned long long)(end.calls[stage] - start->calls[stage]),
        (unsigned long long)(end.disk_read_bytes - start->disk_read_bytes),
        (unsigned long long)(end.cycles[stage] - start->cycles[stage]),
        (unsigned long long)(end.calls[flush_stage] - start->calls[flush_stage]),
        (unsigned long long)(end.cycles[flush_stage] - start->cycles[flush_stage]),
        (unsigned long long)(end.cycles[wait_stage] - start->cycles[wait_stage]),
        (unsigned long long)(end.native_fua_commands - start->native_fua_commands));
    printf("KOBOX_EXT4_NVME_READ_COMMANDS phase=%s 4k=%llu 16k=%llu 32k=%llu "
           "64k=%llu 128k=%llu 256k=%llu 512k=%llu other=%llu other_bytes=%llu\n",
        phase,
        (unsigned long long)(end.disk_read_command_calls[0] -
            start->disk_read_command_calls[0]),
        (unsigned long long)(end.disk_read_command_calls[1] -
            start->disk_read_command_calls[1]),
        (unsigned long long)(end.disk_read_command_calls[2] -
            start->disk_read_command_calls[2]),
        (unsigned long long)(end.disk_read_command_calls[3] -
            start->disk_read_command_calls[3]),
        (unsigned long long)(end.disk_read_command_calls[4] -
            start->disk_read_command_calls[4]),
        (unsigned long long)(end.disk_read_command_calls[5] -
            start->disk_read_command_calls[5]),
        (unsigned long long)(end.disk_read_command_calls[6] -
            start->disk_read_command_calls[6]),
        (unsigned long long)(end.disk_read_command_calls[7] -
            start->disk_read_command_calls[7]),
        (unsigned long long)(end.disk_read_command_bytes[7] -
            start->disk_read_command_bytes[7]));
    const uint64_t command_start = start->disk_read_command_count;
    const uint64_t command_end = end.disk_read_command_count;
    if (command_end >= command_start && command_end - command_start <= 64u) {
        for (uint64_t command = command_start; command < command_end; command++) {
            const size_t slot = (size_t)(command % 64u);
            printf("KOBOX_EXT4_NVME_READ_COMMAND phase=%s index=%llu sector=%llu bytes=%llu\n",
                phase,
                (unsigned long long)(command - command_start),
                (unsigned long long)end.disk_read_command_sectors[slot],
                (unsigned long long)end.disk_read_command_lengths[slot]);
        }
    }
    printf("KOBOX_EXT4_NVME_WRITE_COMMANDS phase=%s 4k=%llu 16k=%llu 32k=%llu "
           "64k=%llu 128k=%llu 256k=%llu 512k=%llu other=%llu other_bytes=%llu\n",
        phase,
        (unsigned long long)(end.disk_write_command_calls[0] -
            start->disk_write_command_calls[0]),
        (unsigned long long)(end.disk_write_command_calls[1] -
            start->disk_write_command_calls[1]),
        (unsigned long long)(end.disk_write_command_calls[2] -
            start->disk_write_command_calls[2]),
        (unsigned long long)(end.disk_write_command_calls[3] -
            start->disk_write_command_calls[3]),
        (unsigned long long)(end.disk_write_command_calls[4] -
            start->disk_write_command_calls[4]),
        (unsigned long long)(end.disk_write_command_calls[5] -
            start->disk_write_command_calls[5]),
        (unsigned long long)(end.disk_write_command_calls[6] -
            start->disk_write_command_calls[6]),
        (unsigned long long)(end.disk_write_command_calls[7] -
            start->disk_write_command_calls[7]),
        (unsigned long long)(end.disk_write_command_bytes[7] -
            start->disk_write_command_bytes[7]));
    const uint64_t write_command_start = start->disk_write_command_count;
    const uint64_t write_command_end = end.disk_write_command_count;
    if (write_command_end >= write_command_start &&
        write_command_end - write_command_start <= 64u)
    {
        for (uint64_t command = write_command_start;
             command < write_command_end;
             command++)
        {
            const size_t slot = (size_t)(command % 64u);
            printf("KOBOX_EXT4_NVME_WRITE_COMMAND phase=%s index=%llu sector=%llu "
                   "bytes=%llu flags=0x%x\n",
                phase,
                (unsigned long long)(command - write_command_start),
                (unsigned long long)end.disk_write_command_sectors[slot],
                (unsigned long long)end.disk_write_command_lengths[slot],
                end.disk_write_command_flags[slot]);
        }
    }
    printf("KOBOX_EXT4_NVME_PROFILE_STAGES phase=%s alloc=%llu map=%llu before=%llu "
           "submit=%llu wait=%llu unmap=%llu free=%llu cq_poll=%llu irq_wait=%llu "
           "yield=%llu post_irq=%llu prp_init=%llu map_pages=%llu prp_build=%llu "
           "prp_aux=%llu\n",
        phase,
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_DMA_MAP] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_DMA_MAP]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD]),
        (unsigned long long)(end.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP] -
            start->cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP]));
}
#define BENCHMARK_PROFILE_BEGIN(name) \
    kb_linux_block_profile_t name = benchmark_block_profile_begin(); \
    kb_pachaos_capsule_irq_profile_t name##_irq = benchmark_irq_profile_begin()
#define BENCHMARK_PROFILE_END(phase, name) \
    do { \
        benchmark_block_profile_end((phase), &(name)); \
        benchmark_irq_profile_end((phase), &(name##_irq)); \
    } while (0)

static void benchmark_hotpath_profile_begin(void)
{
    koboxd_fs_hotpath_profile_reset();
    kb_fs_hotpath_profile_reset();
    kb_memory_hotpath_profile_reset();
    kb_module_external_cross_call_profile_reset();
}

static void benchmark_hotpath_profile_end(const char *phase)
{
    koboxd_fs_hotpath_profile_t profile;
    kb_fs_hotpath_profile_t fs;
    kb_memory_hotpath_profile_t memory;
    koboxd_fs_hotpath_profile_snapshot(&profile);
    kb_fs_hotpath_profile_snapshot(&fs);
    kb_memory_hotpath_profile_snapshot(&memory);
    printf("KOBOX_EXT4_HOTPATH_PROFILE phase=%s cache_lookup=%llu ext4_lookup=%llu "
           "register=%llu create=%llu rename=%llu rename_post=%llu unlink=%llu "
           "unlink_post=%llu refresh=%llu\n",
        phase,
        (unsigned long long)profile.cache_lookup_cycles,
        (unsigned long long)profile.ext4_lookup_cycles,
        (unsigned long long)profile.object_register_cycles,
        (unsigned long long)profile.ext4_create_cycles,
        (unsigned long long)profile.ext4_rename_cycles,
        (unsigned long long)profile.rename_post_cycles,
        (unsigned long long)profile.ext4_unlink_cycles,
        (unsigned long long)profile.unlink_post_cycles,
        (unsigned long long)profile.object_refresh_cycles);
    printf("KOBOX_FS_HOTPATH_PROFILE phase=%s xrefresh_calls=%llu xrefresh=%llu "
           "xload_calls=%llu xload=%llu folio_calls=%llu folio=%llu "
           "buffer_calls=%llu buffer=%llu inode_find_calls=%llu inode_find=%llu "
           "inode_claim_calls=%llu inode_claim=%llu dentry_find_calls=%llu dentry_find=%llu "
           "dentry_claim_calls=%llu dentry_claim=%llu write_begin_calls=%llu "
           "write_begin=%llu write_end_calls=%llu write_end=%llu "
           "bio_calls=%llu preflush=%llu fua=%llu flush_op=%llu "
           "plug_start=%llu plug_finish=%llu plug_queued=%llu plug_max=%llu\n",
        phase,
        (unsigned long long)fs.xarray_refresh_calls,
        (unsigned long long)fs.xarray_refresh_cycles,
        (unsigned long long)fs.xarray_load_calls,
        (unsigned long long)fs.xarray_load_cycles,
        (unsigned long long)fs.folio_lookup_calls,
        (unsigned long long)fs.folio_lookup_cycles,
        (unsigned long long)fs.buffer_lookup_calls,
        (unsigned long long)fs.buffer_lookup_cycles,
        (unsigned long long)fs.inode_find_calls,
        (unsigned long long)fs.inode_find_cycles,
        (unsigned long long)fs.inode_claim_calls,
        (unsigned long long)fs.inode_claim_cycles,
        (unsigned long long)fs.dentry_find_calls,
        (unsigned long long)fs.dentry_find_cycles,
        (unsigned long long)fs.dentry_claim_calls,
        (unsigned long long)fs.dentry_claim_cycles,
        (unsigned long long)fs.write_begin_calls,
        (unsigned long long)fs.write_begin_cycles,
        (unsigned long long)fs.write_end_calls,
        (unsigned long long)fs.write_end_cycles,
        (unsigned long long)fs.bio_submit_calls,
        (unsigned long long)fs.bio_preflush_calls,
        (unsigned long long)fs.bio_fua_calls,
        (unsigned long long)fs.bio_flush_op_calls,
        (unsigned long long)fs.plug_start_calls,
        (unsigned long long)fs.plug_finish_calls,
        (unsigned long long)fs.plug_queued_bios,
        (unsigned long long)fs.plug_max_queued_bios);
    printf("KOBOX_MEMORY_HOTPATH_PROFILE phase=%s kmalloc_calls=%llu kmalloc=%llu "
           "kzalloc_calls=%llu kzalloc=%llu kfree_calls=%llu kfree=%llu "
           "cache_alloc_calls=%llu cache_alloc=%llu cache_free_calls=%llu cache_free=%llu "
           "free_steps=%llu chunk_steps=%llu allocation_steps=%llu\n",
        phase,
        (unsigned long long)memory.kmalloc_calls,
        (unsigned long long)memory.kmalloc_cycles,
        (unsigned long long)memory.kzalloc_calls,
        (unsigned long long)memory.kzalloc_cycles,
        (unsigned long long)memory.kfree_calls,
        (unsigned long long)memory.kfree_cycles,
        (unsigned long long)memory.cache_alloc_calls,
        (unsigned long long)memory.cache_alloc_cycles,
        (unsigned long long)memory.cache_free_calls,
        (unsigned long long)memory.cache_free_cycles,
        (unsigned long long)memory.arena_free_search_steps,
        (unsigned long long)memory.arena_chunk_search_steps,
        (unsigned long long)memory.allocation_search_steps);
    printf("KOBOX_MODULE_HOTPATH_PROFILE phase=%s cross_module_calls=%llu\n",
        phase,
        (unsigned long long)kb_module_external_cross_call_profile_snapshot());
}
#define BENCHMARK_HOTPATH_BEGIN() benchmark_hotpath_profile_begin()
#define BENCHMARK_HOTPATH_END(phase) benchmark_hotpath_profile_end((phase))
#else
#define BENCHMARK_PROFILE_BEGIN(name) ((void)0)
#define BENCHMARK_PROFILE_END(phase, name) ((void)0)
#define BENCHMARK_HOTPATH_BEGIN() ((void)0)
#define BENCHMARK_HOTPATH_END(phase) ((void)0)
#endif

static void fill_block(unsigned char *block, size_t size)
{
    uint32_t state = 0x6b627831u;
    for (size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        block[i] = (unsigned char)(state >> 24);
    }
}

static int benchmark_error(const char *phase, int status)
{
    fprintf(stderr,
        "KOBOX_EXT4_NVME_BENCH_FAIL phase=%s status=%d\n",
        phase,
        status);
    return status != 0 ? status : -1;
}

static int release_object(koboxd_fs_backend_t *backend, uint64_t object_id)
{
    if (object_id == 0 || object_id == 1) {
        return 0;
    }
    return koboxd_fs_backend_release_object(backend, object_id);
}

int koboxd_run_ext4_nvme_benchmark(koboxd_fs_backend_t *backend)
{
    if (backend == NULL || !backend->mounted) {
        return benchmark_error("setup", -22);
    }

    if (benchmark_timer_init() != 0) {
        return benchmark_error("timer", -1);
    }

    unsigned char block[STORAGE_EXT4_NVME_BENCH_BLOCK_BYTES];
    char old_names[STORAGE_EXT4_NVME_BENCH_METADATA_FILES][16];
    char new_names[STORAGE_EXT4_NVME_BENCH_METADATA_FILES][16];
    fill_block(block, sizeof(block));
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        snprintf(old_names[i], sizeof(old_names[i]), "m%03u", i);
        snprintf(new_names[i], sizeof(new_names[i]), "r%03u", i);
    }

    printf("KOBOX_EXT4_NVME_BENCH_START\n");
    fflush(stdout);

    uint64_t read_dir = 0;
    uint64_t read_file = 0;
    BENCHMARK_PROFILE_BEGIN(cold_read_profile);
    BENCHMARK_HOTPATH_BEGIN();
    uint64_t started = benchmark_now_ns();
    int status = koboxd_fs_backend_lookup(
        backend, 1, STORAGE_EXT4_NVME_BENCH_READ_DIR, &read_dir);
    if (status == 0) {
        status = koboxd_fs_backend_lookup(
            backend,
            read_dir,
            STORAGE_EXT4_NVME_BENCH_READ_FILE,
            &read_file);
    }
    size_t read_bytes = 0;
    for (uint64_t offset = 0;
         status == 0 && offset < STORAGE_EXT4_NVME_BENCH_COLD_READ_BYTES;
         offset += sizeof(block))
    {
        status = koboxd_fs_backend_pread(
            backend, read_file, offset, block, sizeof(block), sizeof(block));
        if (status != (int)sizeof(block)) {
            status = status < 0 ? status : -5;
            break;
        }
        status = 0;
        read_bytes += sizeof(block);
    }
    if (status >= 0) {
        status = release_object(backend, read_file);
    }
    if (status == 0) {
        status = release_object(backend, read_dir);
    }
    const uint64_t cold_read_ns = benchmark_now_ns() - started;
    BENCHMARK_PROFILE_END("cold_read", cold_read_profile);
    BENCHMARK_HOTPATH_END("cold_read");
    if (status != 0 || read_bytes != STORAGE_EXT4_NVME_BENCH_COLD_READ_BYTES) {
        return benchmark_error("cold_read", status);
    }
    printf("KOBOX_EXT4_NVME_BENCH phase=cold_read bytes=%zu ns=%llu\n",
        read_bytes,
        (unsigned long long)cold_read_ns);

    uint64_t bench_dir = 0;
    status = koboxd_fs_backend_mkdir(
        backend, 1, STORAGE_EXT4_NVME_BENCH_DIR, 0755, &bench_dir);
    if (status != 0) {
        return benchmark_error("mkdir", status);
    }
    uint64_t stream_file = 0;
    status = koboxd_fs_backend_create(
        backend,
        bench_dir,
        STORAGE_EXT4_NVME_BENCH_STREAM_FILE,
        0644,
        &stream_file);
    if (status == 0) {
        status = release_object(backend, stream_file);
    }
    if (status != 0) {
        return benchmark_error("stream_create", status);
    }

    BENCHMARK_PROFILE_BEGIN(write_sync_profile);
    BENCHMARK_PROFILE_BEGIN(write_body_profile);
    BENCHMARK_HOTPATH_BEGIN();
    started = benchmark_now_ns();
    status = koboxd_fs_backend_lookup(
        backend,
        bench_dir,
        STORAGE_EXT4_NVME_BENCH_STREAM_FILE,
        &stream_file);
    for (uint64_t block_index = 0;
         status == 0 && block_index < STORAGE_EXT4_NVME_BENCH_STREAM_BLOCKS;
         ++block_index)
    {
        status = koboxd_fs_backend_pwrite(
            backend,
            stream_file,
            block_index * sizeof(block),
            block,
            sizeof(block));
        if (status != (int)sizeof(block)) {
            status = status < 0 ? status : -5;
        } else {
            status = 0;
        }
    }
    if (status != 0) {
        return benchmark_error("write_body", status);
    }
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    const uint64_t write_body_ns = benchmark_now_ns() - started;
#endif
    BENCHMARK_PROFILE_END("write_body", write_body_profile);
    BENCHMARK_HOTPATH_END("write_body");
    BENCHMARK_PROFILE_BEGIN(write_fsync_profile);
    BENCHMARK_HOTPATH_BEGIN();
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    const uint64_t write_fsync_started = benchmark_now_ns();
#endif
    status = koboxd_fs_backend_fsync(backend, stream_file);
    if (status != 0) {
        return benchmark_error("write_fsync", status);
    }
    BENCHMARK_PROFILE_END("write_fsync", write_fsync_profile);
    BENCHMARK_HOTPATH_END("write_fsync");
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    printf("KOBOX_EXT4_NVME_PROFILE_TIME phase=write_split body_ns=%llu fsync_ns=%llu\n",
        (unsigned long long)write_body_ns,
        (unsigned long long)(benchmark_now_ns() - write_fsync_started));
#endif
    status = release_object(backend, stream_file);
    if (status != 0) {
        return benchmark_error("write_release", status);
    }
    const uint64_t write_sync_ns = benchmark_now_ns() - started;
    BENCHMARK_PROFILE_END("write_sync", write_sync_profile);
    printf("KOBOX_EXT4_NVME_BENCH phase=write_sync bytes=%u ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_STREAM_BLOCKS * STORAGE_EXT4_NVME_BENCH_BLOCK_BYTES,
        (unsigned long long)write_sync_ns);

    status = koboxd_fs_backend_lookup(
        backend,
        bench_dir,
        STORAGE_EXT4_NVME_BENCH_STREAM_FILE,
        &stream_file);
    BENCHMARK_PROFILE_BEGIN(overwrite_fsync_profile);
    BENCHMARK_HOTPATH_BEGIN();
    uint64_t fsync_total_ns = 0;
    for (unsigned iteration = 0;
         status == 0 && iteration < STORAGE_EXT4_NVME_BENCH_FSYNC_ITERATIONS;
         ++iteration)
    {
        started = benchmark_now_ns();
        status = koboxd_fs_backend_pwrite(
            backend,
            stream_file,
            (uint64_t)iteration * sizeof(block),
            block,
            sizeof(block));
        if (status == (int)sizeof(block)) {
            status = koboxd_fs_backend_fsync(backend, stream_file);
        } else if (status >= 0) {
            status = -5;
        }
        fsync_total_ns += benchmark_now_ns() - started;
    }
    if (status == 0) {
        status = release_object(backend, stream_file);
    }
    if (status != 0) {
        return benchmark_error("overwrite_fsync", status);
    }
    BENCHMARK_PROFILE_END("overwrite_fsync", overwrite_fsync_profile);
    BENCHMARK_HOTPATH_END("overwrite_fsync");
    printf("KOBOX_EXT4_NVME_BENCH phase=overwrite_fsync iterations=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_FSYNC_ITERATIONS,
        (unsigned long long)fsync_total_ns,
        (unsigned long long)(fsync_total_ns / STORAGE_EXT4_NVME_BENCH_FSYNC_ITERATIONS));

    BENCHMARK_PROFILE_BEGIN(create_profile);
    BENCHMARK_HOTPATH_BEGIN();
    uint64_t create_total_ns = 0;
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        uint64_t object_id = 0;
        started = benchmark_now_ns();
        status = koboxd_fs_backend_create(
            backend, bench_dir, old_names[i], 0644, &object_id);
        create_total_ns += benchmark_now_ns() - started;
        if (status == 0) {
            status = release_object(backend, object_id);
        }
        if (status != 0) {
            return benchmark_error("create", status);
        }
    }
    BENCHMARK_PROFILE_END("create", create_profile);
    BENCHMARK_HOTPATH_END("create");
    printf("KOBOX_EXT4_NVME_BENCH phase=create files=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_METADATA_FILES,
        (unsigned long long)create_total_ns,
        (unsigned long long)(create_total_ns / STORAGE_EXT4_NVME_BENCH_METADATA_FILES));

    BENCHMARK_PROFILE_BEGIN(rename_profile);
    BENCHMARK_HOTPATH_BEGIN();
    uint64_t rename_total_ns = 0;
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        uint64_t object_id = 0;
        started = benchmark_now_ns();
        status = koboxd_fs_backend_rename(
            backend,
            bench_dir,
            old_names[i],
            bench_dir,
            new_names[i],
            &object_id);
        rename_total_ns += benchmark_now_ns() - started;
        if (status != 0) {
            return benchmark_error("rename", status);
        }
    }
    BENCHMARK_PROFILE_END("rename", rename_profile);
    BENCHMARK_HOTPATH_END("rename");
    printf("KOBOX_EXT4_NVME_BENCH phase=rename files=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_METADATA_FILES,
        (unsigned long long)rename_total_ns,
        (unsigned long long)(rename_total_ns / STORAGE_EXT4_NVME_BENCH_METADATA_FILES));

    BENCHMARK_PROFILE_BEGIN(unlink_profile);
    BENCHMARK_HOTPATH_BEGIN();
    uint64_t unlink_total_ns = 0;
    for (unsigned i = 0; i < STORAGE_EXT4_NVME_BENCH_METADATA_FILES; ++i) {
        started = benchmark_now_ns();
        status = koboxd_fs_backend_unlink(backend, bench_dir, new_names[i]);
        unlink_total_ns += benchmark_now_ns() - started;
        if (status != 0) {
            return benchmark_error("unlink", status);
        }
    }
    BENCHMARK_PROFILE_END("unlink", unlink_profile);
    BENCHMARK_HOTPATH_END("unlink");
    printf("KOBOX_EXT4_NVME_BENCH phase=unlink files=%u total_ns=%llu avg_ns=%llu\n",
        STORAGE_EXT4_NVME_BENCH_METADATA_FILES,
        (unsigned long long)unlink_total_ns,
        (unsigned long long)(unlink_total_ns / STORAGE_EXT4_NVME_BENCH_METADATA_FILES));

    BENCHMARK_PROFILE_BEGIN(syncfs_profile);
    BENCHMARK_HOTPATH_BEGIN();
    started = benchmark_now_ns();
    status = koboxd_fs_backend_sync_all(backend);
    const uint64_t syncfs_ns = benchmark_now_ns() - started;
    BENCHMARK_PROFILE_END("syncfs", syncfs_profile);
    BENCHMARK_HOTPATH_END("syncfs");
    if (status != 0) {
        return benchmark_error("syncfs", status);
    }
    printf("KOBOX_EXT4_NVME_BENCH phase=syncfs ns=%llu\n",
        (unsigned long long)syncfs_ns);

    status = koboxd_fs_backend_unlink(
        backend, bench_dir, STORAGE_EXT4_NVME_BENCH_STREAM_FILE);
    if (status == 0) {
        status = koboxd_fs_backend_rmdir(
            backend, 1, STORAGE_EXT4_NVME_BENCH_DIR);
    }
    if (status == 0) {
        status = release_object(backend, bench_dir);
    }
    if (status == 0) {
        status = koboxd_fs_backend_sync_all(backend);
    }
    if (status != 0) {
        return benchmark_error("cleanup", status);
    }

    printf("KOBOX_EXT4_NVME_BENCH_DONE\n");
    fflush(stdout);
    return 0;
}
