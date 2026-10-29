/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "filed/manifest.h"
#include <kobox2/sha256.h>
#include <patch_scan.h>
#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW,&ts);
    return (uint64_t)ts.tv_sec*1000000000u+ts.tv_nsec;
}

static int segment(FILE *file,const char *path,const Elf64_Phdr *ph,unsigned phnum,int whole)
{
    const uint64_t offset=whole ? 0 : ph->p_offset;
    uint64_t bytes=ph->p_filesz;
    if (whole) {
        if (fseeko(file,0,SEEK_END)) return -1;
        off_t end=ftello(file);
        if (end<0) return -1;
        bytes=(uint64_t)end;
    }
    if (bytes<2 || bytes>SIZE_MAX || fseeko(file,(off_t)offset,SEEK_SET)) return -1;
    size_t length=(size_t)bytes;
    uint8_t *raw=malloc(length),*expected=malloc(length);
    if (!raw || !expected || fread(raw,1,length,file)!=length) {
        free(raw);free(expected);return -1;
    }
    memcpy(expected,raw,length);
    lpr_patch_scan_result_t scan={0};
    uint64_t sites=0;
    if (!whole) {
        if (lpr_patch_scan_syscalls(expected,length,0,&scan)) {
            free(raw);free(expected);return -1;
        }
        sites=scan.patched_sites;
    } else {
        /* Independent oracle over actual code sections, comparing every byte
         * of the initial mapping, including RX unwind data and later RW data. */
        Elf64_Ehdr eh;
        memcpy(&eh,raw,sizeof(eh));
        if (eh.e_shnum) {
            if (eh.e_shentsize!=sizeof(Elf64_Shdr) || eh.e_shoff>length ||
                eh.e_shnum>(length-eh.e_shoff)/sizeof(Elf64_Shdr)) return -1;
            for (unsigned i=0;i<eh.e_shnum;++i) {
                Elf64_Shdr sh;
                memcpy(&sh,raw+eh.e_shoff+(size_t)i*sizeof(sh),sizeof(sh));
                if (!(sh.sh_flags&SHF_EXECINSTR) || !sh.sh_size) continue;
                if (sh.sh_offset>length || sh.sh_size>length-sh.sh_offset) return -1;
                /* Deliberately use the opposite order from FileD: candidate
                 * context first, known-start linear decoding only if needed.
                 * Compare every byte, including inline assembly data. */
                if (lpr_patch_scan_syscalls(expected+sh.sh_offset,sh.sh_size,0,&scan)) {
                    memcpy(expected+sh.sh_offset,raw+sh.sh_offset,sh.sh_size);
                    if (lpr_patch_scan_syscalls(expected+sh.sh_offset,sh.sh_size,
                        LPR_PATCH_SCAN_START_BOUNDARY,&scan)) return -1;
                }
                sites+=scan.patched_sites;
            }
        } else for (unsigned i=0;i<phnum;++i) {
            if (ph[i].p_type!=PT_LOAD || !(ph[i].p_flags&PF_X)) continue;
            if (ph[i].p_offset>length || ph[i].p_filesz>length-ph[i].p_offset ||
                lpr_patch_scan_syscalls(expected+ph[i].p_offset,ph[i].p_filesz,0,&scan)) return -1;
            sites+=scan.patched_sites;
        }
    }
    uint8_t uuid[16]={1},identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE]={2},
        scanner[32]={3};
    if (whole) memcpy(identity+KB2_FILESYSTEM_SNAPSHOT_STAT_STATX_OFFSET+
        KB2_FILESYSTEM_STATX_SIZE_OFFSET,&bytes,sizeof(bytes));
    filed_rx_manifest_t manifest={0};
    if (filed_rx_manifest_build(raw,length,offset,uuid,identity,
            scanner,&manifest)) {
        free(raw);free(expected);return -1;
    }
    uint64_t hash_ns=0,apply_ns=0;
    for (uint64_t page=0;page<manifest.page_count;++page) {
        uint8_t patched[4096]={0};
        const size_t start=(size_t)page*4096;
        size_t n=length-start;
        if (n>4096) n=4096;
        memcpy(patched,raw+start,n);
        const uint64_t t0=now_ns();
        if (filed_rx_manifest_apply(&manifest,start,patched,4096,
                0,raw,length)) {
            filed_rx_manifest_free(&manifest);
            free(raw);free(expected);return -1;
        }
        apply_ns+=now_ns()-t0;
        if (memcmp(patched,expected+start,n)) {
            fprintf(stderr,"manifest mismatch path=%s page=%" PRIu64 "\n",path,page);
            filed_rx_manifest_free(&manifest);
            free(raw);free(expected);return -1;
        }
        size_t begin=start<FILED_RX_GUARD_BYTES ? 0 : start-FILED_RX_GUARD_BYTES;
        size_t end=start+4096+FILED_RX_GUARD_BYTES;
        if (end>length) end=length;
        uint8_t hash[32];
        const uint64_t h0=now_ns();
        kb2_sha256(raw+begin,end-begin,hash);
        hash_ns+=now_ns()-h0;
    }
    printf("%s whole=%d offset=%" PRIu64 " bytes=%zu pages=%" PRIu64
        " sites=%" PRIu64 " changed_bytes=%" PRIu64
        " hash_ns_per_page=%" PRIu64 " apply_ns_per_page=%" PRIu64 " PASS\n",
        path,whole,offset,length,manifest.page_count,
        sites,manifest.patch_count,
        hash_ns/manifest.page_count,apply_ns/manifest.page_count);
    filed_rx_manifest_free(&manifest);
    free(raw);free(expected);
    return 0;
}

int main(int argc,char **argv)
{
    if (argc<2) return 2;
    const int whole=!strcmp(argv[1],"--whole-image");
    if (whole && argc<3) return 2;
    for (int arg=1+whole;arg<argc;++arg) {
        FILE *file=fopen(argv[arg],"rb");
        if (!file) { perror(argv[arg]);return 1; }
        Elf64_Ehdr header;
        if (fread(&header,1,sizeof(header),file)!=sizeof(header) ||
            memcmp(header.e_ident,ELFMAG,SELFMAG) ||
            header.e_ident[EI_CLASS]!=ELFCLASS64 ||
            header.e_phentsize!=sizeof(Elf64_Phdr)) return 1;
        if (fseeko(file,(off_t)header.e_phoff,SEEK_SET)) return 1;
        Elf64_Phdr *ph=calloc(header.e_phnum,sizeof(*ph));
        if (!ph || fread(ph,sizeof(*ph),header.e_phnum,file)!=header.e_phnum)
            return 1;
        unsigned segments=0;
        for (unsigned i=0;i<header.e_phnum;++i)
            if (ph[i].p_type==PT_LOAD && (ph[i].p_flags&PF_X)) {
                if (!whole && segment(file,argv[arg],&ph[i],1,0)) return 1;
                ++segments;
            }
        if (!segments) return 1;
        if (whole && segment(file,argv[arg],ph,header.e_phnum,1)) return 1;
        free(ph);fclose(file);
    }
    return 0;
}
