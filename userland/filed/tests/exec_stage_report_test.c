#define _GNU_SOURCE
#define FILED_EXEC_STAGE_REPORT 1
#include <assert.h>
#include "../src/manifest_storage.c"
static unsigned opens,closes,renames,unlinks,collisions;
static int short_write,rename_error;
static uint8_t staging[32768],published[32768];static size_t stage_size,published_size;
int filed_storage_pager_request(filed_runtime_t *rt,uint64_t client,uint64_t credential,kb2_fs_request_t *r,kb2_fs_response_t *s,void *out,size_t capacity){(void)rt;(void)s;(void)out;(void)capacity;assert(client==1&&credential==1&&r->opcode==KB2_FILESYSTEM_OP_CLOSE&&r->handle==99);closes++;return 0;}
int filed_storage_pager_request_ex(filed_runtime_t *rt,uint64_t client,uint64_t credential,kb2_fs_request_t *r,const void *tail,size_t n,kb2_fs_response_t *s,void *out,size_t capacity){
 (void)rt;(void)out;(void)capacity;assert(client==1&&credential==1);memset(s,0,sizeof(*s));
 switch(r->opcode){
 case KB2_FILESYSTEM_OP_MKDIRAT:assert(r->mode==0700);return -EEXIST;
 case KB2_FILESYSTEM_OP_OPENAT2:assert(r->flags==(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC)&&r->mode==0600);if(collisions){collisions--;return -EEXIST;}opens++;s->handle=99;stage_size=0;return 0;
 case KB2_FILESYSTEM_OP_PWRITE:assert(r->handle==99&&(uint64_t)r->offset==stage_size&&n==r->length&&stage_size+n<=sizeof staging);memcpy(staging+stage_size,tail,n);stage_size+=n;s->result=n-(short_write?1:0);return 0;
 case KB2_FILESYSTEM_OP_RENAMEAT2:assert(r->second_path.offset==KB2_FILESYSTEM_REQUEST_SIZE+r->path.length);assert(!strcmp((const char*)tail+r->path.length,"/var/cache/filed/mc3-exec.metrics"));if(rename_error)return -EIO;memcpy(published,staging,stage_size);published_size=stage_size;renames++;return 0;
 case KB2_FILESYSTEM_OP_UNLINKAT:unlinks++;return 0;
 default:assert(0);return -EIO;
 }
}
int filed_storage_pager_pread(filed_runtime_t *rt,uint64_t c,uint64_t cred,uint64_t h,uint64_t off,void *p,size_t n,size_t *got){(void)rt;(void)c;(void)cred;(void)h;(void)off;(void)p;(void)n;(void)got;assert(0);return -EIO;}
int main(void){static filed_runtime_t rt;uint8_t data[32768];memset(data,0x5a,sizeof data);collisions=2;assert(!filed_exec_stage_report_store(&rt,data,sizeof data));assert(opens==1&&closes==1&&renames==1&&published_size==sizeof data&&!memcmp(data,published,sizeof data));
 short_write=1;data[0]=0x6a;assert(filed_exec_stage_report_store(&rt,data,32)==-EIO&&renames==1&&unlinks==1&&closes==2&&published[0]==0x5a);short_write=0;rename_error=1;assert(filed_exec_stage_report_store(&rt,data,32)==-EIO&&renames==1&&unlinks==2&&closes==3);rename_error=0;collisions=16;assert(filed_exec_stage_report_store(&rt,data,32)==-EEXIST&&opens==3&&closes==3);assert(filed_exec_stage_report_store(&rt,data,32769)==-EINVAL);puts("actual exec report: fixed private credentials/path, exclusive staging, chunking, short write, failed rename, collisions, preservation and cleanup PASS");}
