#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Opt-in diagnostic preload only. Map live executable segments to modules
 * for host-side vCPU snapshots. No call interception, timer or signal handler.
 * Constructor output precedes measured warm navigations; addresses alone do
 * not identify a process when address spaces overlap. */
static int module(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    unsigned *records = data;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || !p->p_memsz)
            continue;
        if (*records >= 256) return 1;
        ++*records;
        uintptr_t start = info->dlpi_addr + p->p_vaddr;
        char line[768];
        int n = snprintf(line, sizeof(line), "PAGE_XMAP pid=%ld base=%#lx start=%#lx end=%#lx name=%.512s\n",
            (long)getpid(), (unsigned long)info->dlpi_addr,
            (unsigned long)start, (unsigned long)(start + p->p_memsz),
            info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "<main>");
        /* One write keeps concurrent child-process map records together. */
        if (n > 0 && (size_t)n < sizeof(line)) (void)write(2, line, (size_t)n);
    }
    return 0;
}

__attribute__((constructor)) static void loaded(void)
{
    int saved = errno;
    unsigned records = 0;
    char line[96];
    int n = snprintf(line, sizeof(line), "PAGE_XMAP_BEGIN pid=%ld\n", (long)getpid());
    if (n > 0 && (size_t)n < sizeof(line)) (void)write(2, line, (size_t)n);
    dl_iterate_phdr(module, &records);
    errno = saved;
}
