typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int i32;

enum {
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_QUEUE_SUBMIT = 0xE,
    SYSCALL_QUEUE_NOTIFY = 0xF,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_IOMMU_AUTHORIZE = 0x35,
    SYSCALL_DMA_MAP_CREATE = 0x37,
    SYSCALL_DMA_MAP_SET_STATE = 0x38,

    SYSCALL_OK = 0,
    PAGE_BYTES = 4096,

    CONFIG_VA = 0x3C002000,
    VIRTIO_MMIO_BASE_VA = 0x27210000,
    RX_QUEUE_PAGE_VA = 0x27220000,
    TX_QUEUE_PAGE_VA = 0x27221000,
    RX_BUFFER_BASE_VA = 0x27230000,
    TX_BUFFER_VA = 0x27240000,
    CONSOLE_REQUEST_VA = 0x27200000,
    CONSOLE_RESPONSE_VA = 0x27201000,

    CONSOLE_CONFIG_MAGIC = 0x434F4E43,
    CONSOLE_CONFIG_VERSION = 1,
    CONSOLE_STATUS_READY = 0x43524459,
    CONSOLE_ENDPOINT_ID_INDEX = 2,
    CONSOLE_COMMON_PAGE_PADDR_INDEX = 3,
    CONSOLE_NOTIFY_PAGE_PADDR_INDEX = 4,
    CONSOLE_ISR_PAGE_PADDR_INDEX = 5,
    CONSOLE_DEVICE_PAGE_PADDR_INDEX = 6,
    CONSOLE_COMMON_PAGE_OFFSET_INDEX = 7,
    CONSOLE_NOTIFY_PAGE_OFFSET_INDEX = 8,
    CONSOLE_ISR_PAGE_OFFSET_INDEX = 9,
    CONSOLE_DEVICE_PAGE_OFFSET_INDEX = 10,
    CONSOLE_NOTIFY_OFF_MULTIPLIER_INDEX = 11,
    CONSOLE_IOMMU_TOKEN_INDEX = 12,
    CONSOLE_RX_QUEUE_SUBMIT_TOKEN_INDEX = 13,
    CONSOLE_RX_QUEUE_NOTIFY_TOKEN_INDEX = 14,
    CONSOLE_TX_QUEUE_SUBMIT_TOKEN_INDEX = 15,
    CONSOLE_TX_QUEUE_NOTIFY_TOKEN_INDEX = 16,
    CONSOLE_COMMAND_TOKEN_INDEX = 17,
    CONSOLE_RESOURCE_ID_INDEX = 18,
    CONSOLE_DRIVER_STATUS_INDEX = 19,

    COMMON_DEVICE_FEATURE_SELECT = 0x00,
    COMMON_DRIVER_FEATURE_SELECT = 0x08,
    COMMON_DRIVER_FEATURE = 0x0C,
    COMMON_DEVICE_STATUS = 0x14,
    COMMON_QUEUE_SELECT = 0x16,
    COMMON_QUEUE_SIZE = 0x18,
    COMMON_QUEUE_ENABLE = 0x1C,
    COMMON_QUEUE_NOTIFY_OFF = 0x1E,
    COMMON_QUEUE_DESC = 0x20,
    COMMON_QUEUE_AVAIL = 0x28,
    COMMON_QUEUE_USED = 0x30,

    STATUS_ACKNOWLEDGE = 0x01,
    STATUS_DRIVER = 0x02,
    STATUS_DRIVER_OK = 0x04,
    STATUS_FEATURES_OK = 0x08,

    QUEUE_SIZE = 8,
    QUEUE_USED_OFFSET = 2048,
    RX_QUEUE_INDEX = 0,
    TX_QUEUE_INDEX = 1,
    RX_BUFFER_COUNT = 4,
    RX_BUFFER_BYTES = 256,
    TX_BUFFER_BYTES = 256,
    RX_RING_BYTES = 4096,

    CONSOLE_REQUEST_MAGIC = 0x514E4F43,
    CONSOLE_RESPONSE_MAGIC = 0x524E4F43,
    CONSOLE_PROTOCOL_VERSION = 1,
    CONSOLE_OP_CONNECT = 1,
    CONSOLE_OP_READ = 2,
    CONSOLE_OP_WRITE = 3,
    CONSOLE_OP_GET_ATTR = 4,
    CONSOLE_OP_SET_ATTR = 5,
    CONSOLE_REQUEST_FLAG_NONBLOCK = 1 << 0,
    CONSOLE_STATUS_OK = 0,
    CONSOLE_STATUS_AGAIN = 1,
    CONSOLE_STATUS_INVALID = 2,
    CONSOLE_STATUS_IO_ERROR = 3,
    CONSOLE_REQUEST_HEADER_BYTES = 64,
    CONSOLE_RESPONSE_HEADER_BYTES = 64,
    CONSOLE_REQUEST_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_REQUEST_HEADER_BYTES,
    CONSOLE_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_RESPONSE_HEADER_BYTES,
    CONSOLE_REPLY_ENDPOINT_ID = 0xE9,
    CAP_TRANSFER_ID_MIN = 0x1000,

    DESC_FLAG_WRITE = 1 << 1,
    DMA_DIRECTION_READ = 0,
    DMA_DIRECTION_WRITE = 1,
    DMA_STATE_IN_FLIGHT = 1,
    IOMMU_OP_MAP_READ = 0,
    DMA_MAPPING_TOKEN_TAG = (1ULL << 63) | (1ULL << 61),
};

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct virtq_used_elem {
    u32 id;
    u32 len;
};

struct queue_state {
    u16 index;
    u64 submit_token;
    u64 notify_token;
    u64 page_va;
    u64 page_paddr;
    u64 notify_addr;
    u16 last_used_idx;
};

struct boot_state {
    u64 endpoint_id;
    u64 resource_id;
    u64 common_page_paddr;
    u64 notify_page_paddr;
    u64 isr_page_paddr;
    u64 device_page_paddr;
    u64 common_page_offset;
    u64 notify_page_offset;
    u64 isr_page_offset;
    u64 device_page_offset;
    u64 notify_off_multiplier;
    u64 iommu_token;
    u64 command_token;
};

struct console_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 session_nonce;
    u32 length;
    u32 flags;
    u64 arg0;
    u64 arg1;
    u64 arg2;
    u64 reserved0;
};

struct console_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    i32 status;
    u32 result_flags;
    u32 inline_bytes;
    u32 reserved0;
    u64 arg0;
    u64 arg1;
    u64 reserved1;
    u64 reserved2;
};

struct console_session {
    int active;
    u64 request_paddr;
    u64 response_paddr;
    u64 reply_endpoint_id;
    u64 session_nonce;
    u64 last_completed_seq;
};

static struct boot_state g_boot;
static struct queue_state g_rx_queue = { RX_QUEUE_INDEX, 0, 0, RX_QUEUE_PAGE_VA, 0, 0, 0 };
static struct queue_state g_tx_queue = { TX_QUEUE_INDEX, 0, 0, TX_QUEUE_PAGE_VA, 0, 0, 0 };
static struct console_session g_session;
static u64 g_common_base;
static u64 g_notify_base;
static u64 g_isr_base;
static u64 g_rx_buffer_paddrs[RX_BUFFER_COUNT];
static u64 g_rx_dma_tokens[RX_BUFFER_COUNT];
static u64 g_tx_buffer_paddr;
static u64 g_tx_dma_token;
static u8 g_rx_ring[RX_RING_BYTES];
static u64 g_rx_ring_head;
static u64 g_rx_ring_len;
static u64 g_pending_read_seq;
static u64 g_pending_read_max_len;

void *memset(void *dst, int value, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = (u8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static u64 syscall1(u64 nr, u64 a0) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall2(u64 nr, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event(u64 wait_mailbox, u64 timeout_ticks) {
    return syscall2(SYSCALL_WAIT_EVENT, wait_mailbox, timeout_ticks);
}

static u64 accept_cap_transfer(u64 transfer_id) {
    return syscall1(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id);
}

static u64 install_endpoint(u64 endpoint_id, u64 target_process_handle) {
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, target_process_handle);
}

static u64 signal_endpoint(u64 endpoint_id) {
    return syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
}

static u64 alloc_map_pages(u64 base_va, u64 page_count, u64 writable, u64 paddrs_out) {
    return syscall4(SYSCALL_ALLOC_MAP_PAGES, base_va, page_count, writable, paddrs_out);
}

static u64 map_page(u64 va, u64 paddr, u64 writable) {
    return syscall3(SYSCALL_MAP_PAGE, va, paddr, writable);
}

static u64 queue_submit(u64 token, u64 queue_index) {
    return syscall2(SYSCALL_QUEUE_SUBMIT, token, queue_index);
}

static u64 queue_notify(u64 token, u64 queue_index) {
    return syscall2(SYSCALL_QUEUE_NOTIFY, token, queue_index);
}

static u64 iommu_authorize(u64 token, u64 device, u64 op) {
    return syscall3(SYSCALL_IOMMU_AUTHORIZE, token, device, op);
}

static u64 dma_map_create(u64 device, u64 paddr_start, u64 length, u64 direction) {
    const u64 raw = syscall4(SYSCALL_DMA_MAP_CREATE, device, paddr_start, length, direction);
    if ((raw & DMA_MAPPING_TOKEN_TAG) != DMA_MAPPING_TOKEN_TAG) return 0;
    return raw & ~DMA_MAPPING_TOKEN_TAG;
}

static u64 dma_map_set_state(u64 token, u64 state) {
    return syscall2(SYSCALL_DMA_MAP_SET_STATE, token, state);
}

static u8 mmio_read_u8(u64 addr) {
    volatile u8 *p = (volatile u8 *)addr;
    return *p;
}

static u16 mmio_read_u16(u64 addr) {
    volatile u16 *p = (volatile u16 *)addr;
    return *p;
}

static void mmio_write_u8(u64 addr, u8 value) {
    volatile u8 *p = (volatile u8 *)addr;
    *p = value;
}

static void mmio_write_u16(u64 addr, u16 value) {
    volatile u16 *p = (volatile u16 *)addr;
    *p = value;
}

static void mmio_write_u32(u64 addr, u32 value) {
    volatile u32 *p = (volatile u32 *)addr;
    *p = value;
}

static void mmio_write_u64(u64 addr, u64 value) {
    volatile u64 *p = (volatile u64 *)addr;
    *p = value;
}

static u64 cfg_read(u64 index) {
    volatile u64 *cfg = (volatile u64 *)CONFIG_VA;
    return cfg[index];
}

static void cfg_write(u64 index, u64 value) {
    volatile u64 *cfg = (volatile u64 *)CONFIG_VA;
    cfg[index] = value;
}

static int parse_boot_state(void) {
    if (cfg_read(0) != CONSOLE_CONFIG_MAGIC || cfg_read(1) != CONSOLE_CONFIG_VERSION) return 0;
    g_rx_queue.submit_token = cfg_read(CONSOLE_RX_QUEUE_SUBMIT_TOKEN_INDEX);
    g_rx_queue.notify_token = cfg_read(CONSOLE_RX_QUEUE_NOTIFY_TOKEN_INDEX);
    g_tx_queue.submit_token = cfg_read(CONSOLE_TX_QUEUE_SUBMIT_TOKEN_INDEX);
    g_tx_queue.notify_token = cfg_read(CONSOLE_TX_QUEUE_NOTIFY_TOKEN_INDEX);
    g_boot.endpoint_id = cfg_read(CONSOLE_ENDPOINT_ID_INDEX);
    g_boot.resource_id = cfg_read(CONSOLE_RESOURCE_ID_INDEX);
    g_boot.common_page_paddr = cfg_read(CONSOLE_COMMON_PAGE_PADDR_INDEX);
    g_boot.notify_page_paddr = cfg_read(CONSOLE_NOTIFY_PAGE_PADDR_INDEX);
    g_boot.isr_page_paddr = cfg_read(CONSOLE_ISR_PAGE_PADDR_INDEX);
    g_boot.device_page_paddr = cfg_read(CONSOLE_DEVICE_PAGE_PADDR_INDEX);
    g_boot.common_page_offset = cfg_read(CONSOLE_COMMON_PAGE_OFFSET_INDEX);
    g_boot.notify_page_offset = cfg_read(CONSOLE_NOTIFY_PAGE_OFFSET_INDEX);
    g_boot.isr_page_offset = cfg_read(CONSOLE_ISR_PAGE_OFFSET_INDEX);
    g_boot.device_page_offset = cfg_read(CONSOLE_DEVICE_PAGE_OFFSET_INDEX);
    g_boot.notify_off_multiplier = cfg_read(CONSOLE_NOTIFY_OFF_MULTIPLIER_INDEX);
    g_boot.iommu_token = cfg_read(CONSOLE_IOMMU_TOKEN_INDEX);
    g_boot.command_token = cfg_read(CONSOLE_COMMAND_TOKEN_INDEX);
    return 1;
}

static void wait_for_boot_resources(void) {
    for (;;) {
        if (parse_boot_state() &&
            g_boot.endpoint_id != 0 &&
            g_boot.resource_id != ~0ULL &&
            g_rx_queue.submit_token != 0 &&
            g_rx_queue.notify_token != 0 &&
            g_tx_queue.submit_token != 0 &&
            g_tx_queue.notify_token != 0) {
            return;
        }
        (void)wait_event(0, 1);
    }
}

static volatile struct virtq_desc *desc_ptr(struct queue_state *queue, u64 index) {
    return (volatile struct virtq_desc *)(queue->page_va + index * sizeof(struct virtq_desc));
}

static volatile u16 *avail_idx_ptr(struct queue_state *queue) {
    return (volatile u16 *)(queue->page_va + QUEUE_SIZE * sizeof(struct virtq_desc) + 2);
}

static volatile u16 *avail_ring_ptr(struct queue_state *queue) {
    return (volatile u16 *)(queue->page_va + QUEUE_SIZE * sizeof(struct virtq_desc) + 4);
}

static volatile u16 *used_idx_ptr(struct queue_state *queue) {
    return (volatile u16 *)(queue->page_va + QUEUE_USED_OFFSET + 2);
}

static volatile struct virtq_used_elem *used_ring_ptr(struct queue_state *queue) {
    return (volatile struct virtq_used_elem *)(queue->page_va + QUEUE_USED_OFFSET + 4);
}

static void queue_push_avail(struct queue_state *queue, u16 desc_index) {
    const u16 idx = *avail_idx_ptr(queue);
    avail_ring_ptr(queue)[idx % QUEUE_SIZE] = desc_index;
    __asm__ volatile("" ::: "memory");
    *avail_idx_ptr(queue) = (u16)(idx + 1);
}

static int setup_queue(struct queue_state *queue) {
    mmio_write_u16(g_common_base + COMMON_QUEUE_SELECT, queue->index);
    const u16 max_size = mmio_read_u16(g_common_base + COMMON_QUEUE_SIZE);
    if (max_size == 0 || max_size < QUEUE_SIZE) return 0;
    mmio_write_u16(g_common_base + COMMON_QUEUE_SIZE, QUEUE_SIZE);
    mmio_write_u64(g_common_base + COMMON_QUEUE_DESC, queue->page_paddr);
    mmio_write_u64(g_common_base + COMMON_QUEUE_AVAIL, queue->page_paddr + QUEUE_SIZE * sizeof(struct virtq_desc));
    mmio_write_u64(g_common_base + COMMON_QUEUE_USED, queue->page_paddr + QUEUE_USED_OFFSET);
    const u16 queue_notify_off = mmio_read_u16(g_common_base + COMMON_QUEUE_NOTIFY_OFF);
    queue->notify_addr = g_notify_base + (u64)queue_notify_off * g_boot.notify_off_multiplier;
    mmio_write_u16(g_common_base + COMMON_QUEUE_ENABLE, 1);
    return 1;
}

static int init_queue_memory(void) {
    u64 q_paddrs[2] = { 0, 0 };
    if (alloc_map_pages(RX_QUEUE_PAGE_VA, 1, 1, (u64)&q_paddrs[0]) != SYSCALL_OK) return 0;
    if (alloc_map_pages(TX_QUEUE_PAGE_VA, 1, 1, (u64)&q_paddrs[1]) != SYSCALL_OK) return 0;
    g_rx_queue.page_paddr = q_paddrs[0];
    g_tx_queue.page_paddr = q_paddrs[1];
    memset((void *)RX_QUEUE_PAGE_VA, 0, PAGE_BYTES);
    memset((void *)TX_QUEUE_PAGE_VA, 0, PAGE_BYTES);

    for (u64 i = 0; i < RX_BUFFER_COUNT; i++) {
        if (alloc_map_pages(RX_BUFFER_BASE_VA + i * PAGE_BYTES, 1, 1, (u64)&g_rx_buffer_paddrs[i]) != SYSCALL_OK) return 0;
        memset((void *)(RX_BUFFER_BASE_VA + i * PAGE_BYTES), 0, PAGE_BYTES);
    }
    if (alloc_map_pages(TX_BUFFER_VA, 1, 1, (u64)&g_tx_buffer_paddr) != SYSCALL_OK) return 0;
    memset((void *)TX_BUFFER_VA, 0, PAGE_BYTES);
    return 1;
}

static int authorize_dma(void) {
    if (g_boot.iommu_token == 0) return 1;
    if (iommu_authorize(g_boot.iommu_token, g_boot.resource_id, IOMMU_OP_MAP_READ) != SYSCALL_OK) return 0;
    for (u64 i = 0; i < RX_BUFFER_COUNT; i++) {
        g_rx_dma_tokens[i] = dma_map_create(g_boot.resource_id, g_rx_buffer_paddrs[i], RX_BUFFER_BYTES, DMA_DIRECTION_WRITE);
        if (g_rx_dma_tokens[i] == 0) return 0;
        if (dma_map_set_state(g_rx_dma_tokens[i], DMA_STATE_IN_FLIGHT) != SYSCALL_OK) return 0;
    }
    if (iommu_authorize(g_boot.iommu_token, g_boot.resource_id, IOMMU_OP_MAP_READ) != SYSCALL_OK) return 0;
    g_tx_dma_token = dma_map_create(g_boot.resource_id, g_tx_buffer_paddr, TX_BUFFER_BYTES, DMA_DIRECTION_READ);
    if (g_tx_dma_token == 0) return 0;
    return dma_map_set_state(g_tx_dma_token, DMA_STATE_IN_FLIGHT) == SYSCALL_OK;
}

static int prime_rx_queue(void) {
    for (u64 i = 0; i < RX_BUFFER_COUNT; i++) {
        volatile struct virtq_desc *desc = desc_ptr(&g_rx_queue, i);
        desc->addr = g_rx_buffer_paddrs[i];
        desc->len = RX_BUFFER_BYTES;
        desc->flags = DESC_FLAG_WRITE;
        desc->next = 0;
        queue_push_avail(&g_rx_queue, (u16)i);
    }
    if (queue_submit(g_rx_queue.submit_token, g_rx_queue.index) != SYSCALL_OK) return 0;
    if (queue_notify(g_rx_queue.notify_token, g_rx_queue.index) != SYSCALL_OK) return 0;
    mmio_write_u16(g_rx_queue.notify_addr, g_rx_queue.index);
    return 1;
}

static int init_virtio(void) {
    while (map_page(VIRTIO_MMIO_BASE_VA, g_boot.common_page_paddr, 1) != SYSCALL_OK) (void)wait_event(0, 1);
    while (map_page(VIRTIO_MMIO_BASE_VA + 0x1000, g_boot.notify_page_paddr, 1) != SYSCALL_OK) (void)wait_event(0, 1);
    if (g_boot.isr_page_paddr != 0) {
        while (map_page(VIRTIO_MMIO_BASE_VA + 0x2000, g_boot.isr_page_paddr, 0) != SYSCALL_OK) (void)wait_event(0, 1);
    }
    if (g_boot.device_page_paddr != 0) {
        while (map_page(VIRTIO_MMIO_BASE_VA + 0x3000, g_boot.device_page_paddr, 1) != SYSCALL_OK) (void)wait_event(0, 1);
    }

    if (!init_queue_memory()) return 0;

    g_common_base = VIRTIO_MMIO_BASE_VA + g_boot.common_page_offset;
    g_notify_base = VIRTIO_MMIO_BASE_VA + 0x1000 + g_boot.notify_page_offset;
    g_isr_base = g_boot.isr_page_paddr != 0 ? VIRTIO_MMIO_BASE_VA + 0x2000 + g_boot.isr_page_offset : 0;

    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, 0);
    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    mmio_write_u32(g_common_base + COMMON_DEVICE_FEATURE_SELECT, 0);
    mmio_write_u32(g_common_base + COMMON_DRIVER_FEATURE_SELECT, 0);
    mmio_write_u32(g_common_base + COMMON_DRIVER_FEATURE, 0);
    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) | STATUS_FEATURES_OK);
    if ((mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) & STATUS_FEATURES_OK) == 0) return 0;

    if (!setup_queue(&g_rx_queue)) return 0;
    if (!setup_queue(&g_tx_queue)) return 0;
    if (!authorize_dma()) return 0;
    if (!prime_rx_queue()) return 0;

    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) | STATUS_DRIVER_OK);
    return 1;
}

static int send_bytes(const u8 *bytes, u64 len) {
    if (len == 0 || len > TX_BUFFER_BYTES) return 0;
    memcpy((void *)TX_BUFFER_VA, bytes, len);

    volatile struct virtq_desc *desc = desc_ptr(&g_tx_queue, 0);
    desc->addr = g_tx_buffer_paddr;
    desc->len = (u32)len;
    desc->flags = 0;
    desc->next = 0;
    if (queue_submit(g_tx_queue.submit_token, g_tx_queue.index) != SYSCALL_OK) return 0;
    queue_push_avail(&g_tx_queue, 0);
    if (queue_notify(g_tx_queue.notify_token, g_tx_queue.index) != SYSCALL_OK) return 0;
    mmio_write_u16(g_tx_queue.notify_addr, g_tx_queue.index);

    for (u64 spins = 0; spins < 1000000; spins++) {
        if (*used_idx_ptr(&g_tx_queue) != g_tx_queue.last_used_idx) {
            if (g_isr_base != 0) (void)mmio_read_u8(g_isr_base);
            (void)used_ring_ptr(&g_tx_queue)[g_tx_queue.last_used_idx % QUEUE_SIZE];
            g_tx_queue.last_used_idx++;
            return 1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

static struct console_request_header *request_header(void) {
    return (struct console_request_header *)CONSOLE_REQUEST_VA;
}

static struct console_response_header *response_header(void) {
    return (struct console_response_header *)CONSOLE_RESPONSE_VA;
}

static volatile u8 *request_payload(void) {
    return (volatile u8 *)(CONSOLE_REQUEST_VA + CONSOLE_REQUEST_HEADER_BYTES);
}

static volatile u8 *response_payload(void) {
    return (volatile u8 *)(CONSOLE_RESPONSE_VA + CONSOLE_RESPONSE_HEADER_BYTES);
}

static void clear_page(u64 base_va) {
    memset((void *)base_va, 0, PAGE_BYTES);
}

static void write_console_response(u16 op, u64 seq, i32 status, u32 inline_bytes, u64 arg0, u64 arg1) {
    struct console_response_header *response = response_header();
    response->magic = CONSOLE_RESPONSE_MAGIC;
    response->version = CONSOLE_PROTOCOL_VERSION;
    response->op = op;
    response->status = status;
    response->result_flags = 0;
    response->inline_bytes = inline_bytes;
    response->reserved0 = 0;
    response->arg0 = arg0;
    response->arg1 = arg1;
    response->reserved1 = 0;
    response->reserved2 = 0;
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
    if (g_session.reply_endpoint_id != 0) (void)signal_endpoint(g_session.reply_endpoint_id);
}

static void rx_ring_push(u8 byte) {
    if (g_rx_ring_len >= RX_RING_BYTES) {
        g_rx_ring_head = (g_rx_ring_head + 1) % RX_RING_BYTES;
        g_rx_ring_len--;
    }
    const u64 tail = (g_rx_ring_head + g_rx_ring_len) % RX_RING_BYTES;
    g_rx_ring[tail] = byte;
    g_rx_ring_len++;
}

static u64 rx_ring_pop_into(volatile u8 *dst, u64 max_len) {
    const u64 n = g_rx_ring_len < max_len ? g_rx_ring_len : max_len;
    for (u64 i = 0; i < n; i++) dst[i] = g_rx_ring[(g_rx_ring_head + i) % RX_RING_BYTES];
    g_rx_ring_head = (g_rx_ring_head + n) % RX_RING_BYTES;
    g_rx_ring_len -= n;
    return n;
}

static void fulfill_pending_read(void) {
    if (g_pending_read_seq == 0 || g_rx_ring_len == 0) return;
    const u64 n = rx_ring_pop_into(response_payload(), g_pending_read_max_len);
    write_console_response(CONSOLE_OP_READ, g_pending_read_seq, CONSOLE_STATUS_OK, (u32)n, n, 0);
    g_session.last_completed_seq = g_pending_read_seq;
    g_pending_read_seq = 0;
    g_pending_read_max_len = 0;
}

static void handle_input_byte(u8 byte) {
    rx_ring_push(byte);
}

static void reset_input_state(void) {
    g_rx_ring_head = 0;
    g_rx_ring_len = 0;
    g_pending_read_seq = 0;
    g_pending_read_max_len = 0;
}

static int write_volatile_to_tx(volatile u8 *src, u64 len) {
    u8 buf[TX_BUFFER_BYTES];
    u64 done = 0;
    while (done < len) {
        const u64 chunk = len - done < sizeof(buf) ? len - done : sizeof(buf);
        for (u64 i = 0; i < chunk; i++) buf[i] = src[done + i];
        if (!send_bytes(buf, chunk)) return 0;
        done += chunk;
    }
    return 1;
}

static void handle_console_connect_transfer(u64 transfer_id) {
    const u64 request_paddr = accept_cap_transfer(transfer_id);
    if (request_paddr < PAGE_BYTES) {
        user_log("VirtioConsole: accept cap transfer failed\n");
        return;
    }
    if (map_page(CONSOLE_REQUEST_VA, request_paddr, 0) != SYSCALL_OK) return;

    struct console_request_header *request = request_header();
    if (request->magic != CONSOLE_REQUEST_MAGIC ||
        request->version != CONSOLE_PROTOCOL_VERSION ||
        request->op != CONSOLE_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < PAGE_BYTES ||
        request->session_nonce == 0) {
        user_log("VirtioConsole: invalid connect request\n");
        return;
    }

    if (map_page(CONSOLE_RESPONSE_VA, request->arg0, 1) != SYSCALL_OK) return;
    clear_page(CONSOLE_RESPONSE_VA);
    g_session.active = 1;
    g_session.request_paddr = request_paddr;
    g_session.response_paddr = request->arg0;
    g_session.reply_endpoint_id =
        install_endpoint(CONSOLE_REPLY_ENDPOINT_ID, request->arg1) == SYSCALL_OK ? CONSOLE_REPLY_ENDPOINT_ID : 0;
    g_session.session_nonce = request->session_nonce;
    g_session.last_completed_seq = 0;
    reset_input_state();
    write_console_response(CONSOLE_OP_CONNECT, request->request_seq, CONSOLE_STATUS_OK, 0, 0, 0);
    g_session.last_completed_seq = request->request_seq;
    user_log("VirtioConsole: session connect ok\n");
}

static void handle_console_request(void) {
    if (!g_session.active) return;

    struct console_request_header *request = request_header();
    if (request->magic != CONSOLE_REQUEST_MAGIC || request->version != CONSOLE_PROTOCOL_VERSION) return;
    const u64 seq = request->request_seq;
    if (seq == 0 || seq <= g_session.last_completed_seq) return;
    if (request->session_nonce != g_session.session_nonce) return;

    if (request->op == CONSOLE_OP_READ) {
        const u64 max_len = request->length < CONSOLE_RESPONSE_PAYLOAD_BYTES ? request->length : CONSOLE_RESPONSE_PAYLOAD_BYTES;
        if (max_len == 0) {
            write_console_response(CONSOLE_OP_READ, seq, CONSOLE_STATUS_INVALID, 0, 0, 0);
        } else if (g_rx_ring_len == 0 && (request->flags & CONSOLE_REQUEST_FLAG_NONBLOCK) != 0) {
            write_console_response(CONSOLE_OP_READ, seq, CONSOLE_STATUS_AGAIN, 0, 0, 0);
        } else if (g_rx_ring_len == 0) {
            if (g_pending_read_seq == 0 || g_pending_read_seq == seq) {
                g_pending_read_seq = seq;
                g_pending_read_max_len = max_len;
            } else {
                write_console_response(CONSOLE_OP_READ, seq, CONSOLE_STATUS_AGAIN, 0, 0, 0);
                g_session.last_completed_seq = seq;
            }
            return;
        } else {
            const u64 n = rx_ring_pop_into(response_payload(), max_len);
            write_console_response(CONSOLE_OP_READ, seq, CONSOLE_STATUS_OK, (u32)n, n, 0);
        }
    } else if (request->op == CONSOLE_OP_WRITE) {
        const u64 len = request->length < CONSOLE_REQUEST_PAYLOAD_BYTES ? request->length : CONSOLE_REQUEST_PAYLOAD_BYTES;
        if (write_volatile_to_tx(request_payload(), len)) {
            write_console_response(CONSOLE_OP_WRITE, seq, CONSOLE_STATUS_OK, 0, len, 0);
        } else {
            write_console_response(CONSOLE_OP_WRITE, seq, CONSOLE_STATUS_IO_ERROR, 0, 0, 0);
        }
    } else if (request->op == CONSOLE_OP_GET_ATTR) {
        write_console_response(CONSOLE_OP_GET_ATTR, seq, CONSOLE_STATUS_OK, 0, 120, 40);
    } else if (request->op == CONSOLE_OP_SET_ATTR) {
        write_console_response(CONSOLE_OP_SET_ATTR, seq, CONSOLE_STATUS_OK, 0, 0, 0);
    } else if (request->op == CONSOLE_OP_CONNECT) {
        write_console_response(CONSOLE_OP_CONNECT, seq, CONSOLE_STATUS_OK, 0, 0, 0);
    } else {
        write_console_response(request->op, seq, CONSOLE_STATUS_INVALID, 0, 0, 0);
    }
    g_session.last_completed_seq = seq;
}

static void poll_rx_queue(void) {
    int requeued = 0;
    while (*used_idx_ptr(&g_rx_queue) != g_rx_queue.last_used_idx) {
        volatile struct virtq_used_elem *used = &used_ring_ptr(&g_rx_queue)[g_rx_queue.last_used_idx % QUEUE_SIZE];
        const u16 desc_index = (u16)(used->id % QUEUE_SIZE);
        u32 len = used->len;
        if (len > RX_BUFFER_BYTES) len = RX_BUFFER_BYTES;
        g_rx_queue.last_used_idx++;

        const u8 *src = (const u8 *)(RX_BUFFER_BASE_VA + (u64)desc_index * PAGE_BYTES);
        for (u32 i = 0; i < len; i++) handle_input_byte(src[i]);
        queue_push_avail(&g_rx_queue, desc_index);
        requeued = 1;
    }
    if (requeued) (void)queue_submit(g_rx_queue.submit_token, g_rx_queue.index);
    fulfill_pending_read();
    if (queue_notify(g_rx_queue.notify_token, g_rx_queue.index) == SYSCALL_OK) {
        mmio_write_u16(g_rx_queue.notify_addr, g_rx_queue.index);
    }
    if (g_isr_base != 0) (void)mmio_read_u8(g_isr_base);
}

void virtio_console_main(void) {
    user_log("VirtioConsole: started\n");
    wait_for_boot_resources();
    if (!init_virtio()) {
        user_log("VirtioConsole: init failed\n");
        for (;;) (void)wait_event(0, 1);
    }

    static const u8 ready_msg[] = "VirtioConsole: ready\n";
    (void)send_bytes(ready_msg, sizeof(ready_msg) - 1);
    cfg_write(CONSOLE_DRIVER_STATUS_INDEX, CONSOLE_STATUS_READY);
    user_log("VirtioConsole: ready\n");

    for (;;) {
        poll_rx_queue();
        fulfill_pending_read();
        const u64 received = wait_event(1, 1);
        if (received >= CAP_TRANSFER_ID_MIN) handle_console_connect_transfer(received);
        handle_console_request();
    }
}
