/* SPDX-License-Identifier: MIT */
#include "filed/payload.h"
#include "../src/exec/linux_lpr/script.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr64(uint8_t *p,uint64_t value)
{
    for(unsigned i=0;i<8;++i) p[i]=(uint8_t)(value>>(8*i));
}
int main(void)
{
    size_t argc=257,envc=97,string_size=70001;
    char *strings=malloc(string_size);
    assert(strings);
    memset(strings,'x',string_size-1);strings[string_size-1]=0;
    filed_exec_string_ref_t *refs=calloc(argc+envc,sizeof(*refs));
    assert(refs);
    for(size_t i=0;i<argc+envc;++i) refs[i]=(filed_exec_string_ref_t){0,string_size};
    filed_exec_path_t request={.path="/tmp/script",.flags=FILED_EXEC_LINUX_LPR,
        .argc=argc,.envc=envc,.argv=refs,.envp=refs+argc,
        .strings=strings,.string_bytes=string_size};
    uint8_t *encoded=NULL;size_t size=0;
    assert(filed_exec_payload_encode(&request,&encoded,&size)==0);
    assert(size>16*1024*1024);
    filed_exec_path_t decoded;
    assert(filed_exec_payload_decode(encoded,size,request.path,19,request.flags,&decoded)==0);
    assert(decoded.argc==argc && decoded.envc==envc && decoded.dir_handle==19);
    assert(strlen(filed_exec_string(&decoded,decoded.argv[argc-1]))==string_size-1);
    filed_exec_payload_destroy(&decoded);
    uint8_t saved[16];
    memcpy(saved,encoded+KB2_FILESYSTEM_EXEC_PAYLOAD_ENVP_OFFSET_OFFSET,16);
    memcpy(encoded+KB2_FILESYSTEM_EXEC_PAYLOAD_ENVP_OFFSET_OFFSET,
        encoded+KB2_FILESYSTEM_EXEC_PAYLOAD_ARGV_OFFSET_OFFSET,8);
    assert(filed_exec_payload_decode(encoded,size,request.path,19,request.flags,&decoded)==-22);
    memcpy(encoded+KB2_FILESYSTEM_EXEC_PAYLOAD_ENVP_OFFSET_OFFSET,saved,16);
    assert(filed_exec_payload_decode(encoded,size,request.path,19,request.flags+1,&decoded)==-22);
    wr64(encoded+KB2_FILESYSTEM_EXEC_PAYLOAD_ARGC_OFFSET,UINT64_MAX);
    assert(filed_exec_payload_decode(encoded,size,request.path,19,request.flags,&decoded)==-22);
    free(encoded);
    assert(lpr_exec_rewrite_script_request(&request,"/bin/interpreter","--flag","/tmp/script")==0);
    assert(request.argc==argc+2 && request.envc==envc);
    assert(strcmp(filed_exec_string(&request,request.argv[0]),"/bin/interpreter")==0);
    assert(strcmp(filed_exec_string(&request,request.argv[1]),"--flag")==0);
    assert(strcmp(filed_exec_string(&request,request.argv[2]),"/tmp/script")==0);
    assert(strlen(filed_exec_string(&request,request.argv[request.argc-1]))==string_size-1);
    filed_exec_payload_destroy(&request);
    free(strings);free(refs);
    char short_string[]="bad";
    filed_exec_string_ref_t invalid={0,4};
    request=(filed_exec_path_t){.argv=&invalid,.argc=1,.strings=short_string,.string_bytes=3};
    assert(filed_exec_payload_encode(&request,&encoded,&size)==-22);
    puts("filed dynamic exec ABI: PASS");
    return 0;
}
