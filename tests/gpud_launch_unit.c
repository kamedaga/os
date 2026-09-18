/* SPDX-License-Identifier: MIT */
#include "../userland/gpud/launch.h"
#include "../userland/gpud/process.h"
#include "gpud_controller_fixture.h"
#include <pacha/syscall.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[12288], config[37];
static unsigned int calls, fail_at, process_live, process_owned, thread_owned;
static unsigned int view_owned, started, mapped;
static unsigned char *scratch;
static size_t scratch_size;
static long fault_value;
static size_t segment_head;
static uint64_t source_kind;

long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)nr; (void)a; (void)b; (void)c; (void)d;
    abort(); /* Launch tests do not use blocking IPC receive. */
}

static void write16(unsigned char *at, uint16_t value) {
    at[0] = value; at[1] = value >> 8;
}

static void write32(unsigned char *at, uint32_t value) {
    write16(at, value); write16(at + 2, value >> 16);
}

static void write64(unsigned char *at, uint64_t value) {
    write32(at, value); write32(at + 4, value >> 32);
}

static void fixture_image(void) {
    segment_head = 0;
    memset(image, 0, sizeof(image));
    write32(image, UINT32_C(0x464c457f));
    image[4] = 2; image[5] = 1; image[6] = 1;
    write16(image + 16, 2); write16(image + 18, 62); write32(image + 20, 1);
    write64(image + 24, 0x100000); write64(image + 32, 64);
    write16(image + 52, 64); write16(image + 54, 56); write16(image + 56, 2);
    for (unsigned int i = 0; i < 2; ++i) {
        unsigned char *header = image + 64 + i * 56;
        write32(header, 1); write32(header + 4, i ? 6 : 5);
        write64(header + 8, 4096 + i * 4096);
        write64(header + 16, 0x100000 + i * 8192);
        write64(header + 32, 16 + i * 16); write64(header + 40, 4096 + i * 4096);
        write64(header + 48, 4096);
        memset(image + 4096 + i * 4096, 0x41 + i, 16 + i * 16);
    }
    memset(config, 0x73, sizeof(config));
}

static struct gpud_launch_request request(void) {
    static const struct gpud_launch_blob blob = {.bytes = config, .size = sizeof(config),
        .address = 0x200000};
    static const struct pacha_process_fd_grant grant = {.source_fd = 120,
        .target_fd = 16, .rights = PH_IPC_CHANNEL_RIGHTS};
    return (struct gpud_launch_request){.generation = 1, .image = image,
        .image_size = sizeof(image), .stack_address = 0x300000, .stack_size = 65536,
        .blobs = &blob, .blob_count = 1, .grants = &grant, .grant_count = 1};
}

static int injected(void) {
    ++calls;
    return fail_at && calls == fail_at;
}

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    if (injected()) return fault_value;
    if (nr == PACHA_THREAD_SYSCALL_START) {
        assert(fd == 18 && thread_owned && process_live && !started);
        assert(!scratch && !view_owned && mapped == 4);
        started = 1;
        return 0;
    }
    assert(nr == PACHA_FD_SYSCALL_CLOSE);
    if (fd == 19) {
        assert(scratch && !view_owned);
        free(scratch); scratch = NULL;
    } else if (fd == 18) {
        assert(thread_owned && !process_live);
        thread_owned = 0;
    } else {
        assert(fd == 17 && process_owned && !process_live);
        process_owned = 0;
    }
    return 0;
}

long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) {
    if (injected()) return fault_value;
    if (nr == PACHA_FD_SYSCALL_GET_INFO) {
        assert(a == 120);
        *(struct pacha_fd_info *)(uintptr_t)b = (struct pacha_fd_info){.kind = source_kind,
            .rights = PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_TRANSFER};
        return 0;
    }
    if (nr == PACHA_VM_SYSCALL_MUNMAP) {
        assert(view_owned && a == (uintptr_t)scratch && b == scratch_size);
        view_owned = 0;
        return 0;
    }
    assert(a == 17 && process_owned);
    if (nr == PACHA_PROCESS_SYSCALL_KILL) {
        assert(b == 9);
        process_live = 0;
        return 0;
    }
    assert(nr == PACHA_PROCESS_SYSCALL_WAIT);
    if (process_live) return PACHA_SYSCALL_ERR_NOT_READY;
    *(struct gpud_process_exit *)(uintptr_t)b = (struct gpud_process_exit){
        .state = GPUD_PROCESS_KILLED, .code = 9, .principal = 17};
    return 0;
}

long pacha_syscall3(uint64_t nr, uint64_t size, uint64_t rights, uint64_t flags) {
    if (injected()) return fault_value;
    assert(nr == PACHA_FD_SYSCALL_VMO_CREATE && !scratch && !flags);
    assert(rights & PACHA_FD_RIGHT_MAP_WRITE);
    scratch = malloc(size);
    assert(scratch);
    scratch_size = size;
    /* Do not let implicit host zero initialization hide a missing BSS clear. */
    memset(scratch, 0xa5, size);
    return 19;
}

long pacha_syscall5(uint64_t nr, uint64_t flags, uint64_t rights, uint64_t fd_flags,
    uint64_t grants, uint64_t count) {
    if (injected()) return fault_value;
    assert(nr == PACHA_PROCESS_SYSCALL_CREATE && !flags && !fd_flags && count == 1);
    assert(rights & PACHA_FD_RIGHT_SET_CONTEXT);
    const struct pacha_process_fd_grant *grant = (const void *)(uintptr_t)grants;
    assert(grant->source_fd == 120 && grant->target_fd == 16 && grant->rights == PH_IPC_CHANNEL_RIGHTS);
    assert(!process_owned);
    process_live = process_owned = 1;
    return 17;
}

long pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f) {
    if (injected()) return fault_value;
    if (nr == PACHA_VM_SYSCALL_MMAP) {
        assert(a == 19 && !b && scratch && c == scratch_size && !view_owned);
        assert(d == (PACHA_PROT_READ | PACHA_PROT_WRITE) && e == PACHA_MMAP_SHARED && !f);
        view_owned = 1;
        return (long)(uintptr_t)scratch;
    }
    assert(a == 17 && process_owned);
    if (nr == PACHA_THREAD_SYSCALL_CREATE) {
        assert(b == 0x100000 && c == 0x310000 && !d && !e);
        assert(f == (PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_CLOSE));
        assert(mapped == 4 && !scratch && !view_owned);
        thread_owned = 1;
        return 18;
    }
    assert(nr == PACHA_PROCESS_SYSCALL_MAP && !started && mapped < 4);
    const uint64_t addresses[] = {0x100000, 0x102000, 0x200000, 0x300000};
    const size_t sizes[] = {4096, 8192, 4096, 65536};
    const uint64_t protections[] = {PACHA_PROT_READ | PACHA_PROT_EXEC,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_PROT_READ, PACHA_PROT_READ | PACHA_PROT_WRITE};
    assert(c == addresses[mapped] && d == sizes[mapped] && e == protections[mapped]);
    if (mapped == 3) {
        assert(!b && f == (PACHA_PROCESS_MAP_PRIVATE | PACHA_PROCESS_MAP_ANONYMOUS));
    } else {
        assert(b == 19 && view_owned && f == PACHA_PROCESS_MAP_SHARED);
        size_t copied = mapped == 2 ? sizeof(config) : 16 + mapped * 16;
        size_t head = mapped == 1 ? segment_head : 0;
        int fill = mapped == 2 ? 0x73 : 0x41 + (int)mapped;
        for (size_t i = 0; i < scratch_size; ++i)
            assert(scratch[i] == (i >= head && i - head < copied ? fill : 0));
    }
    ++mapped;
    return (long)c;
}

static void reset_native(void) {
    assert(!process_owned && !thread_owned && !scratch && !view_owned);
    calls = fail_at = started = mapped = process_live = 0;
    fault_value = PACHA_SYSCALL_ERR_ALLOC;
    source_kind = PACHA_FD_KIND_CHANNEL;
}

static void test_prepare_failures(void) {
    fixture_image(); reset_native();
    struct gpud_launch_request input = request();
    struct gpud_native_launch launch = {0};
    assert(!gpud_native_launch_prepare(&launch, &input));
    const unsigned int preparation_calls = calls;
    assert(!gpud_native_launch_start(&launch, 1));
    assert(gpud_native_launch_start(&launch, 1) == -EINVAL);
    assert(gpud_native_launch_discard(&launch, 1) == -EBUSY);
    assert(!gpud_native_launch_abort(&launch, 1));
    assert(!gpud_native_launch_discard(&launch, 1));
    unsigned int before = calls;
    assert(gpud_native_launch_prepare(&launch, &input) == -ESTALE && calls == before);
    assert(gpud_native_launch_abort(&launch, 2) == -ESTALE && calls == before);
    for (unsigned int failure = 1; failure <= preparation_calls; ++failure) {
        reset_native();
        launch = (struct gpud_native_launch){0};
        fail_at = failure;
        assert(gpud_native_launch_prepare(&launch, &input) < 0);
        assert(launch.failed && !launch.prepared && !started);
        assert(!gpud_native_launch_abort(&launch, 1));
        assert(!process_owned && !thread_owned && !scratch && !view_owned);
    }
    /* A zero result cannot be accepted as a created FD or mapped address. */
    for (unsigned int failure = 1; failure <= 4; ++failure) {
        reset_native(); launch = (struct gpud_native_launch){0};
        fail_at = failure; fault_value = 0;
        assert(gpud_native_launch_prepare(&launch, &input) == -EPROTO);
        assert(!gpud_native_launch_abort(&launch, 1));
    }
    reset_native(); launch = (struct gpud_native_launch){0};
    assert(!gpud_native_launch_prepare(&launch, &input));
    fail_at = calls + 1;
    assert(gpud_native_launch_start(&launch, 1) == -ENOMEM && launch.failed && !started);
    assert(!gpud_native_launch_abort(&launch, 1));
    /* Abort KILL failure and close failure both retain the recorded owner. */
    reset_native(); launch = (struct gpud_native_launch){0};
    assert(!gpud_native_launch_prepare(&launch, &input));
    fail_at = calls + 2;
    assert(gpud_native_launch_abort(&launch, 1) == -ENOMEM && process_owned && process_live);
    fail_at = 0;
    assert(!gpud_native_process_terminate(&launch.process, 1, 9));
    fail_at = calls + 1;
    assert(gpud_native_launch_discard(&launch, 1) == -ENOMEM && launch.thread_fd == 18);
    assert(!gpud_native_launch_discard(&launch, 1));
    /* Exercise the loader's leading-page copy offset, not just plan decoding. */
    fixture_image(); reset_native(); launch = (struct gpud_native_launch){0};
    write64(image + 64 + 56 + 16, 0x102010);
    write64(image + 64 + 56 + 8, 8208);
    write64(image + 64 + 56 + 40, 8176);
    memmove(image + 8208, image + 8192, 32);
    segment_head = 16;
    assert(!gpud_native_launch_prepare(&launch, &input));
    assert(!gpud_native_launch_abort(&launch, 1));
}

static void expect_invalid_image(void) {
    struct gpud_launch_image plan, before;
    memset(&plan, 0xa5, sizeof(plan)); before = plan;
    assert(gpud_launch_image_plan(image, sizeof(image), 0, &plan) < 0);
    assert(!memcmp(&plan, &before, sizeof(plan)));
}

static void test_image_validation(void) {
    fixture_image();
    struct gpud_launch_image plan;
    assert(!gpud_launch_image_plan(image, sizeof(image), 0, &plan));
    assert(plan.segment_count == 2 && plan.entry == 0x100000);
    assert(plan.segments[1].mapping_size == 8192 && plan.segments[1].file_size == 32);
    /* lld's static PIE segments often begin partway into disjoint pages. */
    write64(image + 64 + 56 + 16, 0x102010);
    write64(image + 64 + 56 + 8, 8208);
    write64(image + 64 + 56 + 40, 8176);
    assert(!gpud_launch_image_plan(image, sizeof(image), 0, &plan));
    assert(plan.segments[1].data_offset == 16 && plan.segments[1].mapping_size == 8192);
    fixture_image();
    for (size_t length = 0; length < 176; ++length)
        assert(gpud_launch_image_plan(image, length, 0, &plan) < 0);
    const size_t corrupt[] = {0, 4, 5, 6, 16, 18, 20, 52, 54};
    for (size_t i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        fixture_image(); image[corrupt[i]] = 0xff; expect_invalid_image();
    }
    fixture_image(); write64(image + 32, UINT64_MAX); expect_invalid_image();
    fixture_image(); write16(image + 56, 65); expect_invalid_image();
    fixture_image(); write64(image + 24, 0x102000); expect_invalid_image();
    fixture_image(); write32(image + 64 + 4, 7); expect_invalid_image();
    fixture_image(); write32(image + 64, 3); expect_invalid_image();
    fixture_image(); write32(image + 64, 7); expect_invalid_image();
    fixture_image(); write64(image + 64 + 48, 3); expect_invalid_image();
    fixture_image(); write64(image + 64 + 32, 8192); expect_invalid_image();
    fixture_image(); write64(image + 64 + 40, UINT64_MAX); expect_invalid_image();
    fixture_image(); write64(image + 64 + 16, 0x100001); expect_invalid_image();
    fixture_image(); write64(image + 64 + 56 + 16, 0x100000); expect_invalid_image();
    fixture_image(); write16(image + 16, 3);
    assert(!gpud_launch_image_plan(image, sizeof(image), 0x400000, &plan));
    assert(plan.entry == 0x500000 && plan.segments[1].address == 0x502000);
    assert(gpud_launch_image_plan(image, sizeof(image), UINT64_MAX - 4095, &plan) < 0);
    fixture_image(); reset_native();
    struct gpud_launch_request input = request();
    struct gpud_native_launch launch = {0};
    input.stack_address = 0x100000;
    assert(gpud_native_launch_prepare(&launch, &input) == -EINVAL && !calls);
    input = request();
    struct gpud_launch_blob blobs[2] = {*input.blobs, *input.blobs};
    input.blobs = blobs; input.blob_count = 2;
    assert(gpud_native_launch_prepare(&launch, &input) == -EINVAL && !calls);
    blobs[1].size = SIZE_MAX;
    assert(gpud_native_launch_prepare(&launch, &input) == -EINVAL && !calls);
}

static void *controller_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void controller_free(void *context, void *pointer, size_t size) {
    (void)context; (void)size;
    free(pointer);
}

static void test_controller_binding(void) {
    unsigned int begin_calls = 0;
    for (unsigned int mode = 0; mode < 5; ++mode) {
        fixture_image(); reset_native();
        kb2_controller_t *controller = gpud_test_controller_create(controller_allocate, controller_free, NULL);
        assert(kb2_controller_start(controller) == KB2_STATUS_OK);
        gpud_test_complete(controller, KB2_ACTION_ALLOCATE_RESOURCES, 51, 0);
        struct gpud_launch_request input = request();
        struct gpud_native_launch launch = {0};
        struct ph_ipc ipc = {.generation = 1, .fd = 16, .admitted = 1};
        struct gpud_launch_resources resources = {.generation = 1, .resource_set_id = 51,
            .sandbox_id = 71, .request = &input};
        struct gpud_launch_transaction transaction = {0};
        /* Binding denials cannot publish a transaction or launch a process. */
        assert(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) ==
            KB2_STATUS_RESOURCE_DENIED && !transaction.controller && !calls);
        memset(resources.manifest_digest, 0x41, sizeof(resources.manifest_digest));
        resources.resource_set_id = 52;
        assert(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) ==
            KB2_STATUS_RESOURCE_DENIED && !calls);
        resources.resource_set_id = 51; resources.generation = 2;
        assert(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) ==
            KB2_STATUS_STALE_GENERATION && !calls);
        resources.generation = 1;
        struct pacha_process_fd_grant grant = *input.grants;
        const struct pacha_process_fd_grant *original = input.grants;
        input.grants = &grant; grant.rights |= PACHA_FD_RIGHT_BUS_MASTER;
        assert(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) ==
            KB2_STATUS_RESOURCE_DENIED && !calls);
        input.grants = original;
        source_kind = PACHA_FD_KIND_VMO;
        assert(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) ==
            KB2_STATUS_RESOURCE_DENIED && !transaction.controller && !process_owned);
        reset_native();
        if (mode == 1) input.image_size = 1;
        if (mode == 2 || mode == 4) fail_at = begin_calls;
        assert(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) == KB2_STATUS_OK);
        assert(kb2_action_type(kb2_controller_pending_action(controller)) == KB2_ACTION_LAUNCH_SANDBOX);
        if (!mode) begin_calls = calls;
        if (mode == 2 || mode == 4) assert(transaction.launch_error == -ENOMEM && !started);
        unsigned int before = calls;
        ++transaction.action_token;
        assert(gpud_launch_step(&transaction) == KB2_STATUS_STALE_ACTION && calls == before);
        --transaction.action_token;
        ipc.generation = 2;
        assert(gpud_launch_step(&transaction) == KB2_STATUS_STALE_GENERATION && calls == before);
        ipc.generation = 1;
        if (mode == 3) process_live = 0; /* Child exits before LAUNCH publication. */
        if (mode == 4) {
            fail_at = calls + 2; /* Failed rollback KILL must not complete LAUNCH. */
            assert(gpud_launch_step(&transaction) == KB2_STATUS_HOST_FAILURE);
            assert(!transaction.completed && process_owned && process_live && !ipc.admitted);
            assert(kb2_action_type(kb2_controller_pending_action(controller)) == KB2_ACTION_LAUNCH_SANDBOX);
            fail_at = 0;
        }
        assert(gpud_launch_step(&transaction) == (mode ? KB2_STATUS_HOST_FAILURE : KB2_STATUS_OK));
        assert(transaction.completed);
        before = calls;
        assert(gpud_launch_step(&transaction) == KB2_STATUS_INVALID_STATE && calls == before);
        if (!mode) {
            assert(kb2_controller_state(controller) == KB2_STATE_STARTING && ipc.admitted);
            assert(kb2_action_type(kb2_controller_pending_action(controller)) == KB2_ACTION_TRANSFER_RESOURCES);
            struct gpud_process_watch watch = {0};
            assert(gpud_process_watch_init(&watch, controller, &launch.process, &ipc, 51, 71) == KB2_STATUS_OK);
            const kb2_action_t *action = kb2_controller_pending_action(controller);
            assert(kb2_controller_complete_action(controller, 1, kb2_action_token(action),
                KB2_STATUS_HOST_FAILURE, 0, 0) == KB2_STATUS_HOST_FAILURE);
            assert(kb2_controller_stop(controller) == KB2_STATUS_OK);
            action = kb2_controller_pending_action(controller);
            assert(gpud_process_action(&watch, 1, kb2_action_token(action), 9) == KB2_STATUS_OK);
            gpud_test_complete(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0);
            action = kb2_controller_pending_action(controller);
            assert(gpud_process_action(&watch, 1, kb2_action_token(action), 9) == KB2_STATUS_OK);
            assert(!gpud_native_launch_discard(&launch, 1));
        } else {
            assert(kb2_controller_state(controller) == KB2_STATE_FAULTED && !ipc.admitted);
            assert(!process_owned && !thread_owned && !scratch && !view_owned);
            assert(kb2_controller_stop(controller) == KB2_STATUS_OK);
            gpud_test_complete(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0);
        }
        gpud_test_complete(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0);
        assert(kb2_controller_state(controller) == KB2_STATE_IDLE);
        kb2_controller_destroy(controller);
    }
}

int main(int argc, char **argv) {
    /* Optional real native images exercise the same parser, not a new oracle. */
    for (int i = 1; i < argc; ++i) {
        FILE *file = fopen(argv[i], "rb");
        assert(file && !fseek(file, 0, SEEK_END));
        long size = ftell(file);
        assert(size >= 64 && size <= GPUD_LAUNCH_IMAGE_LIMIT && !fseek(file, 0, SEEK_SET));
        unsigned char *bytes = malloc((size_t)size);
        assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
        assert(!fclose(file));
        struct gpud_launch_image plan;
        uint64_t bias = bytes[16] == 3 ? UINT64_C(0x40000000) : 0;
        assert(!gpud_launch_image_plan(bytes, (size_t)size, bias, &plan));
        printf("Native image plan: %s (%zu segments)\n", argv[i], plan.segment_count);
        free(bytes);
    }
    test_image_validation();
    test_prepare_failures();
    test_controller_binding();
    puts("gpud native ELF/launch ownership unit: PASS (not a core boot Gate)");
    return 0;
}
