/* SPDX-License-Identifier: MIT */
#include "../src/exec/linux_lpr/map.c"
#include <assert.h>
#include <elf.h>
#include <patch_scan.h>

struct test_object { uint8_t *bytes; size_t size; unsigned refs; };
struct test_fd { struct test_object *object; uint64_t rights; };
static struct test_object objects[32];
static struct test_fd descriptors[256];
static unsigned next_fd=80, next_object, mapped_entries;
static int duplicate_failure, mapping_failure;

int pacha_vmo_create(uint64_t size,uint64_t rights,uint32_t flags)
{
    assert(size && !(size&4095) && !flags && next_object<32 && next_fd<256);
    struct test_object *object=&objects[next_object++];
    *object=(struct test_object){.bytes=malloc((size_t)size),.size=(size_t)size,.refs=1};
    assert(object->bytes);
    int fd=(int)next_fd++;
    descriptors[fd]=(struct test_fd){object,rights};
    return fd;
}
void *pacha_mmap(int fd,uint64_t size,uint64_t prot,uint64_t flags,uint64_t offset)
{
    assert(fd>=80 && (unsigned)fd<next_fd);
    struct test_fd *entry=&descriptors[fd];
    assert(entry->object && flags==PACHA_MMAP_SHARED && !offset);
    assert(size==entry->object->size && prot==(PACHA_PROT_READ|PACHA_PROT_WRITE));
    assert((entry->rights&(PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE))==
        (PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE));
    return entry->object->bytes;
}
int pacha_munmap(void *bytes,uint64_t size)
{
    assert(bytes && size);return 0;
}
long pacha_fd_fcntl(int fd,uint64_t command,uint64_t minimum,uint64_t rights)
{
    assert(command==PACHA_FD_FCNTL_DUP && minimum==16);
    assert(fd>=80 && (unsigned)fd<next_fd && descriptors[fd].object);
    assert(descriptors[fd].rights&PACHA_FD_RIGHT_DUP);
    assert(!(rights&~descriptors[fd].rights));
    if(duplicate_failure) return duplicate_failure;
    assert(next_fd<256);
    int copy=(int)next_fd++;
    descriptors[copy]=(struct test_fd){descriptors[fd].object,rights};
    ++descriptors[copy].object->refs;
    return copy;
}
int pacha_fd_close(int fd)
{
    assert(fd>=80 && (unsigned)fd<next_fd && descriptors[fd].object);
    assert(descriptors[fd].rights&PACHA_FD_RIGHT_CLOSE);
    struct test_object *object=descriptors[fd].object;
    descriptors[fd]=(struct test_fd){0};
    assert(object->refs);
    if(!--object->refs) { free(object->bytes);object->bytes=NULL; }
    return 0;
}
static uint64_t required_rights(uint64_t prot)
{
    uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE;
    if(prot&PACHA_PROT_READ) rights|=PACHA_FD_RIGHT_MAP_READ;
    if(prot&PACHA_PROT_WRITE) rights|=PACHA_FD_RIGHT_MAP_WRITE;
    if(prot&PACHA_PROT_EXEC) rights|=PACHA_FD_RIGHT_MAP_EXEC;
    return rights;
}
int pacha_process_map_batch(int process,const struct pacha_process_map_batch_entry *entries,
    uint64_t count)
{
    assert(process==60 && count);
    if(mapping_failure) return mapping_failure;
    uint64_t previous=0;
    for(uint64_t i=0;i<count;++i) {
        const struct pacha_process_map_batch_entry *entry=&entries[i];
        assert(entry->vmo_fd<next_fd && entry->flags==PACHA_PROCESS_MAP_PRIVATE);
        const struct test_fd *fd=&descriptors[entry->vmo_fd];
        assert(fd->object && entry->vmo_offset<=fd->object->size &&
            entry->size<=fd->object->size-entry->vmo_offset);
        uint64_t mapping_rights=required_rights(entry->prot)&
            (PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE|PACHA_FD_RIGHT_MAP_EXEC);
        if(mapping_rights&~fd->rights) return -1;
        assert(fd->rights==required_rights(entry->prot));
        assert(!(entry->target_va&4095) && !(entry->size&4095) &&
            !(entry->vmo_offset&4095) && entry->target_va>=previous);
        previous=entry->target_va+entry->size;
        ++mapped_entries;
    }
    return 0;
}
/* These tests do not patch Linux text; runtime relocation remains real. */
int lpr_patch_scan_syscalls(unsigned char *bytes,uint64_t size,
    uint64_t start,lpr_patch_scan_result_t *out)
{
    (void)bytes;(void)size;(void)start;(void)out;assert(0);return -1;
}
static void assert_drained(void)
{
    for(unsigned i=0;i<next_object;++i) assert(!objects[i].refs && !objects[i].bytes);
    for(unsigned i=80;i<next_fd;++i) assert(!descriptors[i].object);
}
static lpr_exec_image_t load_image(const char *path)
{
    FILE *file=fopen(path,"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);
    assert(!fseek(file,0,SEEK_SET));
    unsigned char *bytes=malloc((size_t)size);assert(bytes);
    assert(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
    return (lpr_exec_image_t){bytes,(uint64_t)size};
}
static lpr_exec_image_t synthetic_image(void)
{
    unsigned char *bytes=calloc(1,0x5000);assert(bytes);
    Elf64_Ehdr *header=(Elf64_Ehdr *)bytes;
    memcpy(header->e_ident,ELFMAG,SELFMAG);
    header->e_ident[EI_CLASS]=ELFCLASS64;
    header->e_ident[EI_DATA]=ELFDATA2LSB;
    header->e_ident[EI_VERSION]=EV_CURRENT;
    header->e_type=ET_DYN;header->e_machine=EM_X86_64;header->e_version=EV_CURRENT;
    header->e_phoff=sizeof(*header);header->e_phentsize=sizeof(Elf64_Phdr);
    header->e_phnum=3;
    Elf64_Phdr *headers=(Elf64_Phdr *)(bytes+header->e_phoff);
    headers[0]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R,
        .p_filesz=0x3000,.p_memsz=0x3000,.p_align=4096};
    headers[1]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R|PF_X,
        .p_offset=0x3000,.p_vaddr=0x4000,.p_filesz=0x1000,.p_memsz=0x1000,.p_align=4096};
    headers[2]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R|PF_W,
        .p_offset=0x4000,.p_vaddr=0x6000,.p_filesz=0x1000,.p_memsz=0x2000,.p_align=4096};
    return (lpr_exec_image_t){bytes,0x5000};
}
int main(int argc,char **argv)
{
    assert(argc==1 || argc==2);
    lpr_exec_image_t image=argc==2 ? load_image(argv[1]) : synthetic_image();
    lpr_exec_pending_map_batch_t batch;
    lpr_exec_loaded_t loaded;
    lpr_exec_pending_map_batch_init(&batch);
    assert(!prepare_image(&image,LPR_IMAGE_RUNTIME_BASE_VA,0,1,&batch,&loaded));
    assert(batch.count>=3 && loaded.base==LPR_IMAGE_RUNTIME_BASE_VA);
    assert(batch.entries[0].target_va==LPR_IMAGE_RUNTIME_BASE_VA);
    assert(batch.entries[0].prot==PACHA_PROT_READ);
    unsigned executable=0,writable=0;
    for(uint64_t i=0;i<batch.count;++i) {
        executable+=!!(batch.entries[i].prot&PACHA_PROT_EXEC);
        writable+=!!(batch.entries[i].prot&PACHA_PROT_WRITE);
    }
    assert(executable && writable);
    assert(!lpr_exec_pending_map_batch_commit(60,&batch) && mapped_entries>=3);
    assert_drained();
    /* Failed commits must close every explicit, attenuated queued reference. */
    assert(!prepare_image(&image,LPR_IMAGE_RUNTIME_BASE_VA,0,1,&batch,&loaded));
    mapping_failure=-1;
    assert(lpr_exec_pending_map_batch_commit(60,&batch)==-1);
    mapping_failure=0;assert_drained();
    /* A raw positive native status is not a usable fd or successful exec. */
    duplicate_failure=1;
    assert(prepare_image(&image,LPR_IMAGE_RUNTIME_BASE_VA,0,1,&batch,&loaded)==-1);
    lpr_exec_pending_map_batch_discard(&batch);duplicate_failure=0;assert_drained();
    /* PROT_NONE references need cleanup rights but no extra mapping authority. */
    int vmo=-1;unsigned char *mapped=NULL;
    assert(!create_segment_vmo(4096,&vmo,&mapped));
    int no_mapping=(int)pacha_fd_fcntl(vmo,PACHA_FD_FCNTL_DUP,16,PACHA_FD_RIGHT_CLOSE);
    const struct pacha_process_map_batch_entry rejected={.vmo_fd=(uint64_t)no_mapping,
        .target_va=0x40000000,.size=4096,.prot=PACHA_PROT_READ,
        .flags=PACHA_PROCESS_MAP_PRIVATE};
    assert(pacha_process_map_batch(60,&rejected,1)==-1);
    assert(!pacha_fd_close(no_mapping));
    assert(!append_map(&batch,vmo,0x40000000,4096,0,0));
    assert(descriptors[batch.entries[0].vmo_fd].rights==required_rights(0));
    assert(!pacha_munmap(mapped,4096) && !pacha_fd_close(vmo));
    assert(!lpr_exec_pending_map_batch_commit(60,&batch));assert_drained();
    free(image.bytes);
    puts("filed actual LPR ELF private map rights / relocation / reference cleanup: PASS");
}
