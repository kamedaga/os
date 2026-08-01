#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../userland/drmd/src/drm_flip_completion.h"

enum { TEST_FENCE_BYTES = 0x78 };

static void set_fence_refs(unsigned char *fence, uint32_t refs)
{
    memcpy(fence + DRMD_DMA_FENCE_REFCOUNT_OFFSET, &refs, sizeof(refs));
}

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "DRMD_FLIP_COMPLETION_FAIL %s\n", message);
    return 1;
}

int main(void)
{
    unsigned char fence[TEST_FENCE_BYTES] = {0};
    drmd_flip_completion_t pending = {0};
    drmd_flip_completion_t completed = {0};
    int failures = 0;

    set_fence_refs(fence, 1);
    failures += expect(
        drmd_flip_completion_begin(
            &pending, 7, UINT64_C(0x1122334455667788), 71, fence) == -5,
        "unemitted fence accepted");
    failures += expect(!pending.active, "failed submit became pending");

    set_fence_refs(fence, 2);
    failures += expect(
        drmd_flip_completion_begin(
            &pending, 7, UINT64_C(0x1122334455667788), 71, fence) == 0,
        "emitted fence rejected");
    failures += expect(
        drmd_flip_completion_take(&pending, &completed) == 0,
        "event completed before fenced response");
    failures += expect(pending.active, "pending state cleared before response");
    failures += expect(
        drmd_flip_completion_begin(&pending, 7, 9, 72, fence) == -16,
        "second flip accepted while one is pending");

    set_fence_refs(fence, 1);
    failures += expect(
        drmd_flip_completion_take(&pending, &completed) == 1,
        "exact fenced response did not complete flip");
    failures += expect(
        completed.owner == 7 &&
        completed.user_data == UINT64_C(0x1122334455667788) &&
        completed.fb_id == 71 && completed.fence == fence,
        "completed flip payload changed");
    failures += expect(!pending.active, "completed flip remained pending");
    failures += expect(
        drmd_flip_completion_take(&pending, &completed) == 0,
        "one fenced response completed twice");

    if (failures != 0) return 1;
    puts("DRMD_FLIP_COMPLETION_PASS");
    return 0;
}
