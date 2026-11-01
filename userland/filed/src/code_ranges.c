/* SPDX-License-Identifier: MIT */
#include "filed/code_ranges.h"
#include <elf.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void filed_rx_code_ranges_free(filed_rx_code_ranges_t *code)
{
    if (!code) return;
    free(code->starts);free(code->ends);
    memset(code,0,sizeof(*code));
}

/* .eh_frame parsing (LSB x86-64, DWARF CFI with the GNU 'z' augmentation).
 * Any unsupported or inconsistent record discards the whole table: missing
 * FDE knowledge only keeps the strict scanner, so it is never unsafe. */
enum { eh_max_bytes=64u<<20, eh_max_phnum=4096, eh_max_cies=4096 };
enum {
    pe_absptr=0x00,pe_uleb128=0x01,pe_udata2=0x02,pe_udata4=0x03,
    pe_udata8=0x04,pe_sleb128=0x09,pe_sdata2=0x0a,pe_sdata4=0x0b,
    pe_sdata8=0x0c,pe_pcrel=0x10,pe_datarel=0x30,pe_omit=0xff
};
struct eh_bytes { const uint8_t *data; size_t size; uint64_t vaddr; };

static int eh_uleb(const struct eh_bytes *b,size_t *at,uint64_t *out)
{
    uint64_t value=0;
    for (unsigned shift=0;*at<b->size && shift<64;shift+=7) {
        const uint8_t byte=b->data[(*at)++];
        value|=(uint64_t)(byte&0x7f)<<shift;
        if (!(byte&0x80)) { *out=value;return 0; }
    }
    return -ENOEXEC;
}

static int eh_sleb(const struct eh_bytes *b,size_t *at,int64_t *out)
{
    uint64_t value=0;unsigned shift=0;uint8_t byte=0;
    do {
        if (*at>=b->size || shift>=64) return -ENOEXEC;
        byte=b->data[(*at)++];
        value|=(uint64_t)(byte&0x7f)<<shift;
        shift+=7;
    } while (byte&0x80);
    if (shift<64 && (byte&0x40)) value|=~UINT64_C(0)<<shift;
    *out=(int64_t)value;
    return 0;
}

static int eh_fixed(const struct eh_bytes *b,size_t *at,unsigned bytes,uint64_t *out)
{
    if (*at>b->size || bytes>b->size-*at) return -ENOEXEC;
    uint64_t value=0;
    for (unsigned i=0;i<bytes;++i) value|=(uint64_t)b->data[*at+i]<<(8*i);
    *at+=bytes;*out=value;
    return 0;
}

/* Decode one DW_EH_PE value. apply=0 decodes the format only (pc_range). */
static int eh_pointer(const struct eh_bytes *b,size_t *at,uint8_t encoding,
    int apply,uint64_t datarel,uint64_t *out)
{
    if (encoding==pe_omit || (encoding&0x80)) return -ENOEXEC;
    const uint64_t field=b->vaddr+*at;
    uint64_t value=0;int64_t signed_value=0;int status;
    switch (encoding&0x0f) {
    case pe_absptr: case pe_udata8: case pe_sdata8:
        status=eh_fixed(b,at,8,&value);break;
    case pe_udata4: status=eh_fixed(b,at,4,&value);break;
    case pe_sdata4:
        status=eh_fixed(b,at,4,&value);
        value=(uint64_t)(int64_t)(int32_t)(uint32_t)value;break;
    case pe_udata2: status=eh_fixed(b,at,2,&value);break;
    case pe_sdata2:
        status=eh_fixed(b,at,2,&value);
        value=(uint64_t)(int64_t)(int16_t)(uint16_t)value;break;
    case pe_uleb128: status=eh_uleb(b,at,&value);break;
    case pe_sleb128:
        status=eh_sleb(b,at,&signed_value);value=(uint64_t)signed_value;break;
    default: return -ENOEXEC;
    }
    if (status) return status;
    if (apply) switch (encoding&0x70) {
    case 0: break;
    case pe_pcrel: value+=field;break;
    case pe_datarel: value+=datarel;break;
    default: return -ENOEXEC;
    } else if (encoding&0x70) return -ENOEXEC;
    *out=value;
    return 0;
}

/* Return the FDE pointer encoding of the CIE at cie. */
static int eh_cie_encoding(const struct eh_bytes *b,size_t cie,size_t end,
    unsigned id_bytes,uint8_t *encoding)
{
    struct eh_bytes view={b->data,end,b->vaddr};
    size_t at=cie+id_bytes;
    if (at>=end) return -ENOEXEC;
    const uint8_t version=b->data[at++];
    if (version!=1 && version!=3 && version!=4) return -ENOEXEC;
    const char *augmentation=(const char *)b->data+at;
    const uint8_t *nul=memchr(b->data+at,0,end-at);
    if (!nul) return -ENOEXEC;
    const size_t aug_length=(size_t)(nul-(b->data+at));
    at+=aug_length+1;
    if (version==4) {
        if (end-at<2 || b->data[at]!=8 || b->data[at+1]!=0) return -ENOEXEC;
        at+=2;
    }
    uint64_t unsigned_value;int64_t signed_value;
    if (eh_uleb(&view,&at,&unsigned_value) || eh_sleb(&view,&at,&signed_value))
        return -ENOEXEC;
    if (version==1) { if (at>=end) return -ENOEXEC; ++at; }
    else if (eh_uleb(&view,&at,&unsigned_value)) return -ENOEXEC;
    *encoding=pe_absptr;
    if (!aug_length) return 0;
    if (augmentation[0]!='z') return -ENOEXEC;
    uint64_t data_length;
    if (eh_uleb(&view,&at,&data_length) || data_length>end-at) return -ENOEXEC;
    const size_t data_end=at+(size_t)data_length;
    view.size=data_end;
    for (size_t i=1;i<aug_length;++i) {
        uint64_t ignored;
        switch (augmentation[i]) {
        case 'R':
            if (at>=data_end) return -ENOEXEC;
            *encoding=b->data[at++];
            break;
        case 'L':
            if (at>=data_end) return -ENOEXEC;
            ++at;break;
        case 'P': {
            if (at>=data_end) return -ENOEXEC;
            const uint8_t personality=b->data[at++];
            if (eh_pointer(&view,&at,personality&0x7f,1,0,&ignored))
                return -ENOEXEC;
            break;
        }
        case 'S': case 'B': case 'G': break;
        default: return -ENOEXEC;
        }
    }
    return 0;
}

struct code_list { uint64_t *starts,*ends; size_t count,capacity; };

static int code_list_add(struct code_list *list,uint64_t start,uint64_t end)
{
    if (list->count==list->capacity) {
        size_t capacity=list->capacity ? list->capacity*2 : 1024;
        if (capacity>SIZE_MAX/sizeof(uint64_t)) return -ENOMEM;
        uint64_t *starts=realloc(list->starts,capacity*sizeof(uint64_t));
        if (!starts) return -ENOMEM;
        list->starts=starts;
        uint64_t *ends=realloc(list->ends,capacity*sizeof(uint64_t));
        if (!ends) return -ENOMEM;
        list->ends=ends;list->capacity=capacity;
    }
    list->starts[list->count]=start;list->ends[list->count]=end;
    ++list->count;
    return 0;
}

/* Clip a vaddr FDE range to each executable PT_LOAD and record file offsets. */
static int code_list_add_vaddr(struct code_list *list,const Elf64_Phdr *ph,
    unsigned phnum,uint64_t begin,uint64_t length)
{
    if (!length || begin>UINT64_MAX-length) return length ? -ENOEXEC : 0;
    const uint64_t end=begin+length;
    for (unsigned i=0;i<phnum;++i) {
        if (ph[i].p_type!=PT_LOAD || !(ph[i].p_flags&PF_X) || !ph[i].p_filesz)
            continue;
        const uint64_t lo=ph[i].p_vaddr,hi=ph[i].p_vaddr+ph[i].p_filesz;
        const uint64_t from=begin>lo ? begin : lo,until=end<hi ? end : hi;
        if (from>=until) continue;
        int status=code_list_add(list,ph[i].p_offset+(from-lo),
            ph[i].p_offset+(until-lo));
        if (status) return status;
    }
    return 0;
}

static int code_range_compare(const void *left,const void *right)
{
    const uint64_t *a=left,*b=right;
    return a[0]<b[0] ? -1 : a[0]>b[0];
}

static int read_vaddr(filed_rx_read_fn read,void *context,uint64_t file_size,
    const Elf64_Phdr *ph,unsigned phnum,uint64_t vaddr,uint64_t wanted,
    struct eh_bytes *out)
{
    for (unsigned i=0;i<phnum;++i) {
        if (ph[i].p_type!=PT_LOAD || vaddr<ph[i].p_vaddr ||
            vaddr-ph[i].p_vaddr>=ph[i].p_filesz) continue;
        uint64_t available=ph[i].p_filesz-(vaddr-ph[i].p_vaddr);
        const uint64_t offset=ph[i].p_offset+(vaddr-ph[i].p_vaddr);
        if (wanted && wanted<available) available=wanted;
        if (available>eh_max_bytes) available=eh_max_bytes;
        if (offset>file_size || available>file_size-offset) return -ENOEXEC;
        uint8_t *data=malloc((size_t)available);
        if (!data) return -ENOMEM;
        int status=read(context,offset,data,(size_t)available);
        if (status) { free(data);return status; }
        *out=(struct eh_bytes){data,(size_t)available,vaddr};
        return 0;
    }
    return -ENOEXEC;
}

int filed_rx_code_ranges_load(filed_rx_read_fn read,void *context,
    uint64_t file_size,filed_rx_code_ranges_t *out)
{
    if (!read || !out) return -EINVAL;
    memset(out,0,sizeof(*out));
    Elf64_Ehdr eh;
    if (file_size<sizeof(eh)) return -ENOEXEC;
    int status=read(context,0,(uint8_t *)&eh,sizeof(eh));
    if (status) return status;
    if (memcmp(eh.e_ident,ELFMAG,SELFMAG) || eh.e_ident[EI_CLASS]!=ELFCLASS64 ||
        eh.e_ident[EI_DATA]!=ELFDATA2LSB || eh.e_machine!=EM_X86_64 ||
        (eh.e_type!=ET_DYN && eh.e_type!=ET_EXEC) ||
        eh.e_phentsize!=sizeof(Elf64_Phdr) || !eh.e_phnum ||
        eh.e_phnum>eh_max_phnum || eh.e_phoff>file_size ||
        (uint64_t)eh.e_phnum*sizeof(Elf64_Phdr)>file_size-eh.e_phoff)
        return -ENOEXEC;
    Elf64_Phdr *ph=calloc(eh.e_phnum,sizeof(*ph));
    if (!ph) return -ENOMEM;
    status=read(context,eh.e_phoff,(uint8_t *)ph,eh.e_phnum*sizeof(*ph));
    const Elf64_Phdr *hdr_ph=NULL;
    for (unsigned i=0;!status && i<eh.e_phnum;++i) {
        if (ph[i].p_type==PT_LOAD && (ph[i].p_filesz>ph[i].p_memsz ||
            ph[i].p_offset>file_size || ph[i].p_filesz>file_size-ph[i].p_offset ||
            ph[i].p_vaddr>UINT64_MAX-ph[i].p_memsz)) status=-ENOEXEC;
        if (ph[i].p_type==PT_GNU_EH_FRAME) status=hdr_ph ? -ENOEXEC :
            (hdr_ph=&ph[i],0);
    }
    if (!status && !hdr_ph) status=-ENOENT;
    struct eh_bytes hdr={0},frame={0};
    if (!status) status=read_vaddr(read,context,file_size,ph,eh.e_phnum,
        hdr_ph->p_vaddr,hdr_ph->p_filesz,&hdr);
    uint64_t frame_vaddr=0;
    if (!status) {
        size_t at=4;
        if (hdr.size<4 || hdr.data[0]!=1 ||
            eh_pointer(&hdr,&at,hdr.data[1],1,hdr.vaddr,&frame_vaddr))
            status=-ENOEXEC;
    }
    if (!status) status=read_vaddr(read,context,file_size,ph,eh.e_phnum,
        frame_vaddr,0,&frame);
    struct code_list list={0};
    size_t cie_offsets[64];uint8_t cie_encodings[64];size_t cie_cached=0;
    unsigned cies=0;
    for (size_t at=0;!status && frame.size-at>=4;) {
        uint64_t length;size_t cursor=at;
        status=eh_fixed(&frame,&cursor,4,&length);
        if (status || !length) break;
        unsigned id_bytes=4;
        if (length==0xffffffffu) {
            status=eh_fixed(&frame,&cursor,8,&length);
            id_bytes=8;
            if (status) break;
        }
        const size_t body=cursor;
        if (length<id_bytes || length>frame.size-body) { status=-ENOEXEC;break; }
        const size_t end=body+(size_t)length;
        uint64_t id;
        status=eh_fixed(&frame,&cursor,id_bytes,&id);
        if (status) break;
        if (!id) {
            if (++cies>eh_max_cies) status=-ENOEXEC;
            at=end;continue;
        }
        /* The CIE pointer is relative to the FDE's own id field. */
        if (id>body) { status=-ENOEXEC;break; }
        const size_t cie=body-(size_t)id;
        uint8_t encoding=pe_absptr;size_t c=0;
        while (c<cie_cached && cie_offsets[c]!=cie) ++c;
        if (c<cie_cached) encoding=cie_encodings[c];
        else {
            size_t cie_cursor=cie;uint64_t cie_length,cie_id;
            unsigned cie_id_bytes=4;
            status=eh_fixed(&frame,&cie_cursor,4,&cie_length);
            if (!status && cie_length==0xffffffffu) {
                status=eh_fixed(&frame,&cie_cursor,8,&cie_length);
                cie_id_bytes=8;
            }
            if (status || cie_length<cie_id_bytes ||
                cie_length>frame.size-cie_cursor) { status=-ENOEXEC;break; }
            const size_t cie_body=cie_cursor;
            status=eh_fixed(&frame,&cie_cursor,cie_id_bytes,&cie_id);
            if (!status && cie_id) status=-ENOEXEC;
            if (!status) status=eh_cie_encoding(&frame,cie_body,
                cie_body+(size_t)cie_length,cie_id_bytes,&encoding);
            if (status) break;
            if (cie_cached<sizeof(cie_offsets)/sizeof(cie_offsets[0])) {
                cie_offsets[cie_cached]=cie;cie_encodings[cie_cached++]=encoding;
            }
        }
        struct eh_bytes record={frame.data,end,frame.vaddr};
        uint64_t begin,range;
        status=eh_pointer(&record,&cursor,encoding,1,0,&begin);
        if (!status) status=eh_pointer(&record,&cursor,encoding&0x0f,0,0,&range);
        if (!status) status=code_list_add_vaddr(&list,ph,eh.e_phnum,begin,range);
        at=end;
    }
    free((void *)hdr.data);free((void *)frame.data);free(ph);
    if (!status && !list.count) status=-ENOENT;
    if (status) { free(list.starts);free(list.ends);return status; }
    uint64_t (*pairs)[2]=malloc(list.count*sizeof(*pairs));
    if (!pairs) { free(list.starts);free(list.ends);return -ENOMEM; }
    for (size_t i=0;i<list.count;++i) {
        pairs[i][0]=list.starts[i];pairs[i][1]=list.ends[i];
    }
    qsort(pairs,list.count,sizeof(*pairs),code_range_compare);
    size_t merged=0;
    for (size_t i=0;i<list.count;++i) {
        if (merged && pairs[i][0]<=list.ends[merged-1]) {
            if (pairs[i][1]>list.ends[merged-1]) list.ends[merged-1]=pairs[i][1];
            continue;
        }
        list.starts[merged]=pairs[i][0];list.ends[merged]=pairs[i][1];
        ++merged;
    }
    free(pairs);
    *out=(filed_rx_code_ranges_t){list.starts,list.ends,merged};
    return 0;
}

int filed_rx_code_ranges_outside(const filed_rx_code_ranges_t *code,uint64_t file_offset)
{
    if (!code || !code->count || file_offset>UINT64_MAX-2) return 0;
    const uint64_t last=file_offset+2;
    size_t lo=0,hi=code->count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if (code->ends[mid]<=file_offset) lo=mid+1;
        else hi=mid;
    }
    return lo==code->count || code->starts[lo]>=last;
}
