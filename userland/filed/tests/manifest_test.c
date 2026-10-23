/* SPDX-License-Identifier: MIT */
#include "filed/manifest.h"
#include <patch_scan.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
}
