/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* One process-owned lease. The lock covers request/reply lifetime; it is
 * deliberately independent of the tty/DRM page so evdev cannot queue behind
 * a GPU wait. No thread owns retained resources after releasing its lease. */
struct lpr_input_wire_cache { int fd; void *page; volatile uint32_t lock; };
struct lpr_input_wire_io {
    void (*lock)(volatile uint32_t *);
    void (*unlock)(volatile uint32_t *);
    int (*create)(void **);
    void (*destroy)(int, void *);
};
static inline int lpr_input_wire_cache_acquire(struct lpr_input_wire_cache *c,
    const struct lpr_input_wire_io *io, void **page)
{
    *page = 0;
    io->lock(&c->lock);
    if (!c->page) {
        void *mapping = 0;
        int fd = io->create(&mapping);
        if (fd < 16 || !mapping) {
            if (fd >= 16) io->destroy(fd, mapping);
            io->unlock(&c->lock);
            return fd < 0 ? fd : -5;
        }
        c->fd = fd;
        c->page = mapping;
    }
    *page = c->page;
    return c->fd;
}
static inline void lpr_input_wire_cache_release(struct lpr_input_wire_cache *c,
    const struct lpr_input_wire_io *io)
{
    io->unlock(&c->lock);
}
static inline void lpr_input_wire_cache_child(struct lpr_input_wire_cache *c,
    const struct lpr_input_wire_io *io)
{
    /* Parent locked the lease across native fork. Its page is shared, while
     * metadata/lock are private copies. Retire only this child's inherited
     * mapping and FD before child transaction RPCs allocate/reuse anything. */
    int fd = c->fd;
    void *page = c->page;
    c->fd = -1;
    c->page = 0;
    c->lock = 0;
    if (fd >= 16 || page) io->destroy(fd, page);
}
