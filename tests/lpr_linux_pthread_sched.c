#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

static void *start(void *value) { return value; }

int main(void)
{
    pthread_attr_t attr;
    struct sched_param param={.sched_priority=0};
    pthread_t thread;
    int result=pthread_attr_init(&attr);
    if (result) return 2;
    result=pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
    if (!result) result=pthread_attr_setschedparam(&attr,&param);
    if (!result) result=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
    if (!result) result=pthread_create(&thread,&attr,start,(void *)123);
    printf("PTHREAD_EXPLICIT_OTHER_CREATE=%d\n",result);
    if (!result) {
        void *value=NULL;
        result=pthread_join(thread,&value);
        if (!result && value!=(void *)123) result=1;
    }
    pthread_attr_destroy(&attr);
    if (result) return 1;
    if (sched_setscheduler(0,SCHED_OTHER,&param) || sched_setparam(0,&param)) return 1;
    if (sched_getscheduler(0)!=SCHED_OTHER || sched_getparam(0,&param) ||
        param.sched_priority!=0) return 1;
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(getpid(),sizeof(affinity),&affinity) ||
        CPU_COUNT(&affinity)==0) return 1;
    puts("PTHREAD_EXPLICIT_SCHED=PASS normal-create-join-and-query");
    return 0;
}
