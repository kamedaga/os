#include <assert.h>
#include <stdio.h>
#include "filed/vmo_create.h"

static struct pacha_fd_table_info current;
static unsigned creates, queries, grows;
static int first_result, retry_result, query_result, grow_result;
static uint64_t growth_target;
static int grow_without_slots;

int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    assert(size == 12288 && rights == 0x123 && flags == 7);
    assert(creates < 2);
    return creates++ == 0 ? first_result : retry_result;
}

int pacha_fd_table(uint64_t minimum, struct pacha_fd_table_info *out)
{
    if (!minimum) {
        ++queries;
        *out = current;
        return query_result;
    }
    ++grows;
    growth_target = minimum;
    assert(minimum > current.capacity && minimum <= current.maximum);
    if (!grow_result && !grow_without_slots) {
        current.free_slots += minimum - current.capacity;
        current.capacity = minimum;
    }
    *out = current;
    return grow_result;
}

static void reset(void)
{
    current = (struct pacha_fd_table_info){512, 4096, 0};
    creates = queries = grows = 0;
    first_result = PACHA_ERR_ALLOC;
    retry_result = 512;
    query_result = grow_result = grow_without_slots = 0;
    growth_target = 0;
}

static int create(void) { return filed_vmo_create(12288, 0x123, 7); }

int main(void)
{
    reset(); first_result = 23;
    assert(create() == 23 && creates == 1 && queries == 0 && grows == 0);
    reset(); first_result = PACHA_ERR_INVALID;
    assert(create() == PACHA_ERR_INVALID && queries == 0);
    reset();
    assert(create() == 512 && creates == 2 && queries == 1 && grows == 1);
    assert(growth_target == 1024);
    reset(); current.free_slots = 17;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 0);
    reset(); current.capacity = current.maximum;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 0);
    reset(); current.capacity = 3000;
    assert(create() == 512 && growth_target == 4096);
    reset(); query_result = PACHA_ERR_INVALID;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 0);
    reset(); grow_result = PACHA_ERR_ALLOC;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 1);
    reset(); grow_without_slots = 1;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 1);
    reset(); retry_result = PACHA_ERR_ALLOC;
    assert(create() == PACHA_ERR_ALLOC && creates == 2 && grows == 1);
    reset(); retry_result = PACHA_ERR_INVALID;
    assert(create() == PACHA_ERR_INVALID && creates == 2 && grows == 1);
    reset(); current.capacity = 0;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 0);
    reset(); current.maximum = UINT64_MAX;
    assert(create() == PACHA_ERR_ALLOC && creates == 1 && grows == 0);
    puts("filed VMO FD growth: PASS");
}
