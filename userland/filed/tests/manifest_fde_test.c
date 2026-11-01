/* SPDX-License-Identifier: MIT */
#include "filed/manifest.h"
#include <patch_scan.h>
#include <elf.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct memory { const uint8_t *bytes; size_t size; };

static int memory_read(void *raw,uint64_t offset,uint8_t *destination,size_t length)
{
    const struct memory *m=raw;
    if (offset>m->size || length>m->size-offset) return -EIO;
    memcpy(destination,m->bytes+offset,length);
    return 0;
}

static void put32(uint8_t *p,uint32_t v) { memcpy(p,&v,4); }

enum { image_size=0x3000, function=0x1000, function_size=0x40,
    table=0x1800, table_size=300, candidate=280, hdr=0x2000, frame=0x2100 };

static void fill_ambiguous_table(uint8_t *bytes)
{
    uint32_t x=2654435761u|1;
    for (unsigned i=0;i<table_size;++i) {
        x^=x<<13;x^=x>>17;x^=x<<5;
        bytes[i]=(uint8_t)(x>>24);
    }
    for (unsigned i=0;i+1<table_size;++i)
        if (bytes[i]==0x0f && bytes[i+1]==0x05) bytes[i]=0x90;
    bytes[candidate]=0x0f;bytes[candidate+1]=0x05;
}

/* Minimal image: RX code page with one FDE-covered function and a constant
 * table, an R page holding .eh_frame_hdr/.eh_frame (zR CIE, pcrel|sdata4). */
static void build_image(uint8_t *raw,int with_fde)
{
    memset(raw,0,image_size);
    Elf64_Ehdr eh={0};
    memcpy(eh.e_ident,ELFMAG,SELFMAG);
    eh.e_ident[EI_CLASS]=ELFCLASS64;eh.e_ident[EI_DATA]=ELFDATA2LSB;
    eh.e_ident[EI_VERSION]=EV_CURRENT;
    eh.e_type=ET_DYN;eh.e_machine=EM_X86_64;eh.e_version=EV_CURRENT;
    eh.e_ehsize=sizeof(eh);eh.e_phoff=sizeof(eh);
    eh.e_phentsize=sizeof(Elf64_Phdr);eh.e_phnum=3;
    Elf64_Phdr ph[3]={
        {.p_type=PT_LOAD,.p_flags=PF_R|PF_X,.p_offset=0,.p_vaddr=0,
            .p_filesz=0x2000,.p_memsz=0x2000,.p_align=4096},
        {.p_type=PT_LOAD,.p_flags=PF_R,.p_offset=0x2000,.p_vaddr=0x2000,
            .p_filesz=0x1000,.p_memsz=0x1000,.p_align=4096},
        {.p_type=PT_GNU_EH_FRAME,.p_flags=PF_R,.p_offset=hdr,.p_vaddr=hdr,
            .p_filesz=20,.p_memsz=20,.p_align=4}};
    memcpy(raw,&eh,sizeof(eh));memcpy(raw+eh.e_phoff,ph,sizeof(ph));
    /* mov eax,39; syscall; ret, then NOP padding to the FDE end. */
    memset(raw+function,0x90,function_size);
    const uint8_t getpid[]={0xb8,39,0,0,0,0x0f,0x05,0xc3};
    memcpy(raw+function,getpid,sizeof(getpid));
    /* A constant table whose only 0F 05 is a SYSCALL on one resync path and
     * interior bytes on another (fixed xorshift stream, checked below). */
    fill_ambiguous_table(raw+table);
    uint8_t *h=raw+hdr;
    h[0]=1;h[1]=0x1b;h[2]=0x03;h[3]=0x3b;
    put32(h+4,(uint32_t)(frame-(hdr+4)));
    put32(h+8,1);
    uint8_t *f=raw+frame;
    const uint8_t cie[]={0,0,0,0, 0,0,0,0, 1,'z','R',0, 1,0x78,16, 1,0x1b, 0,0,0};
    memcpy(f,cie,sizeof(cie));put32(f,sizeof(cie)-4);
    size_t at=sizeof(cie);
    if (with_fde) {
        uint8_t *fde=f+at;
        put32(fde,16);
        put32(fde+4,(uint32_t)(at+4));
        put32(fde+8,(uint32_t)(function-(frame+at+8)));
        put32(fde+12,function_size);
        fde[16]=0;
        at+=20;
    }
    put32(f+at,0);
}

static void synthetic_test(void)
{
    static uint8_t raw[image_size];
    uint8_t uuid[16]={1},identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE]={0},scanner[32]={3};
    build_image(raw,1);
    struct memory m={raw,image_size};
    filed_rx_code_ranges_t code;
    assert(!filed_rx_code_ranges_load(memory_read,&m,image_size,&code));
    assert(code.count==1 && code.starts[0]==function &&
        code.ends[0]==function+function_size);
    assert(!filed_rx_code_ranges_outside(&code,function+5));
    assert(!filed_rx_code_ranges_outside(&code,function-1));
    assert(filed_rx_code_ranges_outside(&code,function-2));
    assert(filed_rx_code_ranges_outside(&code,table+candidate));
    {
        uint8_t copy[table_size];lpr_patch_scan_result_t scan;
        memcpy(copy,raw+table,table_size);
        assert(lpr_patch_scan_syscalls(copy,table_size,0,&scan)==-1);
    }

    /* Partial RX mapping as ld.so creates it: raw path, base 0x1000. */
    filed_rx_manifest_t man={0};
    assert(filed_rx_manifest_build(raw+0x1000,0x1000,0x1000,uuid,identity,
        scanner,NULL,&man)==-EIO);
    assert(!filed_rx_manifest_build(raw+0x1000,0x1000,0x1000,uuid,identity,
        scanner,&code,&man));
    uint8_t page[4096];
    memcpy(page,raw+0x1000,4096);
    assert(!filed_rx_manifest_apply(&man,0,page,4096,0,raw+0x1000,0x1000));
    assert(page[5]==0xff && page[6]==0xd0);
    assert(!memcmp(page+(table-0x1000),raw+table,table_size));
    filed_rx_manifest_free(&man);

    /* The same ambiguous table inside an FDE still rejects. */
    code.ends[0]=table+table_size;
    assert(filed_rx_manifest_build(raw+0x1000,0x1000,0x1000,uuid,identity,
        scanner,&code,&man)==-EIO);
    assert(!man.patches && !man.page_hashes);
    filed_rx_code_ranges_free(&code);

    /* Whole-image initial reservation (segment path) uses the same rule. */
    build_image(raw,1);
    uint64_t file_size=image_size;
    memcpy(identity+KB2_FILESYSTEM_SNAPSHOT_STAT_STATX_OFFSET+
        KB2_FILESYSTEM_STATX_SIZE_OFFSET,&file_size,sizeof(file_size));
    assert(!filed_rx_code_ranges_load(memory_read,&m,image_size,&code));
    assert(filed_rx_manifest_build(raw,image_size,0,uuid,identity,scanner,
        NULL,&man)==-EIO);
    assert(!filed_rx_manifest_build(raw,image_size,0,uuid,identity,scanner,
        &code,&man));
    filed_rx_manifest_free(&man);
    filed_rx_code_ranges_free(&code);

    /* No FDE at all is not evidence of data: the image keeps rejecting. */
    build_image(raw,0);
    assert(filed_rx_code_ranges_load(memory_read,&m,image_size,&code)==-ENOENT);
    /* A malformed CIE discards the table rather than guessing. */
    build_image(raw,1);
    raw[frame+9]='y';
    assert(filed_rx_code_ranges_load(memory_read,&m,image_size,&code)==-ENOEXEC);
    puts("RX FDE map: outside-FDE ambiguity kept as data, in-function and no-FDE rejection: PASS");
}

/* Real DSO: build every RX PT_LOAD as a separate range mapping (ld.so's
 * layout) with the FDE map, then check each FDE function against the exact
 * linear decode from its known start. */
static int dso_test(const char *path)
{
    FILE *file=fopen(path,"rb");
    if (!file) { perror(path);return 1; }
    fseek(file,0,SEEK_END);const long length=ftell(file);fseek(file,0,SEEK_SET);
    uint8_t *image=malloc((size_t)length);
    assert(image && fread(image,1,(size_t)length,file)==(size_t)length);
    fclose(file);
    struct memory m={image,(size_t)length};
    filed_rx_code_ranges_t code;
    assert(!filed_rx_code_ranges_load(memory_read,&m,(uint64_t)length,&code));
    const Elf64_Ehdr *eh=(const void *)image;
    const Elf64_Phdr *ph=(const void *)(image+eh->e_phoff);
    uint8_t uuid[16]={1},identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE]={2},scanner[32]={3};
    uint64_t checked=0,skipped_functions=0,misaligned=0,sites=0;
    for (unsigned i=0;i<eh->e_phnum;++i) {
        if (ph[i].p_type!=PT_LOAD || !(ph[i].p_flags&PF_X) || !ph[i].p_offset) continue;
        const uint8_t *seg=image+ph[i].p_offset;
        const size_t seg_size=(size_t)ph[i].p_filesz;
        filed_rx_manifest_t man={0};
        const int strict=filed_rx_manifest_build(seg,seg_size,ph[i].p_offset,uuid,
            identity,scanner,NULL,&man);
        filed_rx_manifest_free(&man);
        assert(!filed_rx_manifest_build(seg,seg_size,ph[i].p_offset,uuid,identity,
            scanner,&code,&man));
        uint8_t *patched=malloc(seg_size);
        assert(patched);
        memcpy(patched,seg,seg_size);
        for (size_t p=0;p<man.patch_count;++p) patched[man.patches[p].offset]=man.patches[p].value;
        sites+=man.patch_count/2;
        for (size_t r=0;r<code.count;++r) {
            if (code.starts[r]<ph[i].p_offset ||
                code.ends[r]>ph[i].p_offset+seg_size) continue;
            const size_t from=(size_t)(code.starts[r]-ph[i].p_offset),
                size=(size_t)(code.ends[r]-code.starts[r]);
            uint8_t *oracle=malloc(size);
            assert(oracle);
            memcpy(oracle,seg+from,size);
            lpr_patch_scan_result_t scan;
            if (lpr_patch_scan_syscalls(oracle,size,LPR_PATCH_SCAN_START_BOUNDARY,&scan)) {
                ++skipped_functions;free(oracle);continue;
            }
            /* Every SYSCALL the exact decode finds must be patched. The
             * reverse can differ: glibc's __restore_rt FDE deliberately
             * starts one byte early, so that decode is misaligned there. */
            for (size_t b=0;b<size;++b) {
                if (oracle[b]!=seg[from+b] && patched[from+b]!=oracle[b]) {
                    fprintf(stderr,"missed site path=%s offset=%#" PRIx64 "\n",
                        path,code.starts[r]+b);
                    return 1;
                }
            }
            if (memcmp(oracle,patched+from,size)) ++misaligned;
            ++checked;free(oracle);
        }
        printf("%s segment offset=%#" PRIx64 " strict=%d sites=%" PRIu64 "\n",
            path,(uint64_t)ph[i].p_offset,strict,man.patch_count/2);
        free(patched);filed_rx_manifest_free(&man);
    }
    printf("%s fde_ranges=%zu functions_checked=%" PRIu64
        " functions_with_inline_data=%" PRIu64 " misaligned_fde_starts=%" PRIu64 " PASS\n",
        path,code.count,checked,skipped_functions,misaligned);
    (void)sites;
    filed_rx_code_ranges_free(&code);free(image);
    return 0;
}

int main(int argc,char **argv)
{
    synthetic_test();
    for (int i=1;i<argc;++i) if (dso_test(argv[i])) return 1;
    return 0;
}
