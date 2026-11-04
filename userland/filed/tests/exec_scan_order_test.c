#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {unsigned char *bytes;uint64_t size;} image_t;
int lpr_exec_patch_syscalls(const image_t*,uint64_t,unsigned char*,uint64_t,uint64_t*);
int mc3_linear_first_oracle(const image_t*,uint64_t,unsigned char*,uint64_t,uint64_t*);
int lpr_exec_image_find_text_section(const image_t*,uint64_t*,uint64_t*);
static unsigned cases;
static void compare(const image_t *im,uint64_t offset,size_t size){
 unsigned char *a=malloc(size?size:1),*b=malloc(size?size:1);assert(a&&b);memcpy(a,im->bytes+offset,size);memcpy(b,a,size);
 uint64_t na=999,nb=999;int ra=lpr_exec_patch_syscalls(im,offset,a,size,&na),rb=mc3_linear_first_oracle(im,offset,b,size,&nb);
 if(ra!=rb||na!=nb||memcmp(a,b,size)){fprintf(stderr,"DIFF case=%u off=%llu size=%zu rc=%d/%d sites=%llu/%llu\n",cases,(unsigned long long)offset,size,ra,rb,(unsigned long long)na,(unsigned long long)nb);abort();}
 if(ra)assert(!memcmp(a,im->bytes+offset,size));free(a);free(b);cases++;
}
static uint64_t rng=123456789;
static unsigned random_byte(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng&255;}
int main(int argc,char **argv){
 unsigned char bytes[32768];image_t im={bytes,sizeof bytes};
 for(unsigned n=0;n<10000;n++){size_t size=n%1024;for(size_t i=0;i<size+32;i++)bytes[i]=random_byte();if(size>8){unsigned j=random_byte()%(size-2);bytes[16+j]=0x0f;bytes[17+j]=5;}compare(&im,16,size);}
 memset(bytes,0x90,sizeof bytes);for(unsigned i=0;i<2500;i++){size_t p=i*8;bytes[p]=0xb8;bytes[p+1]=39;bytes[p+2]=bytes[p+3]=bytes[p+4]=0;bytes[p+5]=0x0f;bytes[p+6]=5;bytes[p+7]=0xc3;}compare(&im,0,2500*8);
 for(int f=1;f<argc;f++){FILE *in=fopen(argv[f],"rb");assert(in&&fseek(in,0,SEEK_END)==0);long n=ftell(in);assert(n>0&&fseek(in,0,SEEK_SET)==0);image_t real={malloc(n),n};assert(real.bytes&&fread(real.bytes,1,n,in)==(size_t)n);fclose(in);uint64_t off,size;int rc=lpr_exec_image_find_text_section(&real,&off,&size);if(!rc){assert(off<=real.size&&size<=real.size-off);compare(&real,off,size);}free(real.bytes);}
 printf("exec scan ordering: %u exact legacy comparisons, random/truncated/large-plan/real ELF, failure atomicity PASS\n",cases);
}
