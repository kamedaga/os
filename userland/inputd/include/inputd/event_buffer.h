#ifndef INPUTD_EVENT_BUFFER_H
#define INPUTD_EVENT_BUFFER_H
#include <inputd/event_ring.h>
#include <pacha/ipc.h>
struct inputd_event_buffer { pacha_fd_t fd; struct inputd_event_ring *ring; };
static inline int inputd_event_buffer_create(struct inputd_event_buffer *b) {
    b->ring=0;b->fd=pacha_vmo_create(INPUTD_RING_BYTES,PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,0);
    if(b->fd<16)return -12;
    b->ring=pacha_mmap(b->fd,INPUTD_RING_BYTES,PACHA_PROT_READ|PACHA_PROT_WRITE,PACHA_MMAP_SHARED,0);
    if(!b->ring){(void)pacha_fd_close(b->fd);b->fd=-1;return -12;}return 0;
}
static inline void inputd_event_buffer_close(struct inputd_event_buffer *b) {
    if(b->ring)(void)pacha_munmap(b->ring,INPUTD_RING_BYTES);
    if(b->fd>=16)(void)pacha_fd_close(b->fd);b->ring=0;b->fd=-1;
}
#endif
