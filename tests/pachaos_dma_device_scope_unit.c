#include <assert.h>
#ifndef CAPSULE_DMA_SOURCE
#define CAPSULE_DMA_SOURCE "../_kobox/src/device/pachaos_capsule/device_pachaos_capsule.c"
#endif
#include CAPSULE_DMA_SOURCE

static pacha_fd_t closed[8];
static unsigned close_count;

pacha_fd_t pacha_capsule_is_fd(pacha_fd_t fd) { return fd >= 16; }
int pacha_capsule_close(pacha_fd_t fd)
{
    assert(close_count < sizeof(closed)/sizeof(closed[0]));
    closed[close_count++] = fd;
    return 0;
}
int pacha_capsule_device_derive_dma_buffer(pacha_fd_t device_fd, void *addr,
    uint64_t iova, size_t len, uint64_t flags, struct pacha_capsule_dma *out)
{
    (void)iova; (void)flags;
    *out = (struct pacha_capsule_dma){ .fd=device_fd+100, .addr=addr,
        .iova=0x3000000, .len=len };
    return 0;
}

int main(void)
{
    kb_pachaos_capsule_backend_t backend = {0};
    kb_device_t keyboard = {.backend=&backend, .device_fd=20};
    kb_device_t mouse = {.backend=&backend, .device_fd=21};
    kb_device_t other = {.backend=&backend, .device_fd=22};
    kb_dma_buffer_t a={0}, b={0};
    assert(pachaos_capsule_dma_alloc(&keyboard,4096,4096,KB_DMA_BIDIRECTIONAL,&a)==KB_OK);
    assert(pachaos_capsule_dma_alloc(&mouse,4096,4096,KB_DMA_BIDIRECTIONAL,&b)==KB_OK);
    assert(a.dma_addr==b.dma_addr && a.cpu_addr!=b.cpu_addr);
    memset(b.cpu_addr,0x5a,b.size);
    /* Identical IOVAs in separate device domains are unrelated owners. */
    pachaos_capsule_dma_free(&keyboard,&a);
    if(close_count!=1 || closed[0]!=120) {
        fprintf(stderr,"FAIL keyboard free closed fd=%ld; expected keyboard fd=120\n",closed[0]);
        return 1;
    }
    assert(*(unsigned char*)b.cpu_addr==0x5a);
    /* A third device must not find either range, including interior IOVAs. */
    pachaos_capsule_dma_unmap(&other,b.dma_addr+48,8,KB_DMA_FROM_DEVICE);
    assert(close_count==1);
    pachaos_capsule_dma_free(&mouse,&b);
    assert(close_count==2 && closed[1]==121);
    assert(backend.dma_mappings==NULL);
    puts("PASS device-scoped DMA ownership, overlapping IOVA, foreign unmap");
    return 0;
}
