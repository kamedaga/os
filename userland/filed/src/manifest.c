/* SPDX-License-Identifier: MIT */
#include "filed/manifest.h"
#include <kobox2/sha256.h>
#include <patch_scan.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t manifest_magic[16]={
    'F','I','L','E','D','-','Z','P','O','L','I','N','E',0,0,0};
enum {
    h_magic=0,h_total=16,h_file_offset=24,h_length=32,h_pages=40,
    h_patches=48,h_uuid=56,h_scanner=72,h_identity=104,
    h_key=h_identity+KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE,
    h_digest=h_key+32,h_size=h_digest+32
};
static void put64(uint8_t *p,uint64_t v)
{ for (unsigned i=0;i<8;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t get64(const uint8_t *p)
{ uint64_t v=0;for (unsigned i=0;i<8;++i) v|=(uint64_t)p[i]<<(8*i);return v; }

void filed_rx_manifest_free(filed_rx_manifest_t *m)
{
    if (!m) return;
    free(m->page_hashes);free(m->patches);
    memset(m,0,sizeof(*m));
}

void filed_rx_manifest_key(const uint8_t root_uuid[16],
    const uint8_t identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE],
    uint64_t file_offset,uint64_t length,const uint8_t scanner_hash[32],
    uint8_t key[32])
{
    uint8_t range[16];
    put64(range,file_offset);put64(range+8,length);
    kb2_sha256_context_t ctx;
    kb2_sha256_initialize(&ctx);
    kb2_sha256_update(&ctx,root_uuid,16);
    kb2_sha256_update(&ctx,identity,KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE);
    kb2_sha256_update(&ctx,range,sizeof(range));
    kb2_sha256_update(&ctx,scanner_hash,32);
    kb2_sha256_finish(&ctx,key);
}

static void page_digest(const uint8_t *raw,size_t length,uint64_t page,
    uint8_t out[FILED_RX_HASH_BYTES])
{
    uint64_t start=page*4096;
    uint64_t before=start<FILED_RX_GUARD_BYTES ? 0 :
        start-FILED_RX_GUARD_BYTES;
    uint64_t end=start+4096+FILED_RX_GUARD_BYTES;
    if (end>length) end=length;
    uint8_t hash[32];
    kb2_sha256(raw+before,(size_t)(end-before),hash);
    memcpy(out,hash,FILED_RX_HASH_BYTES);
}

int filed_rx_manifest_build(const uint8_t *raw,size_t length,
    uint64_t file_offset,const uint8_t root_uuid[16],
    const uint8_t identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE],
    const uint8_t scanner_hash[32],filed_rx_manifest_t *out)
{
    if (!raw || !length || !root_uuid || !identity || !scanner_hash || !out ||
        length>SIZE_MAX-4095) return -EINVAL;
    filed_rx_manifest_t m={0};
    m.file_offset=file_offset;m.length=length;
    m.page_count=(length+4095)/4096;
    memcpy(m.root_uuid,root_uuid,16);
    memcpy(m.identity,identity,KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE);
    memcpy(m.scanner_hash,scanner_hash,32);
    filed_rx_manifest_key(root_uuid,identity,file_offset,length,scanner_hash,m.key);
    if (m.page_count>SIZE_MAX/FILED_RX_HASH_BYTES) return -EOVERFLOW;
    m.page_hashes=malloc((size_t)m.page_count*FILED_RX_HASH_BYTES);
    uint8_t *patched=malloc(length);
    if (!m.page_hashes || !patched) {
        free(patched);filed_rx_manifest_free(&m);return -ENOMEM;
    }
    for (uint64_t i=0;i<m.page_count;++i)
        page_digest(raw,length,i,m.page_hashes+i*FILED_RX_HASH_BYTES);
    memcpy(patched,raw,length);
    lpr_patch_scan_result_t scan;
    if (lpr_patch_scan_syscalls(patched,length,0,&scan)) {
        free(patched);filed_rx_manifest_free(&m);return -EIO;
    }
    for (size_t i=0;i<length;++i) m.patch_count+=patched[i]!=raw[i];
    if (m.patch_count>SIZE_MAX/sizeof(*m.patches)) {
        free(patched);filed_rx_manifest_free(&m);return -EOVERFLOW;
    }
    m.patches=m.patch_count ? malloc((size_t)m.patch_count*sizeof(*m.patches)) : NULL;
    if (m.patch_count && !m.patches) {
        free(patched);filed_rx_manifest_free(&m);return -ENOMEM;
    }
    size_t at=0;
    for (size_t i=0;i<length;++i)
        if (patched[i]!=raw[i]) m.patches[at++]=(filed_rx_patch_t){i,patched[i]};
    free(patched);
    *out=m;
    return 0;
}

int filed_rx_manifest_apply(const filed_rx_manifest_t *m,
    uint64_t target_offset,uint8_t *page_bytes,size_t page_length,
    uint64_t guarded_offset,const uint8_t *guarded,size_t guarded_length)
{
    if (!m || !page_bytes || !guarded || !page_length ||
        ((target_offset|page_length)&4095) ||
        m->length>UINT64_MAX-4095 ||
        target_offset>m->length || page_length>m->length+4095-target_offset)
        return -EINVAL;
    const uint64_t end=target_offset+page_length;
    for (uint64_t page=target_offset/4096;page<end/4096;++page) {
        if (page>=m->page_count) return -EINVAL;
        const uint64_t start=page*4096;
        const uint64_t first=start<FILED_RX_GUARD_BYTES ? 0 :
            start-FILED_RX_GUARD_BYTES;
        uint64_t last=start+4096+FILED_RX_GUARD_BYTES;
        if (last>m->length) last=m->length;
        if (first<guarded_offset || last-guarded_offset>guarded_length)
            return -EINVAL;
        uint8_t hash[32];
        kb2_sha256(guarded+(first-guarded_offset),(size_t)(last-first),hash);
        if (memcmp(hash,m->page_hashes+page*FILED_RX_HASH_BYTES,
                FILED_RX_HASH_BYTES)) return -EAGAIN;
    }
    size_t lo=0,hi=(size_t)m->patch_count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if (m->patches[mid].offset<target_offset) lo=mid+1;
        else hi=mid;
    }
    for (size_t i=lo;i<m->patch_count && m->patches[i].offset<end;++i)
        page_bytes[m->patches[i].offset-target_offset]=m->patches[i].value;
    return 0;
}

int filed_rx_manifest_encode(const filed_rx_manifest_t *m,uint8_t **bytes,size_t *length)
{
    if (!m || !bytes || !length || m->page_count>SIZE_MAX/FILED_RX_HASH_BYTES ||
        m->patch_count>(SIZE_MAX-h_size-m->page_count*FILED_RX_HASH_BYTES)/16)
        return -EINVAL;
    size_t hash_bytes=(size_t)m->page_count*FILED_RX_HASH_BYTES;
    size_t total=h_size+hash_bytes+(size_t)m->patch_count*16;
    uint8_t *data=calloc(1,total);
    if (!data) return -ENOMEM;
    memcpy(data+h_magic,manifest_magic,16);
    put64(data+h_total,total);put64(data+h_file_offset,m->file_offset);
    put64(data+h_length,m->length);put64(data+h_pages,m->page_count);
    put64(data+h_patches,m->patch_count);
    memcpy(data+h_uuid,m->root_uuid,16);
    memcpy(data+h_scanner,m->scanner_hash,32);
    memcpy(data+h_identity,m->identity,KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE);
    memcpy(data+h_key,m->key,32);
    memcpy(data+h_size,m->page_hashes,hash_bytes);
    for (size_t i=0;i<m->patch_count;++i) {
        uint8_t *record=data+h_size+hash_bytes+i*16;
        put64(record,m->patches[i].offset);
        record[8]=m->patches[i].value;
    }
    kb2_sha256(data+h_size,total-h_size,data+h_digest);
    *bytes=data;*length=total;
    return 0;
}

int filed_rx_manifest_decode(const uint8_t *data,size_t length,
    const uint8_t expected_key[32],const uint8_t scanner_hash[32],
    filed_rx_manifest_t *out)
{
    if (!data || !expected_key || !scanner_hash || !out || length<h_size ||
        memcmp(data+h_magic,manifest_magic,16) ||
        get64(data+h_total)!=length ||
        memcmp(data+h_key,expected_key,32) ||
        memcmp(data+h_scanner,scanner_hash,32)) return -EINVAL;
    const uint64_t rx_length=get64(data+h_length);
    const uint64_t pages=get64(data+h_pages);
    const uint64_t patches=get64(data+h_patches);
    if (!rx_length || rx_length>UINT64_MAX-4095 ||
        pages!=(rx_length+4095)/4096 ||
        pages>(SIZE_MAX-h_size)/FILED_RX_HASH_BYTES ||
        patches>(SIZE_MAX-h_size-pages*FILED_RX_HASH_BYTES)/16 ||
        h_size+pages*FILED_RX_HASH_BYTES+patches*16!=length)
        return -EINVAL;
    uint8_t hash[32];
    kb2_sha256(data+h_size,length-h_size,hash);
    if (memcmp(hash,data+h_digest,32)) return -EINVAL;
    uint8_t key[32];
    filed_rx_manifest_key(data+h_uuid,data+h_identity,
        get64(data+h_file_offset),rx_length,scanner_hash,key);
    if (memcmp(key,expected_key,32)) return -EINVAL;
    filed_rx_manifest_t m={0};
    m.file_offset=get64(data+h_file_offset);m.length=rx_length;
    m.page_count=pages;m.patch_count=patches;
    memcpy(m.root_uuid,data+h_uuid,16);
    memcpy(m.scanner_hash,scanner_hash,32);
    memcpy(m.identity,data+h_identity,KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE);
    memcpy(m.key,key,32);
    m.page_hashes=malloc((size_t)pages*FILED_RX_HASH_BYTES);
    m.patches=patches ? malloc((size_t)patches*sizeof(*m.patches)) : NULL;
    if (!m.page_hashes || (patches && !m.patches)) {
        filed_rx_manifest_free(&m);return -ENOMEM;
    }
    memcpy(m.page_hashes,data+h_size,(size_t)pages*FILED_RX_HASH_BYTES);
    uint64_t previous=UINT64_MAX;
    for (uint64_t i=0;i<patches;++i) {
        const uint8_t *record=data+h_size+pages*FILED_RX_HASH_BYTES+i*16;
        const uint64_t offset=get64(record);
        if (offset>=rx_length || (i && offset<=previous)) {
            filed_rx_manifest_free(&m);return -EINVAL;
        }
        for (unsigned j=9;j<16;++j)
            if (record[j]) { filed_rx_manifest_free(&m);return -EINVAL; }
        m.patches[i]=(filed_rx_patch_t){offset,record[8]};
        previous=offset;
    }
    *out=m;
    return 0;
}
