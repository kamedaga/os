/* SPDX-License-Identifier: MIT */
#include "filed/manifest.h"
#include <patch_scan.h>
#include <elf.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void elf_snapshot_test(void)
{
    enum { size=8192,code_size=1024 };
    uint8_t raw[size],actual[size],saved[size];
    memset(raw,0,sizeof(raw));
    memset(raw+256,0x90,code_size-256);
    Elf64_Ehdr eh={0};
    memcpy(eh.e_ident,ELFMAG,SELFMAG);
    eh.e_ident[EI_CLASS]=ELFCLASS64;eh.e_ident[EI_DATA]=ELFDATA2LSB;
    eh.e_ident[EI_VERSION]=EV_CURRENT;
    eh.e_type=ET_DYN;eh.e_machine=EM_X86_64;eh.e_version=EV_CURRENT;
    eh.e_ehsize=sizeof(eh);eh.e_phoff=sizeof(eh);
    eh.e_phentsize=sizeof(Elf64_Phdr);eh.e_phnum=2;
    Elf64_Phdr loads[2]={
        {.p_type=PT_LOAD,.p_flags=PF_R|PF_X,.p_offset=0,.p_vaddr=0,
            .p_filesz=code_size,.p_memsz=code_size,.p_align=4096},
        {.p_type=PT_LOAD,.p_flags=PF_R|PF_W,.p_offset=4096,.p_vaddr=8192,
            .p_filesz=4096,.p_memsz=8192,.p_align=4096}};
    memcpy(raw,&eh,sizeof(eh));memcpy(raw+eh.e_phoff,loads,sizeof(loads));
    raw[512]=0x0f;raw[513]=0x05;
    /* Decodable data must also remain data: the raw scanner would patch this
     * NOP-surrounded byte sequence as a syscall, silently corrupting it. */
    memset(raw+4096,0x90,4096);raw[5000]=0x0f;raw[5001]=0x05;
    memcpy(saved,raw,size);
    uint8_t uuid[16]={1},identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE]={0},scanner[32]={3};
    uint64_t file_size=size;
    memcpy(identity+KB2_FILESYSTEM_SNAPSHOT_STAT_STATX_OFFSET+
        KB2_FILESYSTEM_STATX_SIZE_OFFSET,&file_size,sizeof(file_size));
    filed_rx_manifest_t m={0};
    assert(!filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m));
    memcpy(actual,raw,size);
    assert(!filed_rx_manifest_apply(&m,0,actual,size,0,raw,size));
    assert(actual[512]==0xff && actual[513]==0xd0);
    assert(!memcmp(actual+code_size,saved+code_size,size-code_size));
    raw[5000]^=1;
    assert(filed_rx_manifest_apply(&m,4096,actual+4096,4096,0,raw,size)==-EAGAIN);
    memcpy(raw,saved,size);filed_rx_manifest_free(&m);

    /* The ELF optimization must not hide an ambiguous executable sequence. */
    memset(raw+256,0,code_size-256);raw[512]=0x0f;raw[513]=0x05;
    assert(filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m)==-EIO);
    assert(!m.patches && !m.page_hashes);
    memcpy(raw,saved,size);

    /* Mid-file and partial-file snapshots retain the ordinary raw scanner. */
    assert(!filed_rx_manifest_build(raw+4096,4096,4096,uuid,identity,scanner,&m));
    memcpy(actual,raw+4096,4096);
    assert(!filed_rx_manifest_apply(&m,0,actual,4096,0,raw+4096,4096));
    assert(actual[5000-4096]==0xff && actual[5001-4096]==0xd0);
    filed_rx_manifest_free(&m);
    assert(!filed_rx_manifest_build(raw,size-1,0,uuid,identity,scanner,&m));
    assert(m.patch_count==4);filed_rx_manifest_free(&m);
    loads[0].p_flags=PF_R;memcpy(raw+sizeof(eh),loads,sizeof(loads));
    assert(!filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m));
    assert(m.patch_count==4);filed_rx_manifest_free(&m);
    loads[0].p_flags=PF_R|PF_X;memcpy(raw,saved,size);

    /* Unwind/rodata can share the RX segment; use flags, never .text names. */
    eh.e_shoff=7000;eh.e_shentsize=sizeof(Elf64_Shdr);eh.e_shnum=3;
    Elf64_Shdr sections[3]={
        {0},
        {.sh_type=SHT_PROGBITS,.sh_flags=SHF_ALLOC|SHF_EXECINSTR,
            .sh_addr=256,.sh_offset=256,.sh_size=512},
        {.sh_type=SHT_PROGBITS,.sh_flags=SHF_ALLOC,
            .sh_addr=800,.sh_offset=800,.sh_size=128}};
    memcpy(raw,&eh,sizeof(eh));memcpy(raw+eh.e_shoff,sections,sizeof(sections));
    raw[820]=0x0f;raw[821]=0x05;
    assert(!filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m));
    memcpy(actual,raw,size);
    assert(!filed_rx_manifest_apply(&m,0,actual,size,0,raw,size));
    assert(actual[512]==0xff && actual[513]==0xd0);
    assert(!memcmp(actual+768,raw+768,size-768));
    filed_rx_manifest_free(&m);
    raw[256]=0x06; /* Invalid legacy PUSH ES in long mode, within code. */
    assert(filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m)==-EIO);
    assert(!m.patches && !m.page_hashes);raw[256]=0x90;
    /* Assembly can put non-instruction bytes between functions in .text.
     * Preserve that data and a SYSCALL-shaped immediate while patching an
     * independently decodable real syscall beyond the data. */
    uint8_t mixed[size];memcpy(mixed,raw,size);
    mixed[264]=0x06; /* Inline data, not a long-mode instruction. */
    mixed[512]=mixed[513]=0x90;
    const uint8_t immediate[]={0x48,0xb8,0x0f,0x05,0,0,0,0,0,0};
    memcpy(mixed+590,immediate,sizeof(immediate));
    mixed[700]=0x0f;mixed[701]=0x05;
    assert(!filed_rx_manifest_build(mixed,size,0,uuid,identity,scanner,&m));
    memcpy(actual,mixed,size);
    assert(!filed_rx_manifest_apply(&m,0,actual,size,0,mixed,size));
    for (size_t i=0;i<size;++i)
        assert(actual[i]==(i==700 ? 0xff : i==701 ? 0xd0 : mixed[i]));
    mixed[264]^=1;
    assert(filed_rx_manifest_apply(&m,0,actual,4096,0,mixed,size)==-EAGAIN);
    mixed[264]^=1;filed_rx_manifest_free(&m);
    /* Losing the linear stream cannot turn an ambiguous candidate into a
     * success. No partially validated manifest may escape on this failure. */
    memset(mixed+444,0,256);
    assert(filed_rx_manifest_build(mixed,size,0,uuid,identity,scanner,&m)==-EIO);
    assert(!m.patches && !m.page_hashes);
    eh.e_shoff=UINT64_MAX;memcpy(raw,&eh,sizeof(eh));
    assert(filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m)==-ENOEXEC);
    eh.e_shoff=7000;memcpy(raw,&eh,sizeof(eh));
    sections[1].sh_addr=4096;memcpy(raw+eh.e_shoff,sections,sizeof(sections));
    assert(filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m)==-ENOEXEC);
    memcpy(raw,saved,size);

    /* Malformed complete ELF images must not get an exemption. */
    eh.e_phoff=UINT64_MAX;memcpy(raw,&eh,sizeof(eh));
    assert(filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m)==-ENOEXEC);
    memcpy(raw,saved,size);
    loads[1].p_filesz=UINT64_MAX;memcpy(raw+sizeof(eh),loads,sizeof(loads));
    assert(filed_rx_manifest_build(raw,size,0,uuid,identity,scanner,&m)==-ENOEXEC);
    puts("RX ELF snapshot: executable patch, data/guards preserved, range fallback, invalid ELF/ambiguous code rejected: PASS");
}

int main(void)
{
    enum { length=8192 };
    uint8_t raw[length],expected[length],actual[length];
    memset(raw,0x90,sizeof(raw));
    raw[100]=0x0f;raw[101]=0x05;
    raw[4095]=0x0f;raw[4096]=0x05;
    raw[7000]=0x0f;raw[7001]=0x05;
    memcpy(expected,raw,length);
    lpr_patch_scan_result_t scan;
    assert(!lpr_patch_scan_syscalls(expected,length,0,&scan));
    assert(scan.patched_sites>=3);
    uint8_t uuid[16]={1},identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE]={2},
        scanner[32]={3};
    filed_rx_manifest_t manifest={0},decoded={0};
    assert(!filed_rx_manifest_build(raw,length,4096,uuid,identity,scanner,
        &manifest));
    assert(manifest.patch_count>=6 && manifest.page_count==2);
    uint8_t *encoded=NULL;size_t encoded_length=0;
    assert(!filed_rx_manifest_encode(&manifest,&encoded,&encoded_length));
    assert(!filed_rx_manifest_decode(encoded,encoded_length,
        manifest.key,scanner,&decoded));
    memcpy(actual,raw,length);
    assert(!filed_rx_manifest_apply(&decoded,0,actual,4096,0,raw,length));
    assert(!filed_rx_manifest_apply(&decoded,4096,actual+4096,4096,
        0,raw,length));
    assert(!memcmp(actual,expected,length));
    raw[4090]^=1;
    assert(filed_rx_manifest_apply(&decoded,4096,actual+4096,4096,
        0,raw,length)==-EAGAIN);
    encoded[encoded_length-1]^=1;
    filed_rx_manifest_t rejected={0};
    assert(filed_rx_manifest_decode(encoded,encoded_length,
        manifest.key,scanner,&rejected)==-EINVAL);
    free(encoded);
    filed_rx_manifest_free(&decoded);
    filed_rx_manifest_free(&manifest);
    puts("RX manifest: page guards, boundary patch, source equivalence, digest rejection: PASS");
    elf_snapshot_test();
}
