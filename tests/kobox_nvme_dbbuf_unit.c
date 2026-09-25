/* Exercise the native FileD completion path, not a patched Linux module. */
#ifdef KOBOX_NVME_QUEUE_TEST
#include "../_kobox/src/linux_personality/linux_nvme.c"
#else
#include "linux_subsystem/nvme/nvme.h"
#include <stdio.h>
#endif
#include <assert.h>

#ifdef KOBOX_NVME_QUEUE_TEST
static int request_object, completed;
void *kb_linux_block_request_tag_set(void *request)
{ assert(request == &request_object); return request; }
void *kb_block_subsystem_tagset_request(void *set, size_t queue, uint32_t tag)
{ assert(set == &request_object && queue == 0 && tag == 5); return set; }
uint8_t kb_linux_block_request_generation(const void *request)
{ assert(request == &request_object); return 7; }
void kb_linux_block_request_set_result_status(void *request, uint64_t value, uint16_t status)
{ assert(request == &request_object && value == 0x1234 && status == 0); }
void kb_linux_block_request_mark_complete(void *request, unsigned int status)
{ assert(request == &request_object && status == 0); ++completed; }

static void completion_case(int enabled, int notify, int wrap)
{
    _Alignas(8) unsigned char queue[160] = {0}, device[0x200] = {0}, cq[32] = {0};
    volatile uint32_t doorbells[4] = {0, 0, 0xbeef, 0};
    const uint16_t before = wrap ? 1 : 0;
    const uint16_t after = wrap ? 0 : 1;
    volatile uint32_t shadow = before, event_index = notify ? before : after;
    void *dev = device, *entries = cq;
    volatile uint32_t *sq_db = doorbells;
    volatile uint32_t *shadow_ptr = enabled ? &shadow : NULL;
    const volatile uint32_t *event_ptr = enabled ? &event_index : NULL;
    memcpy(queue + KB_NVME_QUEUE_DEV_OFFSET, &dev, sizeof(dev));
    memcpy(queue + KB_NVME_QUEUE_CQ_OFFSET, &entries, sizeof(entries));
    memcpy(queue + KB_NVME_QUEUE_SQ_DB_OFFSET, &sq_db, sizeof(sq_db));
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_DB_OFFSET, &shadow_ptr, sizeof(shadow_ptr));
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_EI_OFFSET, &event_ptr, sizeof(event_ptr));
    write_u32(device + KB_NVME_DEV_DB_STRIDE_OFFSET, 2);
    write_u16(queue + KB_NVME_QUEUE_QID_OFFSET, 1);
    write_u16(queue + KB_NVME_QUEUE_DEPTH_OFFSET, 2);
    write_u16(queue + KB_NVME_QUEUE_CQ_HEAD_OFFSET, before);
    queue[KB_NVME_QUEUE_PHASE_OFFSET] = 1;
    write_u64(cq + before * 16, 0x1234);
    write_u16(cq + before * 16 + 12, 0x7005);
    write_u16(cq + before * 16 + 14, 1);
    /* After wrapping, the already-consumed slot still carries the old phase. */
    if (wrap) write_u16(cq + 14, 1);
    completed = 0;
    assert(nvme_block_drain_io_completion(queue, &request_object) == 1);
    assert(completed == 1 && read_u16(queue + KB_NVME_QUEUE_CQ_HEAD_OFFSET) == after);
    assert(queue[KB_NVME_QUEUE_PHASE_OFFSET] == (wrap ? 0 : 1));
    assert(shadow == (enabled ? after : before));
    assert(doorbells[2] == (enabled && !notify ? 0xbeef : after));
    assert(doorbells[0] == 0 && doorbells[1] == 0 && doorbells[3] == 0);
    assert(nvme_block_drain_io_completion(queue, &request_object) == 0);
    assert(completed == 1);
}
#endif

static void check(uint16_t old, uint16_t event, uint16_t value, int notify)
{
    volatile uint32_t shadow = old, event_index = event;
    assert(kb_nvme_update_dbbuf(value, &shadow, &event_index) == notify);
    assert(shadow == value && event_index == event);
}

int main(void)
{
    check(0, 0, 1, 1);
    check(0, 2, 1, 0);
    check(7, 3, 8, 0);
    check(7, 7, 10, 1);
    check(7, 8, 10, 1);
    check(7, 9, 10, 1);
    check(7, 10, 10, 0);
    check(9, 9, 9, 0);
    check(65534, 65535, 1, 1);
    check(65534, 1, 1, 0);
    check(65535, 65535, 0, 1);
    check(63, 63, 0, 1);
    check(63, 0, 0, 0);

    volatile uint32_t shadow = 7, event_index = 20;
    assert(kb_nvme_update_dbbuf(8, NULL, &event_index) == 1);
    assert(kb_nvme_update_dbbuf(8, &shadow, NULL) == 1 && shadow == 8);
    assert(kb_nvme_update_dbbuf(8, NULL, NULL) == 1);

#ifdef KOBOX_NVME_QUEUE_TEST
    completion_case(1, 0, 0);
    completion_case(1, 1, 0);
    completion_case(1, 0, 1);
    completion_case(1, 1, 1);
    completion_case(0, 0, 0);
    completion_case(0, 0, 1);
    unsigned char queue[160] = {0};
    volatile uint32_t *db = &shadow;
    const volatile uint32_t *ei = &event_index;
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_DB_OFFSET, &db, sizeof(db));
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_EI_OFFSET, &ei, sizeof(ei));
    write_u16(queue + KB_NVME_QUEUE_QID_OFFSET, 1);
    assert(nvme_update_cq_dbbuf(queue, 9) == 0 && shadow == 9);
    event_index = 9;
    assert(nvme_update_cq_dbbuf(queue, 10) == 1 && shadow == 10);

    /* Failed DBBUF configuration clears queue-local pointers; the old
     * tracked DMA allocations must not suppress the MMIO fallback. */
    db = NULL;
    ei = NULL;
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_DB_OFFSET, &db, sizeof(db));
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_EI_OFFSET, &ei, sizeof(ei));
    assert(nvme_update_cq_dbbuf(queue, 11) == 1 && shadow == 10);

    /* Admin queues must not dereference DBBUF, even during reconfiguration. */
    db = (volatile uint32_t *)(uintptr_t)1;
    ei = (const volatile uint32_t *)(uintptr_t)1;
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_DB_OFFSET, &db, sizeof(db));
    memcpy(queue + KB_NVME_QUEUE_DBBUF_CQ_EI_OFFSET, &ei, sizeof(ei));
    write_u16(queue + KB_NVME_QUEUE_QID_OFFSET, 0);
    assert(nvme_update_cq_dbbuf(queue, 12) == 1);
    assert(nvme_update_cq_dbbuf(NULL, 12) == 1);
    puts("NVMe CQ DBBUF: production queue pointers and admin fallback PASS");
#endif
    puts("NVMe DBBUF: event crossings, wrap and missing-pointer fallback PASS");
    return 0;
}
