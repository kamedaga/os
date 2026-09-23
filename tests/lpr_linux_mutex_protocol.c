#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    if (pthread_mutexattr_init(&attr)) return 1;
    int r = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    printf("MUTEX_PROTOCOL inherit=%d expected=%d\n", r, ENOTSUP);
    if (r != ENOTSUP) return 2;
    /* The rejected protocol must leave the default mutex usable. */
    if (pthread_mutex_init(&mutex, &attr) || pthread_mutex_lock(&mutex) ||
        pthread_mutex_unlock(&mutex) || pthread_mutex_destroy(&mutex)) return 3;
    if (pthread_mutexattr_destroy(&attr)) return 4;
    const int ops[] = {6, 7, 8, 11, 12, 13};
    unsigned word = 0;
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        for (unsigned flags = 0; flags <= 128; flags += 128) {
            errno = 0;
            if (syscall(SYS_futex, &word, ops[i] | flags, 0, 0, 0, 0) != -1 ||
                errno != ENOTSUP || word != 0) return 5;
        }
    }
    errno = 0;
    if (syscall(SYS_futex, &word, 127, 0, 0, 0, 0) != -1 || errno != ENOSYS)
        return 6;
    void *pulse = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!pulse) { fprintf(stderr, "PULSE_LOAD %s\n", dlerror()); return 7; }
    void *(*create)(void) = (void *(*)(void))dlsym(pulse, "pa_threaded_mainloop_new");
    void (*release)(void *) = (void (*)(void *))dlsym(pulse, "pa_threaded_mainloop_free");
    int (*start)(void *) = (int (*)(void *))dlsym(pulse, "pa_threaded_mainloop_start");
    void (*stop)(void *) = (void (*)(void *))dlsym(pulse, "pa_threaded_mainloop_stop");
    void (*lock)(void *) = (void (*)(void *))dlsym(pulse, "pa_threaded_mainloop_lock");
    void (*unlock)(void *) = (void (*)(void *))dlsym(pulse, "pa_threaded_mainloop_unlock");
    if (!create || !release || !start || !stop || !lock || !unlock) return 8;
    for (unsigned i = 0; i < 32; ++i) {
        void *loop = create();
        if (!loop) return 9;
        /* Creation alone does not exercise the wakeup descriptor or worker
         * teardown used by WebKit's audio initialization. */
        if (start(loop) != 0) { release(loop); return 10; }
        lock(loop);
        unlock(loop);
        stop(loop);
        release(loop);
    }
    dlclose(pulse);
    puts("PULSE_MUTEX PASS 32 real PulseAudio mainloop start/lock/stop/free cycles");
    puts("MUTEX_PROTOCOL PASS unsupported PI, ordinary mutex, unknown op");
    return 0;
}
