/* Test-only driver for a dedicated modern virtio-rng QEMU function.
 * Uses native capabilities, real MMIO, DMA and MSI-X; no LPR or GPU service.
 * Launch grants exactly one spare RNG device to FD 224. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"
#include "pacha/capsule.h"

#define DEVICE_FD 224u
#define PAGE 4096u
#define DMA_SIZE (4u * PAGE)
/* Inside the native VT-d allocator's existing 0x80000000..0x90000000 aperture. */
#define TEST_IOVA UINT64_C(0x83000000)
#define MAP_BASE UINT64_C(0x200000000)
#define STR1(x) #x
#define STR(x) STR1(x)

static const char *stage = "entry";
static uint64_t last_result;
static unsigned char dma_bytes[DMA_SIZE] __attribute__((aligned(PAGE)));
static unsigned char fault_stack[16384] __attribute__((aligned(PAGE)));
static volatile uint64_t fault_expected, fault_resume, fault_seen;

/* Adapter fixtures reuse the hardware helpers with their own runtime/entry. */
#ifndef NATIVE_DEVICE_EXTERNAL_RUNTIME
void *memset(void *destination, int value, size_t size)
{
    unsigned char *p = destination;
    for (size_t i = 0; i < size; i++) p[i] = (unsigned char)value;
    return destination;
}
#endif

static uint64_t call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f)
{
    register uint64_t r10 __asm__("r10") = d;
    register uint64_t r8 __asm__("r8") = e;
    register uint64_t r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
        "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    last_result = nr;
    return nr;
}

static void log_text(const char *text)
{
    size_t n = 0;
    while (text[n]) n++;
    (void)call(1, (uintptr_t)text, n, 0, 0, 0, 0); /* Native boot log syscall. */
}

static void log_number(uint64_t value)
{
    char text[19] = "0x0000000000000000\n";
    for (unsigned i = 0; i < 16; i++)
        text[17 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    log_text(text);
}

__attribute__((noreturn)) static void fail_at(unsigned line)
{
    const uint64_t result = last_result;
    log_text("NATIVE_DEVICE_CONTRACT=FAIL stage="); log_text(stage);
    log_text(" line="); log_number(line);
    log_text("last_result="); log_number(result);
    (void)call(PACHA_PROCESS_SYSCALL_EXIT, 1, 0, 0, 0, 0, 0);
    __builtin_trap();
}
#define CHECK(condition) do { if (!(condition)) fail_at(__LINE__); } while (0)

static int is_fd(uint64_t value) { return value >= 16 && value < PACHA_FD_TABLE_LIMIT; }
static uint64_t now(void)
{
    uint64_t ts[2];
    CHECK(call(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)ts, 0, 0, 0, 0) == 0);
    return ts[0] * UINT64_C(1000000000) + ts[1];
}

static void close_fd(uint64_t fd)
{
    CHECK(is_fd(fd));
    CHECK(call(PACHA_FD_SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0) == 0);
}

static uint32_t config(unsigned offset, unsigned width)
{
    uint32_t value = 0;
    const uint64_t result = call(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ, DEVICE_FD,
        offset, (uintptr_t)&value, width, 0, 0);
    if (result != 0) {
        log_text("pci_config offset="); log_number(offset);
        log_text("pci_config width="); log_number(width);
        last_result = result;
        fail_at(__LINE__);
    }
    return value;
}

static void set_config(unsigned offset, unsigned width, uint32_t value)
{
    CHECK(call(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_WRITE, DEVICE_FD, offset,
        (uintptr_t)&value, width, 0, 0) == 0);
}

static struct pacha_capsule_info query(uint64_t fd)
{
    struct pacha_capsule_info info;
    CHECK(call(PACHA_CAPSULE_SYSCALL_QUERY, fd, (uintptr_t)&info, 11, 0, 0, 0) == 11);
    return info;
}

static struct pacha_capsule_bar_info bar_info(unsigned bar)
{
    struct pacha_capsule_bar_info info;
    CHECK(call(PACHA_CAPSULE_SYSCALL_PCI_BAR_INFO, DEVICE_FD, bar,
        (uintptr_t)&info, 4, 0, 0) == 4);
    CHECK(info.flags & PACHA_CAPSULE_BAR_MEM);
    return info;
}

static uint64_t map_bar(unsigned bar, uint64_t page_offset, uint64_t address, uint64_t flags)
{
    return call(PACHA_CAPSULE_SYSCALL_DERIVE_MMIO, DEVICE_FD, bar,
        address, PAGE, flags, page_offset);
}

struct mapped_page { unsigned bar; uint64_t offset, address, fd; };
static struct mapped_page maps[12];
static unsigned map_count;

static uintptr_t map_register(unsigned bar, uint64_t offset, unsigned width)
{
    const struct pacha_capsule_bar_info info = bar_info(bar);
    CHECK(offset < info.size && width <= info.size - offset);
    const uint64_t physical_offset = (info.start & (PAGE - 1)) + offset;
    const uint64_t page_offset = physical_offset & ~(uint64_t)(PAGE - 1);
    CHECK(width <= PAGE - (physical_offset & (PAGE - 1)));
    for (unsigned i = 0; i < map_count; i++)
        if (maps[i].bar == bar && maps[i].offset == page_offset)
            return maps[i].address + (physical_offset & (PAGE - 1));
    CHECK(map_count < sizeof(maps) / sizeof(maps[0]));
    struct mapped_page *mapping = &maps[map_count];
    mapping->bar = bar;
    mapping->offset = page_offset;
    mapping->address = MAP_BASE + (uint64_t)map_count * 0x10000;
    mapping->fd = map_bar(bar, page_offset, mapping->address, 0);
    CHECK(is_fd(mapping->fd));
    const struct pacha_capsule_info snapshot = query(mapping->fd);
    CHECK(snapshot.object_id == (info.start & ~(uint64_t)(PAGE - 1)) + page_offset);
    CHECK(snapshot.size == PAGE && snapshot.user_va == mapping->address);
    map_count++;
    return mapping->address + (physical_offset & (PAGE - 1));
}

static uint8_t r8(uintptr_t p) { return *(volatile uint8_t *)p; }
static uint16_t r16(uintptr_t p) { return *(volatile uint16_t *)p; }
static uint32_t r32(uintptr_t p) { return *(volatile uint32_t *)p; }
static void w8(uintptr_t p, uint8_t v) { *(volatile uint8_t *)p = v; }
static void w16(uintptr_t p, uint16_t v) { *(volatile uint16_t *)p = v; }
static void w32(uintptr_t p, uint32_t v) { *(volatile uint32_t *)p = v; }
static void w64(uintptr_t p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

extern void fault_entry(void), probe_read(uintptr_t), probe_write(uintptr_t);
extern unsigned char probe_read_resume[], probe_write_resume[];
extern unsigned char __start_native_handlers[], __stop_native_handlers[];

__attribute__((used, noinline, section("native_handlers")))
static void fault_body(struct pacha_native_fault_frame *fault)
{
    CHECK(fault->signal.magic == PACHA_PROCESS_SIGNAL_FRAME_MAGIC);
    CHECK(fault->vector == 14 && fault->address == fault_expected && fault_resume != 0);
    CHECK(fault->error_code & 4); /* User access, never a kernel-side mapping probe. */
    fault->signal.rip = fault_resume;
    fault_seen++;
}

__asm__(
#ifndef NATIVE_DEVICE_EXTERNAL_RUNTIME
    ".text\n.global _start\n"
    "_start: and $-16,%rsp; call main; mov %eax,%edi; mov $" STR(PACHA_PROCESS_SYSCALL_EXIT) ",%eax; syscall; ud2\n"
#else
    ".text\n"
#endif
    ".global probe_read,probe_write,probe_read_resume,probe_write_resume\n"
    "probe_read: mov (%rdi),%al\nprobe_read_resume: ret\n"
    "probe_write: movb $0,(%rdi)\nprobe_write_resume: ret\n"
    ".pushsection native_handlers,\"ax\",@progbits\n.global fault_entry\n"
    "fault_entry: mov %rdi,%r12; call fault_body; mov %r12,%rsi; mov $3,%edi; mov $"
        STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall; ud2\n.popsection\n");

static void expect_fault(uintptr_t address, int write)
{
    fault_expected = address;
    fault_resume = (uintptr_t)(write ? probe_write_resume : probe_read_resume);
    const uint64_t before = fault_seen;
    if (write) probe_write(address); else probe_read(address);
    CHECK(fault_seen == before + 1);
    fault_resume = 0;
}

struct virtio_cap { unsigned bar; uint32_t offset, length; };
static struct virtio_cap common_cap, notify_cap;
static unsigned msix_cap;
static uint32_t notify_multiplier;
static uintptr_t common, notify, table, pba;
static uint32_t vector_base;

static void discover(void)
{
    stage = "pci-config";
    const struct pacha_capsule_info device = query(DEVICE_FD);
    CHECK(device.kind == PACHA_CAPSULE_KIND_DEVICE);
    CHECK(device.flags & PACHA_CAPSULE_DMA_TRANSLATED);
    CHECK(!(device.flags & PACHA_CAPSULE_DMA_QUARANTINED));
    vector_base = (uint32_t)device.index;
    CHECK(vector_base >= 0x50 && vector_base < 0xd0);
    const uint32_t identity = config(0, 4);
    CHECK((identity & 0xffff) == 0x1af4 && (identity >> 16) == 0x1044);
    CHECK(config(0, 2) == (identity & 0xffff));
    CHECK(config(1, 1) == ((identity >> 8) & 255));
    const uint16_t command = (uint16_t)config(4, 2);
    const uint16_t status = (uint16_t)config(6, 2);
    set_config(5, 1, ((command >> 8) ^ 4) & 255);
    CHECK(config(4, 2) == (uint16_t)(command ^ 0x400));
    CHECK((config(6, 2) & 0xf900) == (status & 0xf900));
    set_config(4, 2, (command | 2) & ~4u); /* Decode MMIO, DMA stays disabled until mapped. */
    CHECK((config(6, 2) & 0xf900) == (status & 0xf900));
    (void)config(0x100, 4);
    (void)config(4092, 4);
    uint32_t temporary = 0;
    CHECK(call(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ, DEVICE_FD, 0,
        (uintptr_t)&temporary, 3, 0, 0) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(call(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ, DEVICE_FD, 4096,
        (uintptr_t)&temporary, 1, 0, 0) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(call(PACHA_CAPSULE_SYSCALL_PCI_CONFIG_READ, DEVICE_FD, 255,
        (uintptr_t)&temporary, 2, 0, 0) == PACHA_SYSCALL_ERR_INVALID);

    uint64_t seen = 0;
    unsigned cap = config(0x34, 1) & 0xfc;
    while (cap) {
        CHECK(cap >= 0x40 && cap <= 0xf8 && !(seen & (UINT64_C(1) << (cap / 4))));
        seen |= UINT64_C(1) << (cap / 4);
        const uint32_t header = config(cap, 4);
        if ((header & 255) == 0x11) msix_cap = cap;
        if ((header & 255) == 9) {
            const unsigned type = header >> 24;
            if (type == 1 || type == 2) {
                CHECK(((header >> 16) & 255) >= (type == 2 ? 20u : 16u));
                struct virtio_cap *target = type == 1 ? &common_cap : &notify_cap;
                target->bar = config(cap + 4, 1);
                target->offset = config(cap + 8, 4);
                target->length = config(cap + 12, 4);
                if (type == 2) notify_multiplier = config(cap + 16, 4);
            }
        }
        cap = (header >> 8) & 0xfc;
    }
    CHECK(common_cap.length >= 56 && notify_cap.length >= 2 && msix_cap);
    common = map_register(common_cap.bar, common_cap.offset, 56);
    const uint32_t table_info = config(msix_cap + 4, 4);
    const uint32_t pba_info = config(msix_cap + 8, 4);
    table = map_register(table_info & 7, table_info & ~7u, 16);
    pba = map_register(pba_info & 7, pba_info & ~7u, 8);
    log_text("NATIVE_PCI_CONFIG=PASS\n");
}

static void mapping_checks(void)
{
    stage = "bar-protection";
    const struct pacha_capsule_bar_info info = bar_info(common_cap.bar);
    const uint64_t offset = ((info.start & (PAGE - 1)) + common_cap.offset) & ~(uint64_t)(PAGE - 1);
    const uint64_t address = MAP_BASE + 0x100000;
    const uint64_t readonly = map_bar(common_cap.bar, offset, address, PACHA_CAPSULE_MMIO_READ_ONLY);
    CHECK(is_fd(readonly));
    CHECK(!(query(readonly).rights & PACHA_FD_RIGHT_MMIO_MAP_WRITE));
    const uintptr_t alias = address + (((info.start & (PAGE - 1)) + common_cap.offset) & (PAGE - 1));
    CHECK(r8(alias + 20) == r8(common + 20));
    expect_fault(alias + 20, 1);
    CHECK(!is_fd(map_bar(common_cap.bar, offset, address + PAGE, PACHA_CAPSULE_MMIO_CACHE_WC)));
    expect_fault(address + PAGE, 0);
    const uint64_t outside = ((info.start & (PAGE - 1)) + info.size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    CHECK(!is_fd(map_bar(common_cap.bar, outside, address + PAGE, 0)));
    close_fd(readonly);
    expect_fault(alias + 20, 0);
    const uint64_t again = map_bar(common_cap.bar, offset, address, PACHA_CAPSULE_MMIO_READ_ONLY);
    CHECK(is_fd(again) && r8(alias + 20) == r8(common + 20));
    close_fd(again);
    log_text("NATIVE_BAR_MAPPING=PASS\n");
}

static void reset_device(void)
{
    w8(common + 20, 0);
    const uint64_t until = now() + UINT64_C(2000000000);
    while (r8(common + 20)) CHECK(now() < until);
    /* Virtio reset acknowledges queue teardown before its RAM is unpinned. */
    set_config(4, 2, config(4, 2) & ~4u);
    CHECK(!(config(4, 2) & 4));
    barrier();
}

static uint64_t derive_irq(void)
{
    return call(PACHA_CAPSULE_SYSCALL_DERIVE_IRQ, DEVICE_FD,
        PACHA_CAPSULE_IRQ_MSIX, 0, 0, 0, 0);
}

static uint64_t irq_count(uint64_t irq)
{
    uint64_t count = UINT64_MAX;
    CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_POLL, irq, UINT64_MAX,
        (uintptr_t)&count, 1, 0, 0) == 1);
    return count;
}

static void setup_queue(void)
{
    w8(common + 20, 1 | 2);
    w32(common, 1);
    const uint32_t features = r32(common + 4);
    CHECK((features & 3) == 3); /* VERSION_1 and ACCESS_PLATFORM, no bypass DMA. */
    w32(common + 8, 0); w32(common + 12, 0);
    w32(common + 8, 1); w32(common + 12, 3);
    w8(common + 20, 1 | 2 | 8);
    CHECK(r8(common + 20) == (1 | 2 | 8));
    w16(common + 22, 0);
    CHECK(r16(common + 24) >= 8);
    w16(common + 24, 8);
    CHECK(r16(common + 24) == 8);
    w16(common + 16, 0xffff);
    w16(common + 26, 0);
    CHECK(r16(common + 26) == 0);
    w64(common + 32, TEST_IOVA);
    w64(common + 40, TEST_IOVA + PAGE);
    w64(common + 48, TEST_IOVA + 2 * PAGE);
    const uint64_t delta = (uint64_t)r16(common + 30) * notify_multiplier;
    CHECK(delta <= notify_cap.length - 2);
    notify = map_register(notify_cap.bar, notify_cap.offset + delta, 2);
    w16(common + 28, 1);
    CHECK(r16(common + 28) == 1);
    w8(common + 20, 1 | 2 | 8 | 4);
    CHECK(r8(common + 20) == 15);
    set_config(4, 2, config(4, 2) | 6);
}

static void request_rng(unsigned index)
{
    for (unsigned i = 0; i < 64; i++) dma_bytes[3 * PAGE + i] = 0xa5;
    *(uint64_t *)&dma_bytes[0] = TEST_IOVA + 3 * PAGE;
    *(uint32_t *)&dma_bytes[8] = 64;
    *(uint16_t *)&dma_bytes[12] = 2; /* One device-writable descriptor. */
    *(uint16_t *)&dma_bytes[14] = 0;
    *(uint16_t *)&dma_bytes[PAGE + 4 + 2 * ((index - 1) & 7)] = 0;
    barrier();
    w16((uintptr_t)dma_bytes + PAGE + 2, (uint16_t)index);
    barrier();
    w16(notify, 0);
    const uint64_t until = now() + UINT64_C(2000000000);
    while (r16((uintptr_t)dma_bytes + 2 * PAGE + 2) != index) CHECK(now() < until);
    barrier();
    const uintptr_t used = (uintptr_t)dma_bytes + 2 * PAGE + 4 + 8 * ((index - 1) & 7);
    CHECK(r32(used) == 0 && r32(used + 4) > 0 && r32(used + 4) <= 64);
    unsigned changed = 0;
    for (unsigned i = 0; i < r32(used + 4); i++) changed |= dma_bytes[3 * PAGE + i] ^ 0xa5;
    CHECK(changed); /* Actual RNG writes and used completion, not mapping-only proof. */
}

int main(void)
{
    CHECK(call(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)fault_entry, (uintptr_t)__start_native_handlers,
        (uintptr_t)__stop_native_handlers, 0, 0) == 0);
    CHECK(call(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT,
        (uintptr_t)fault_entry, (uintptr_t)fault_stack, sizeof(fault_stack), 0, 0) == 0);
    discover();
    mapping_checks();
    reset_device();
    for (unsigned round = 0; round < 2; round++) {
        stage = round ? "dma-iova-reuse" : "dma-setup";
        memset(dma_bytes, 0, sizeof(dma_bytes));
        const uint64_t dma = call(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING,
            DEVICE_FD, (uintptr_t)dma_bytes, TEST_IOVA, DMA_SIZE,
            PACHA_CAPSULE_DMA_BIDIRECTIONAL, 0);
        CHECK(is_fd(dma));
        const struct pacha_capsule_info mapping = query(dma);
        CHECK(mapping.iova == TEST_IOVA && mapping.iova != (uintptr_t)dma_bytes);
        CHECK(mapping.size == DMA_SIZE && (mapping.flags & PACHA_CAPSULE_DMA_TRANSLATED));
        CHECK(!is_fd(call(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING,
            DEVICE_FD, (uintptr_t)dma_bytes, TEST_IOVA, DMA_SIZE,
            PACHA_CAPSULE_DMA_BIDIRECTIONAL, 0)));
        w32(table + 12, 1);
        w32(table, 0xfee00000); w32(table + 4, 0); w32(table + 8, vector_base);
        set_config(msix_cap + 2, 2, (config(msix_cap + 2, 2) | 0x8000) & ~0x4000u);
        CHECK(!(r32(pba) & 1));
        const uint64_t irq = derive_irq();
        CHECK(is_fd(irq));
        CHECK(irq_count(irq) == 0);
        setup_queue();
        stage = round ? "irq-rederive-completion" : "dma-msix-completion";
        w32(table + 12, 0);
        CHECK(!(r32(table + 12) & 1));
        request_rng(1);
        const uint64_t until = now() + UINT64_C(2000000000);
        while (irq_count(irq) == 0) CHECK(now() < until);
        CHECK(!(r32(pba) & 1));
        if (!round) {
            stage = "masked-pending-close";
            w32(table + 12, 1); CHECK(r32(table + 12) & 1);
            const uint64_t before = irq_count(irq);
            request_rng(2);
            const uint64_t pending_until = now() + UINT64_C(2000000000);
            while (!(r32(pba) & 1)) CHECK(now() < pending_until);
            CHECK(irq_count(irq) == before);
            close_fd(irq);
            CHECK(derive_irq() == PACHA_SYSCALL_ERR_NOT_READY);
            reset_device();
            const uint64_t reset_until = now() + UINT64_C(2000000000);
            while (r32(pba) & 1) CHECK(now() < reset_until);
        } else {
            reset_device();
            close_fd(irq);
            CHECK(r32(table + 12) & 1);
        }
        close_fd(dma); /* Only after real device reset and bus-master readback. */
        CHECK(!(query(DEVICE_FD).flags & PACHA_CAPSULE_DMA_QUARANTINED));
    }
    log_text("NATIVE_DMA_IOVA_REUSE=PASS\n");
    log_text("NATIVE_MSIX_REDERIVE=PASS\n");
    stage = "mmio-close";
    for (unsigned i = 0; i < map_count; i++) {
        close_fd(maps[i].fd);
        expect_fault(maps[i].address, 0);
    }
    CHECK(call(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT,
        0, 0, 0, 0, 0) == 0);
    close_fd(DEVICE_FD);
    log_text("NATIVE_DEVICE_CONTRACT=PASS\n");
    return 0;
}
