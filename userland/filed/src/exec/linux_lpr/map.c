#include "internal.h"
#include "relocate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "pacha/trace.h"
typedef struct lpr_exec_map_metric_record {
    const char *name;
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t total_bytes;
} lpr_exec_map_metric_record_t;

enum {
    LPR_EXEC_MAP_METRIC_SLOTS = 64,
};

static lpr_exec_map_metric_record_t lpr_exec_map_metrics[LPR_EXEC_MAP_METRIC_SLOTS];

static uint64_t lpr_exec_map_now_cycles(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static lpr_exec_map_metric_record_t *lpr_exec_map_metric_slot(const char *label)
{
    if (label == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < LPR_EXEC_MAP_METRIC_SLOTS; ++i) {
        if (lpr_exec_map_metrics[i].name == label ||
            (lpr_exec_map_metrics[i].name != NULL && strcmp(lpr_exec_map_metrics[i].name, label) == 0))
        {
            return &lpr_exec_map_metrics[i];
        }
    }
    for (size_t i = 0; i < LPR_EXEC_MAP_METRIC_SLOTS; ++i) {
        if (lpr_exec_map_metrics[i].name == NULL) {
            lpr_exec_map_metrics[i].name = label;
            return &lpr_exec_map_metrics[i];
        }
    }
    return NULL;
}

static void lpr_exec_map_metric_time_cycles(
    const char *label,
    uint64_t start_cycles,
    uint64_t end_cycles,
    uint64_t bytes)
{
    if (start_cycles == 0 || end_cycles < start_cycles) {
        return;
    }
    lpr_exec_map_metric_record_t *metric = lpr_exec_map_metric_slot(label);
    if (metric == NULL) {
        return;
    }
    const uint64_t elapsed_cycles = end_cycles - start_cycles;
    metric->count++;
    metric->total_cycles += elapsed_cycles;
    if (elapsed_cycles > metric->max_cycles) {
        metric->max_cycles = elapsed_cycles;
    }
    metric->total_bytes += bytes;
}

void lpr_exec_map_dump_metrics(void)
{
    for (size_t i = 0; i < LPR_EXEC_MAP_METRIC_SLOTS; ++i) {
        const lpr_exec_map_metric_record_t *metric = &lpr_exec_map_metrics[i];
        if (metric->name == NULL || metric->count == 0) {
            continue;
        }
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_EXEC_METRIC,
            PACHA_TRACE_CLASS_METRIC,
            pacha_trace_name_id(metric->name),
            metric->count,
            metric->total_ns / metric->count,
            metric->max_ns,
            metric->total_cycles / metric->count,
            metric->max_cycles);
        pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_EXEC_METRIC, PACHA_TRACE_CLASS_METRIC, pacha_trace_name_id(metric->name), metric->total_bytes);
    }
}

static int fill_vmo(int vmo_fd, const void *initial_bytes, uint64_t initial_size, uint64_t map_size)
{
    if (initial_size == 0) {
        return 0;
    }
    if (initial_bytes == NULL || initial_size > map_size) {
        return -22;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        return -12;
    }
    memcpy(mapped, initial_bytes, (size_t)initial_size);
    (void)pacha_munmap(mapped, map_size);
    return 0;
}

static int create_segment_vmo(uint64_t map_size, int *out_vmo_fd, unsigned char **out_mapped)
{
    if (out_vmo_fd == NULL || out_mapped == NULL || map_size == 0 || map_size > SIZE_MAX) {
        return -22;
    }
    *out_vmo_fd = -1;
    *out_mapped = NULL;

    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    uint64_t metric_start = lpr_exec_map_now_cycles();
    const int vmo_fd = pacha_vmo_create(map_size, rights, 0);
    lpr_exec_map_metric_time_cycles(
        "segment_vmo_create_syscall",
        metric_start,
        lpr_exec_map_now_cycles(),
        map_size);
    if (vmo_fd < 16) {
        return -12;
    }
    metric_start = lpr_exec_map_now_cycles();
    unsigned char *mapped = pacha_mmap(
        vmo_fd,
        map_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    lpr_exec_map_metric_time_cycles(
        "segment_vmo_mmap_syscall",
        metric_start,
        lpr_exec_map_now_cycles(),
        map_size);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }

    *out_vmo_fd = vmo_fd;
    *out_mapped = mapped;
    return 0;
}

static int build_zpoline_page(unsigned char *page, uint64_t handler_va)
{
    if (page == NULL || handler_va == 0) {
        return -22;
    }
    memset(page, LPR_ZPOLINE_NOP_BYTE, LPR_ZPOLINE_PAGE_SIZE);
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_POP_RCX_OFFSET] =
        LPR_ZPOLINE_SHIM_POP_RCX_BYTE;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_MOVABS_R11_OFFSET] =
        LPR_ZPOLINE_SHIM_MOVABS_R11_BYTE0;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_MOVABS_R11_OFFSET + 1u] =
        LPR_ZPOLINE_SHIM_MOVABS_R11_BYTE1;
    lpr_exec_wr64(
        page + LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_HANDLER_VA_OFFSET,
        handler_va);
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_JMP_R11_OFFSET] =
        LPR_ZPOLINE_SHIM_JMP_R11_BYTE0;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_JMP_R11_OFFSET + 1u] =
        LPR_ZPOLINE_SHIM_JMP_R11_BYTE1;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_JMP_R11_OFFSET + 2u] =
        LPR_ZPOLINE_SHIM_JMP_R11_BYTE2;
    return 0;
}

static int zpoline_page_vmo(uint64_t handler_va)
{
    static int cached_vmo_fd = -1;
    static uint64_t cached_handler_va = 0;

    if (handler_va == 0) {
        return -22;
    }
    if (cached_vmo_fd >= 16 && cached_handler_va == handler_va) {
        return cached_vmo_fd;
    }
    if (cached_vmo_fd >= 16) {
        (void)pacha_fd_close(cached_vmo_fd);
        cached_vmo_fd = -1;
        cached_handler_va = 0;
    }

    unsigned char zpoline[LPR_ZPOLINE_PAGE_SIZE];
    int status = build_zpoline_page(zpoline, handler_va);
    if (status != 0) {
        return status;
    }

    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(LPR_ZPOLINE_PAGE_SIZE, rights, 0);
    if (vmo_fd < 16) {
        return -12;
    }
    status = fill_vmo(vmo_fd, zpoline, sizeof(zpoline), LPR_ZPOLINE_PAGE_SIZE);
    if (status != 0) {
        (void)pacha_fd_close(vmo_fd);
        return status;
    }
    cached_vmo_fd = vmo_fd;
    cached_handler_va = handler_va;
    return cached_vmo_fd;
}

int lpr_exec_install_low_layout(int process_fd, uint64_t syscall_entry_va)
{
    const int vmo_fd = zpoline_page_vmo(syscall_entry_va);
    if (vmo_fd < 16) {
        return vmo_fd;
    }
    const long map_result = pacha_process_map(
        process_fd,
        vmo_fd,
        LPR_ZPOLINE_PAGE_VA,
        LPR_ZPOLINE_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_EXEC,
        0);
    return (uint64_t)map_result == LPR_ZPOLINE_PAGE_VA ? 0 : -12;
}


static int phdr_va_from_phdrs(
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t phoff,
    uint64_t *out_phdr_va)
{
    if (out_phdr_va == NULL) {
        return -22;
    }
    uint64_t phdr_va = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(ph) == LPR_EXEC_PT_PHDR) {
            phdr_va = lpr_exec_rd64(ph + 16);
            break;
        }
    }
    if (phdr_va == 0) {
        const uint64_t phdr_bytes = (uint64_t)phnum * (uint64_t)phent;
        for (uint16_t i = 0; i < phnum; ++i) {
            const unsigned char *ph = phdrs + (uint64_t)i * phent;
            if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
                continue;
            }
            const uint64_t p_offset = lpr_exec_rd64(ph + 8);
            const uint64_t p_vaddr = lpr_exec_rd64(ph + 16);
            const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
            if (phoff >= p_offset &&
                phoff - p_offset <= p_filesz &&
                phdr_bytes <= p_filesz - (phoff - p_offset))
            {
                const uint64_t phdr_delta = phoff - p_offset;
                if (p_vaddr > UINT64_MAX - phdr_delta) {
                    return -75;
                }
                phdr_va = p_vaddr + phdr_delta;
                break;
            }
        }
    }
    if (phdr_va == 0) {
        return -8;
    }
    *out_phdr_va = phdr_va;
    return 0;
}

static int load_bias_for_type(uint16_t type, uint64_t dyn_base, uint64_t *out_load_bias)
{
    if (out_load_bias == NULL) {
        return -22;
    }
    *out_load_bias = type == LPR_EXEC_ELF_TYPE_DYN ? dyn_base : 0;
    return 0;
}

static int set_loaded_result(
    lpr_exec_loaded_t *loaded,
    uint64_t entry,
    uint64_t load_bias,
    uint64_t phdr_va,
    uint16_t phent,
    uint16_t phnum,
    uint16_t load_segments)
{
    if (loaded == NULL || load_segments == 0) {
        return -8;
    }
    loaded->entry = entry + load_bias;
    loaded->base = load_bias;
    loaded->phdr_va = phdr_va + load_bias;
    loaded->phent = phent;
    loaded->phnum = phnum;
    loaded->load_segments = load_segments;
    return 0;
}

static int validate_loaded_meta(uint16_t type, uint16_t phent, uint16_t phnum)
{
    if ((type != LPR_EXEC_ELF_TYPE_DYN && type != LPR_EXEC_ELF_TYPE_EXEC) ||
        phnum == 0 ||
        phent < LPR_EXEC_PHDR_BYTES)
    {
        return -8;
    }
    return 0;
}


void lpr_exec_pending_map_batch_init(lpr_exec_pending_map_batch_t *batch)
{
    memset(batch, 0, sizeof(*batch));
}
void lpr_exec_pending_map_batch_discard(lpr_exec_pending_map_batch_t *batch)
{
    if (!batch) return;
    for (uint64_t i = 0; i < batch->count; ++i)
        if (batch->close_fds[i] >= 16) (void)pacha_fd_close(batch->close_fds[i]);
    free(batch->entries);
    free(batch->close_fds);
    memset(batch, 0, sizeof(*batch));
}
int lpr_exec_pending_map_batch_commit(int process_fd, lpr_exec_pending_map_batch_t *batch)
{
    if (!batch || process_fd < 16) return -22;
    int status = 0;
    for (uint64_t done = 0; done < batch->count;) {
        uint64_t count = batch->count - done;
        if (count > PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES)
            count = PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES;
        status = pacha_process_map_batch(process_fd, batch->entries + done, count);
        if (status) break;
        done += count;
    }
    lpr_exec_pending_map_batch_discard(batch);
    return status;
}
static int append_map(lpr_exec_pending_map_batch_t *batch, int vmo,
                      uint64_t target, uint64_t length, uint64_t offset,
                      uint64_t protection)
{
    if (batch->count == batch->capacity) {
        uint64_t capacity = batch->capacity ? batch->capacity * 2 : 16;
        if (capacity < batch->capacity ||
            capacity > SIZE_MAX / sizeof(*batch->entries) ||
            capacity > SIZE_MAX / sizeof(*batch->close_fds)) return -75;
        void *entries = realloc(batch->entries, (size_t)capacity * sizeof(*batch->entries));
        if (!entries) return -12;
        batch->entries = entries;
        void *fds = realloc(batch->close_fds, (size_t)capacity * sizeof(*batch->close_fds));
        if (!fds) return -12;
        batch->close_fds = fds;
        batch->capacity = capacity;
    }
    /* DUP takes explicit attenuated rights, not zero-as-inherit. The queued
     * reference needs its exact segment protections and cleanup authority. */
    uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE;
    if (protection & PACHA_PROT_READ) rights |= PACHA_FD_RIGHT_MAP_READ;
    if (protection & PACHA_PROT_WRITE) rights |= PACHA_FD_RIGHT_MAP_WRITE;
    if (protection & PACHA_PROT_EXEC) rights |= PACHA_FD_RIGHT_MAP_EXEC;
    long duplicate = pacha_fd_fcntl(vmo, PACHA_FD_FCNTL_DUP, 16, rights);
    if (duplicate < 16) return duplicate < 0 ? (int)duplicate : -1;
    int copy = (int)duplicate;
    batch->entries[batch->count] = (struct pacha_process_map_batch_entry){
        .vmo_fd = (uint64_t)copy, .target_va = target, .size = length,
        .prot = protection, .vmo_offset = offset,
        .flags = PACHA_PROCESS_MAP_PRIVATE };
    batch->close_fds[batch->count++] = copy;
    return 0;
}
/* Build an exec-local image from the current Linux file, then retain only
 * the child mappings. No VMO, map plan or file bytes survive this exec. */
static int prepare_image(const lpr_exec_image_t *image, uint64_t dyn_base,
                          int patch_text, int relocate_runtime,
                          lpr_exec_pending_map_batch_t *batch,
                          lpr_exec_loaded_t *loaded)
{
    int status = lpr_exec_validate_elf(image);
    if (status) return status;
    const unsigned char *ehdr = image->bytes;
    uint16_t type = lpr_exec_rd16(ehdr + 16), phent = lpr_exec_rd16(ehdr + 54);
    uint16_t phnum = lpr_exec_rd16(ehdr + 56);
    uint64_t phoff = lpr_exec_rd64(ehdr + 32), bias = 0, phdr_va = 0;
    const unsigned char *phdrs = image->bytes + phoff;
    status = validate_loaded_meta(type, phent, phnum);
    if (!status) status = load_bias_for_type(type, dyn_base, &bias);
    if (!status) status = phdr_va_from_phdrs(phdrs, phnum, phent, phoff, &phdr_va);
    if (status) return status;
    uint64_t low = UINT64_MAX, high = 0;
    uint16_t segments = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *p = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(p) != LPR_EXEC_PT_LOAD) continue;
        uint64_t offset = lpr_exec_rd64(p + 8), va = lpr_exec_rd64(p + 16);
        uint64_t filesz = lpr_exec_rd64(p + 32), memsz = lpr_exec_rd64(p + 40);
        if (filesz > memsz || offset > image->size || filesz > image->size-offset ||
            va > UINT64_MAX - memsz || va > UINT64_MAX - bias ||
            (va & 4095) != (offset & 4095)) return -8;
        if (!memsz) continue;
        uint64_t end;
        status = lpr_exec_align_up(va + memsz, &end);
        if (status) return status;
        if (end > UINT64_MAX - bias) return -8;
        uint64_t begin = lpr_exec_align_down(va);
        if (begin < low) low = begin;
        if (end > high) high = end;
        ++segments;
    }
    if (!segments || high <= low || high-low > SIZE_MAX) return -8;
    uint64_t span = high-low, pages = span / LPR_IMAGE_PAGE_SIZE;
    if (pages > SIZE_MAX / sizeof(uint64_t)) return -75;
    uint64_t *protections = calloc((size_t)pages, sizeof(*protections));
    if (!protections) return -12;
    int vmo = -1;
    unsigned char *mapped = NULL;
    status = create_segment_vmo(span, &vmo, &mapped);
    if (status) { free(protections); return status; }
    memset(mapped, 0, (size_t)span);
    uint64_t textoff = 0, textsize = 0;
    if (patch_text) {
        status = lpr_exec_image_find_text_section(image, &textoff, &textsize);
        if (status) goto done;
    }
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *p = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(p) != LPR_EXEC_PT_LOAD) continue;
        uint64_t off = lpr_exec_rd64(p+8), va = lpr_exec_rd64(p+16);
        uint64_t filesz = lpr_exec_rd64(p+32), memsz = lpr_exec_rd64(p+40);
        if (!memsz) continue;
        uint64_t end = 0;
        status = lpr_exec_align_up(va + memsz, &end);
        if (status) goto done;
        uint64_t prot = lpr_exec_prot_from_elf_flags(lpr_exec_rd32(p+4));
        /* The present bit distinguishes a mapped PROT_NONE page from an ELF
         * hole; later PT_LOAD mappings own overlapping page protections. */
        for (uint64_t page = (lpr_exec_align_down(va)-low)/4096;
             page < (end-low)/4096; ++page) protections[page] = prot | (1ull<<63);
        if (filesz) memcpy(mapped + va-low, image->bytes + off, (size_t)filesz);
        memset(mapped + va-low + filesz, 0, (size_t)(memsz-filesz));
        if (patch_text && textsize && (prot & PACHA_PROT_EXEC)) {
            uint64_t from = off > textoff ? off : textoff;
            uint64_t until = off+filesz < textoff+textsize ? off+filesz : textoff+textsize;
            if (until > from) {
                uint64_t patched = 0;
                status = lpr_exec_patch_syscalls(mapped + va-low + from-off,
                                                 until-from, &patched);
                if (status) goto done;
            }
        }
    }
    if (relocate_runtime) {
        status=lpr_exec_relocate_runtime(mapped,span,low+bias,bias,
            phdrs,phnum,phent);
        if(status) goto done;
    }
    for (uint64_t page = 0; page < pages;) {
        if (!protections[page]) { ++page; continue; }
        uint64_t end = page+1;
        while (end < pages && protections[end] == protections[page]) ++end;
        status = append_map(batch, vmo, low+bias+page*4096,
                            (end-page)*4096, page*4096,
                            protections[page] & ~(1ull<<63));
        if (status) goto done;
        page = end;
    }
    status = set_loaded_result(loaded, lpr_exec_rd64(ehdr+24), bias, phdr_va,
                                phent, phnum, segments);
done:
    (void)pacha_munmap(mapped, span);
    (void)pacha_fd_close(vmo);
    free(protections);
    return status;
}
int lpr_exec_prepare_file_into_map_batch(filed_runtime_t *runtime,
    const lpr_exec_file_t *file, const lpr_exec_meta_t *meta, uint64_t dyn_base,
    int patch_text, lpr_exec_pending_map_batch_t *batch, lpr_exec_loaded_t *loaded)
{
    if (!runtime || !file || !meta || !batch || !loaded) return -22;
    lpr_exec_image_t image = {0};
    int status = lpr_exec_read_full_file_image(runtime, file, &image);
    if (!status) status = prepare_image(&image, dyn_base, patch_text, 0, batch, loaded);
    free(image.bytes);
    return status;
}
int lpr_exec_load_image_into_process(int process_fd, const lpr_exec_image_t *image,
    uint64_t dyn_base, int patch_text, lpr_exec_loaded_t *loaded)
{
    lpr_exec_pending_map_batch_t batch;
    lpr_exec_pending_map_batch_init(&batch);
    int status = prepare_image(image, dyn_base, patch_text, 0, &batch, loaded);
    if (!status) status = lpr_exec_pending_map_batch_commit(process_fd, &batch);
    else lpr_exec_pending_map_batch_discard(&batch);
    return status;
}
int lpr_exec_load_image_with_low_layout_into_process(int process_fd,
    const lpr_exec_image_t *image, uint64_t dyn_base, int patch_text,
    uint64_t syscall_entry_offset, lpr_exec_loaded_t *loaded)
{
    lpr_exec_pending_map_batch_t batch;
    lpr_exec_pending_map_batch_init(&batch);
    int status=prepare_image(image,dyn_base,patch_text,1,&batch,loaded);
    if(!status) status=lpr_exec_pending_map_batch_commit(process_fd,&batch);
    else lpr_exec_pending_map_batch_discard(&batch);
    if (status) return status;
    if (syscall_entry_offset > UINT64_MAX - loaded->base) return -75;
    return lpr_exec_install_low_layout(process_fd, loaded->base + syscall_entry_offset);
}
int lpr_exec_load_file_into_process(filed_runtime_t *runtime, int process_fd,
    const lpr_exec_file_t *file, const lpr_exec_meta_t *meta, uint64_t dyn_base,
    int patch_text, lpr_exec_loaded_t *loaded)
{
    lpr_exec_pending_map_batch_t batch;
    lpr_exec_pending_map_batch_init(&batch);
    int status = lpr_exec_prepare_file_into_map_batch(runtime,file,meta,dyn_base,
                                                      patch_text,&batch,loaded);
    if (!status) status = lpr_exec_pending_map_batch_commit(process_fd,&batch);
    else lpr_exec_pending_map_batch_discard(&batch);
    return status;
}
