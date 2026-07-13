#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

static int is_wlroots_keymap_name(const char *name) {
    return name != NULL && strncmp(name, "/wlroots-", 9) == 0;
}

int shm_open(const char *name, int flags, mode_t mode) {
    if (is_wlroots_keymap_name(name)) {
        (void)flags;
        (void)mode;
        return memfd_create("wlroots-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    }
    typedef int (*next_fn)(const char *, int, mode_t);
    static next_fn next;
    if (next == NULL) next = (next_fn)(uintptr_t)dlsym(RTLD_NEXT, "shm_open");
    if (next == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next(name, flags, mode);
}

int shm_unlink(const char *name) {
    if (is_wlroots_keymap_name(name)) return 0;
    typedef int (*next_fn)(const char *);
    static next_fn next;
    if (next == NULL) next = (next_fn)(uintptr_t)dlsym(RTLD_NEXT, "shm_unlink");
    if (next == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next(name);
}
