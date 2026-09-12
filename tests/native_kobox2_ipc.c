/* SPDX-License-Identifier: MIT */
#include "native_kobox2_ipc_common.h"
#include "../userland/gpud/launch_native.h"
#include "../userland/gpud/launch.h"
#include "../userland/gpud/process.h"
#define GPUD_TEST_CHECK(condition) IPC_CHECK(condition)
#include "gpud_controller_fixture.h"
#include <kobox2/closure_manifest.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>
#include "../userland/seed0boot/src/bootfs_reader.h"

struct ipc_test_child { struct gpud_native_launch launch; };

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return (unsigned char)*left - (unsigned char)*right;
}

static uint64_t new_vmo(size_t size, uint64_t rights) {
    long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, size, rights, 0);
    IPC_CHECK(fd >= 16 && fd < PACHA_FD_TABLE_LIMIT);
    return (uint64_t)fd;
}

static struct ipc_test_child start_child(struct ph_ipc *ipc, uint64_t generation,
    unsigned int mode, const void *image, size_t image_size, const struct ph_package_identity *identity) {
    uint64_t pair[2];
    IPC_CHECK(!pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uintptr_t)pair,
        PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE, 0));
    IPC_CHECK(!ph_ipc_init(ipc, (int)pair[0], generation));
    struct pacha_process_fd_grant grant = {.source_fd = pair[1],
        .target_fd = IPC_TEST_CHILD_FD, .rights = PH_IPC_CHANNEL_RIGHTS,
        .flags = PACHA_FD_FLAG_CLOEXEC};
    struct ipc_test_config config = {.generation = generation, .mode = mode};
    if (identity) config.identity = *identity;
    struct gpud_launch_blob blob = {.bytes = &config, .size = sizeof(config),
        .address = IPC_TEST_CONFIG};
    struct gpud_launch_request request = {.generation = generation,
        .image = image, .image_size = image_size, .stack_address = IPC_TEST_STACK,
        .stack_size = 16 * IPC_TEST_PAGE, .blobs = &blob, .blob_count = 1,
        .grants = &grant, .grant_count = 1};
    struct ipc_test_child child = {0};
    IPC_CHECK(!gpud_native_launch_prepare(&child.launch, &request));
    IPC_CHECK(gpud_native_launch_discard(&child.launch, generation) == -EBUSY);
    ipc_test_close(pair[1]); /* Only the actual child owns the peer now. */
    IPC_CHECK(!gpud_native_launch_start(&child.launch, generation));
    struct ph_ipc_packet ready = {0};
    IPC_CHECK(!ipc_test_receive(ipc, &ready));
    IPC_CHECK(ready.operation == IPC_TEST_READY && !ready.fd_count);
    IPC_CHECK(gpud_native_process_poll(&child.launch.process, generation - 1) == -ESTALE);
    return child;
}

static void reap_child(struct ph_ipc *ipc, struct ipc_test_child *child, int killed) {
    uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
    int result;
    while ((result = gpud_native_process_poll(&child->launch.process, ipc->generation)) == -EAGAIN)
        IPC_CHECK(ipc_test_now() < deadline);
    IPC_CHECK(!result && child->launch.process.exit.state ==
        (killed ? GPUD_PROCESS_KILLED : GPUD_PROCESS_EXITED));
    if (!killed) IPC_CHECK(!child->launch.process.exit.code);
    struct pacha_pollfd peer = {.fd = ipc->fd, .events = PACHA_FD_EVENT_HANGUP,
        .revents = UINT64_MAX};
    result = pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uintptr_t)&peer, 1);
    IPC_CHECK(result == 1 && peer.revents == PACHA_FD_EVENT_HANGUP);
    IPC_CHECK(!gpud_native_launch_discard(&child->launch, ipc->generation));
    IPC_CHECK(!gpud_native_launch_discard(&child->launch, ipc->generation));
    IPC_CHECK(!ph_ipc_destroy(ipc, ipc->generation));
}

static void shared_round(struct ph_ipc *ipc, uint64_t generation,
    const void *image, size_t image_size, int *previous_fd) {
    struct ipc_test_child child = start_child(ipc, generation, 0, image, image_size, NULL);
    if (*previous_fd) IPC_CHECK(ipc->fd == *previous_fd);
    *previous_fd = ipc->fd;
    struct ph_ipc_packet outgoing = {.operation = IPC_TEST_SHARE, .generation = generation,
        .correlation = 1, .value = generation * 100, .fd_count = PACHA_IPC_MAX_TRANSFER_FDS};
    uint64_t *mappings[PACHA_IPC_MAX_TRANSFER_FDS];
    for (size_t i = 0; i < outgoing.fd_count; ++i) {
        uint64_t fd = new_vmo(IPC_TEST_PAGE, IPC_TEST_VMO_RIGHTS);
        mappings[i] = ipc_test_map(fd, IPC_TEST_PAGE, IPC_TEST_RW);
        mappings[i][0] = outgoing.value + i;
        outgoing.fds[i] = (struct pacha_ipc_fd){.fd = fd,
            .rights = IPC_TEST_VMO_RIGHTS & (i ? UINT64_MAX : ~PACHA_FD_RIGHT_MAP_WRITE),
            .flags = PACHA_FD_FLAG_CLOEXEC};
    }
    /* Even after the numeric channel FD is reused, a previous generation's
     * local request must fail before touching the new peer or its FD table. */
    outgoing.generation = generation - 1;
    IPC_CHECK(ph_ipc_send(ipc, &outgoing) == -ESTALE);
    outgoing.generation = generation;
    ipc_test_send(ipc, &outgoing);
    struct ph_ipc_packet returned = {0};
    IPC_CHECK(!ipc_test_receive(ipc, &returned));
    IPC_CHECK(returned.operation == IPC_TEST_RETURN && returned.correlation == 1);
    IPC_CHECK(returned.fd_count == outgoing.fd_count);
    for (size_t i = 0; i < outgoing.fd_count; ++i) {
        IPC_CHECK(mappings[i][0] == ((outgoing.value + i) ^ (i ? IPC_TEST_MARK : 0)));
        IPC_CHECK(returned.fds[i].rights == outgoing.fds[i].rights);
        IPC_CHECK(returned.fds[i].fd != outgoing.fds[i].fd);
        uint64_t *alias = ipc_test_map(returned.fds[i].fd, IPC_TEST_PAGE, PACHA_PROT_READ);
        IPC_CHECK(alias[0] == mappings[i][0]);
        ipc_test_unmap(alias, IPC_TEST_PAGE);
        ipc_test_unmap(mappings[i], IPC_TEST_PAGE);
    }
    IPC_CHECK(!ph_ipc_packet_release(&returned));
    IPC_CHECK(!ph_ipc_packet_release(&outgoing));

    uint64_t moved_fd = new_vmo(IPC_TEST_PAGE, IPC_TEST_VMO_RIGHTS);
    uint64_t *shared = ipc_test_map(moved_fd, IPC_TEST_PAGE, IPC_TEST_RW);
    shared[0] = generation;
    outgoing = (struct ph_ipc_packet){.operation = IPC_TEST_MOVE, .generation = generation,
        .correlation = 2, .value = generation, .fd_count = 1,
        .fds = {{.fd = moved_fd, .rights = IPC_TEST_VMO_RIGHTS,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE}}};
    ipc_test_send(ipc, &outgoing);
    IPC_CHECK(outgoing.fds[0].fd == PH_IPC_NO_FD);
    struct pacha_fd_info info;
    IPC_CHECK(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, moved_fd,
        (uintptr_t)&info) == PACHA_SYSCALL_ERR_INVALID);
    IPC_CHECK(!ipc_test_receive(ipc, &returned));
    IPC_CHECK(returned.operation == IPC_TEST_HELD && returned.correlation == 2 && !returned.fd_count);
    IPC_CHECK(shared[0] == (generation ^ IPC_TEST_MARK) && returned.value == shared[0]);
    uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
    int stopped;
    while ((stopped = gpud_native_process_terminate(&child.launch.process, generation, 9)) == -EAGAIN)
        IPC_CHECK(ipc_test_now() < deadline);
    IPC_CHECK(!stopped && child.launch.process.terminal);
    reap_child(ipc, &child, 1);
    /* The surviving parent's VMO mapping is not revoked by peer death. */
    IPC_CHECK(shared[0] == (generation ^ IPC_TEST_MARK));
    shared[0] = 1234;
    IPC_CHECK(shared[0] == 1234);
    ipc_test_unmap(shared, IPC_TEST_PAGE);
    IPC_CHECK(!ph_ipc_packet_release(&outgoing));
}

static void stale_peer_round(struct ph_ipc *ipc, const void *image, size_t image_size) {
    struct ipc_test_child child = start_child(ipc, 12, 1, image, image_size, NULL);
    struct ph_ipc_packet outgoing = {.fd_count = PACHA_IPC_MAX_TRANSFER_FDS};
    for (size_t i = 0; i < outgoing.fd_count; ++i) {
        outgoing.fds[i] = (struct pacha_ipc_fd){
            .fd = new_vmo(IPC_TEST_PAGE, IPC_TEST_VMO_RIGHTS), .rights = IPC_TEST_VMO_RIGHTS};
    }
    /* Bypass the sender port only to inject an old peer generation. */
    struct pacha_ipc_msg stale = {.word0 = IPC_TEST_SHARE, .word1 = 11,
        .fds = outgoing.fds, .fd_count = outgoing.fd_count};
    IPC_CHECK(!pacha_syscall2(PACHA_IPC_SYSCALL_SEND, ipc->fd, (uintptr_t)&stale));
    reap_child(ipc, &child, 0);
    IPC_CHECK(!ph_ipc_packet_release(&outgoing));
}

static void backpressure(void) {
    uint64_t pair[2];
    struct ph_ipc sender = {0}, receiver = {0};
    IPC_CHECK(!pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uintptr_t)pair,
        PH_IPC_CHANNEL_RIGHTS, 0));
    IPC_CHECK(!ph_ipc_init(&sender, (int)pair[0], 1));
    IPC_CHECK(!ph_ipc_init(&receiver, (int)pair[1], 1));
    struct ph_ipc_packet fill = {.operation = IPC_TEST_READY, .generation = 1};
    size_t queued = 0;
    for (;;) {
        fill.correlation = queued;
        int result = ph_ipc_send(&sender, &fill);
        if (result == -EAGAIN) break;
        IPC_CHECK(!result && ++queued < 1024);
    }
    IPC_CHECK(queued);
    uint64_t source = new_vmo(IPC_TEST_PAGE, IPC_TEST_VMO_RIGHTS);
    struct ph_ipc_packet move = {.operation = IPC_TEST_MOVE, .generation = 1,
        .fd_count = 1, .fds = {{.fd = source, .rights = IPC_TEST_VMO_RIGHTS,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE}}};
    IPC_CHECK(ph_ipc_send(&sender, &move) == -EAGAIN && move.fds[0].fd == source);
    struct pacha_fd_info info;
    IPC_CHECK(!pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, source, (uintptr_t)&info));
    IPC_CHECK(info.kind == PACHA_FD_KIND_VMO);
    struct ph_ipc_packet received = {0};
    for (size_t i = 0; i < queued; ++i) {
        IPC_CHECK(!ph_ipc_receive(&receiver, 1, &received));
        IPC_CHECK(received.operation == IPC_TEST_READY && received.correlation == i && !received.fd_count);
    }
    IPC_CHECK(ph_ipc_receive(&receiver, 1, &received) == -EAGAIN);
    move.fds[0].rights |= PACHA_FD_RIGHT_MAP_EXEC; /* Authority not in source. */
    IPC_CHECK(ph_ipc_send(&sender, &move) == -EINVAL && move.fds[0].fd == source);
    IPC_CHECK(!pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, source, (uintptr_t)&info));
    IPC_CHECK(ph_ipc_receive(&receiver, 1, &received) == -EAGAIN);
    move.fds[0].rights = IPC_TEST_VMO_RIGHTS;
    IPC_CHECK(!ph_ipc_send(&sender, &move) && move.fds[0].fd == PH_IPC_NO_FD);
    IPC_CHECK(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, source,
        (uintptr_t)&info) == PACHA_SYSCALL_ERR_INVALID);
    IPC_CHECK(!ph_ipc_receive(&receiver, 1, &received) && received.fd_count == 1);
    IPC_CHECK(received.fds[0].fd == source); /* Real FD-table slot reuse. */
    IPC_CHECK(!ph_ipc_packet_release(&move));
    IPC_CHECK(!pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
        received.fds[0].fd, (uintptr_t)&info));
    IPC_CHECK(!ph_ipc_packet_release(&received));
    IPC_CHECK(!ph_ipc_destroy(&sender, 1));
    IPC_CHECK(!ph_ipc_destroy(&receiver, 1));
    ipc_test_log("NATIVE_KOBOX2_IPC_BACKPRESSURE=PASS failed-move-retained retry-once fd-slot-reuse\n");
}

static void bootstrap_round(struct ph_ipc *ipc, const void *image, size_t image_size, int malformed) {
    const uint64_t generation = malformed ? 14 : 13;
    struct ipc_test_child child = start_child(ipc, generation,
        malformed ? IPC_TEST_BOOTSTRAP_BAD_ORDER : IPC_TEST_BOOTSTRAP, image, image_size, NULL);
    struct ph_bootstrap_item items[IPC_TEST_BOOTSTRAP_ITEMS];
    for (size_t i = 0; i < IPC_TEST_BOOTSTRAP_ITEMS; ++i) {
        uint64_t fd = new_vmo(IPC_TEST_PAGE, IPC_TEST_VMO_RIGHTS);
        uint64_t *data = ipc_test_map(fd, IPC_TEST_PAGE, IPC_TEST_RW);
        data[0] = generation * 100 + i;
        ipc_test_unmap(data, IPC_TEST_PAGE);
        items[i] = (struct ph_bootstrap_item){
            .size = i < 2 + IPC_TEST_BOOTSTRAP_ARTIFACTS ? 37 + i : 0,
            .capability = {.fd = fd, .flags = PACHA_FD_FLAG_CLOEXEC,
                .rights = i < 2 + IPC_TEST_BOOTSTRAP_ARTIFACTS ?
                    PH_BOOTSTRAP_BLOB_RIGHTS : IPC_TEST_VMO_RIGHTS}};
    }
    struct ph_bootstrap_sender sender = {0};
    IPC_CHECK(!ph_bootstrap_sender_init(&sender, generation, items,
        IPC_TEST_BOOTSTRAP_ARTIFACTS, IPC_TEST_BOOTSTRAP_RESOURCES));
    uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
    while (!sender.finished) {
        if (malformed && sender.next == 5) {
            struct pacha_ipc_msg invalid = {.word0 = PH_BOOTSTRAP_BLOB, .word1 = generation,
                .word2 = 99, .word3 = items[5].size, .fds = &items[5].capability, .fd_count = 1};
            IPC_CHECK(!pacha_syscall2(PACHA_IPC_SYSCALL_SEND, ipc->fd, (uintptr_t)&invalid));
            break;
        }
        int result = ph_bootstrap_send_next(&sender, ipc);
        IPC_CHECK(!result || result == -EAGAIN);
        IPC_CHECK(ipc_test_now() < deadline);
    }
    if (!malformed) {
        struct ph_ipc_packet reply = {0};
        IPC_CHECK(!ipc_test_receive(ipc, &reply));
        IPC_CHECK(reply.operation == IPC_TEST_BOOTSTRAP_DONE && reply.value == IPC_TEST_BOOTSTRAP_ITEMS);
        IPC_CHECK(!reply.fd_count);
    }
    reap_child(ipc, &child, 0);
    for (size_t i = 0; i < IPC_TEST_BOOTSTRAP_ITEMS; ++i) {
        struct pacha_fd_info info;
        IPC_CHECK(!pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
            items[i].capability.fd, (uintptr_t)&info));
        ipc_test_close(items[i].capability.fd);
    }
    if (!malformed) ipc_test_log("NATIVE_KOBOX2_BOOTSTRAP=PASS staged=25 explicit-finish source-retained\n");
}

/* Encode real core/module identities with the same MIT codecs as gpud. The
 * independent launch config and the streamed VMO bytes are separate inputs. */
static void package_round(struct ph_ipc *ipc, const void *image, size_t image_size, unsigned int mode) {
    const uint64_t generation = 15 + mode - IPC_TEST_PACKAGE;
    const unsigned char *core, *module;
    uint32_t core_size, module_size;
    IPC_CHECK(!seed0_bootfs_open_file("/tests/package-core.so", &core, &core_size));
    IPC_CHECK(!seed0_bootfs_open_file("/tests/package-module.ko", &module, &module_size));
    const void *bytes[4] = {NULL, NULL, core, module};
    size_t sizes[4] = {0, 0, core_size, module_size};
    kb2_closure_manifest_artifact_t artifacts[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        artifacts[i] = (kb2_closure_manifest_artifact_t){.node_id = i + 1,
            .kind = i ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE : KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX | (i ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0),
            .content_size = sizes[i + 2], .namespace_name = {.data = i ? "drm_panel_orientation_quirks" : "core",
                .length = i ? sizeof("drm_panel_orientation_quirks") - 1 : 4}};
        kb2_sha256(bytes[i + 2], sizes[i + 2], artifacts[i].content_digest);
    }
    uint8_t manifest_bytes[4096], grant_bytes[4096];
    kb2_closure_manifest_dependency_t dependency = {.consumer_node_id = 2, .provider_node_id = 1};
    kb2_closure_manifest_source_t manifest = {.artifacts = artifacts, .artifact_count = 2,
        .dependencies = &dependency, .dependency_count = 1};
    IPC_CHECK(kb2_closure_manifest_encode(manifest_bytes, sizeof(manifest_bytes), &sizes[0],
        &manifest) == KB2_PROTOCOL_OK);
    bytes[0] = manifest_bytes;
    struct ph_package_identity identity = {.generation = generation};
    kb2_sha256(bytes[0], sizes[0], identity.manifest_digest);
    kb2_resource_grant_source_t grant = {.generation = mode == IPC_TEST_PACKAGE_OLD_GRANT ?
        generation - 1 : generation};
    memcpy(grant.closure_manifest_digest, identity.manifest_digest, 32);
    IPC_CHECK(kb2_resource_grant_encode(grant_bytes, sizeof(grant_bytes), &sizes[1], &grant) == KB2_PROTOCOL_OK);
    bytes[1] = grant_bytes;
    kb2_sha256(bytes[1], sizes[1], identity.grant_digest);
    struct ipc_test_child child = start_child(ipc, generation, mode, image, image_size, &identity);
    struct ph_bootstrap_item items[4];
    unsigned char *writers[4];
    size_t mapping_sizes[4];
    for (size_t i = 0; i < 4; ++i) {
        mapping_sizes[i] = (sizes[i] + IPC_TEST_PAGE - 1) & ~(IPC_TEST_PAGE - 1);
        uint64_t fd = new_vmo(mapping_sizes[i], IPC_TEST_VMO_RIGHTS);
        writers[i] = ipc_test_map(fd, mapping_sizes[i], IPC_TEST_RW);
        memcpy(writers[i], bytes[i], sizes[i]);
        items[i] = (struct ph_bootstrap_item){.size = sizes[i], .capability = {.fd = fd,
            .rights = PH_BOOTSTRAP_BLOB_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}};
    }
    if (mode == IPC_TEST_PACKAGE_CORRUPT) writers[2][0] ^= 1;
    struct ph_bootstrap_sender sender = {0};
    IPC_CHECK(!ph_bootstrap_sender_init(&sender, generation, items, 2, 0));
    uint64_t deadline = ipc_test_now() + UINT64_C(10000000000);
    while (!sender.finished) {
        int result = ph_bootstrap_send_next(&sender, ipc);
        IPC_CHECK(!result || result == -EAGAIN);
        IPC_CHECK(ipc_test_now() < deadline);
    }
    struct ph_ipc_packet reply = {0};
    IPC_CHECK(!ipc_test_receive(ipc, &reply) && !reply.fd_count);
    if (mode == IPC_TEST_PACKAGE) {
        IPC_CHECK(reply.operation == IPC_TEST_PACKAGE_SNAPSHOT);
        for (size_t i = 0; i < 4; ++i) writers[i][0] ^= 0x96;
    } else {
        IPC_CHECK(reply.operation == IPC_TEST_PACKAGE_REJECTED);
        IPC_CHECK(reply.value == (uint64_t)(mode == IPC_TEST_PACKAGE_CORRUPT ? EBADMSG : ESTALE));
    }
    for (size_t i = 0; i < 4; ++i) {
        ipc_test_unmap(writers[i], mapping_sizes[i]);
        ipc_test_close(items[i].capability.fd);
    }
    if (mode == IPC_TEST_PACKAGE) {
        reply = (struct ph_ipc_packet){.operation = IPC_TEST_PACKAGE_MUTATED, .generation = generation};
        ipc_test_send(ipc, &reply);
    }
    reap_child(ipc, &child, 0);
    if (mode == IPC_TEST_PACKAGE_CORRUPT) ipc_test_log("NATIVE_KOBOX2_PACKAGE_CORRUPT=PASS\n");
    if (mode == IPC_TEST_PACKAGE_OLD_GRANT) ipc_test_log("NATIVE_KOBOX2_PACKAGE_STALE=PASS\n");
}

static void aborted_launch_rounds(const void *image, size_t image_size) {
    const uint64_t before = ipc_test_used_fds();
    struct gpud_native_launch launch = {0};
    int old_process_fd = 0;
    for (uint64_t generation = 20; generation < 22; ++generation) {
        struct gpud_launch_request request = {.generation = generation,
            .image = image, .image_size = image_size, .stack_address = IPC_TEST_STACK,
            .stack_size = 16 * IPC_TEST_PAGE};
        IPC_CHECK(!gpud_native_launch_prepare(&launch, &request));
        IPC_CHECK(!launch.started && launch.thread_fd && launch.process.fd);
        if (old_process_fd) IPC_CHECK(launch.process.fd == old_process_fd);
        old_process_fd = launch.process.fd;
        IPC_CHECK(gpud_native_process_poll(&launch.process, generation) == -EAGAIN);
        IPC_CHECK(gpud_native_launch_abort(&launch, generation - 1) == -ESTALE);
        uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
        int result;
        while ((result = gpud_native_launch_abort(&launch, generation)) == -EAGAIN)
            IPC_CHECK(ipc_test_now() < deadline);
        IPC_CHECK(!result && !launch.process.fd && !launch.thread_fd && !launch.scratch_fd);
        IPC_CHECK(!gpud_native_launch_discard(&launch, generation));
        IPC_CHECK(gpud_native_launch_prepare(&launch, &request) == -ESTALE);
        IPC_CHECK(ipc_test_used_fds() == before);
    }
    ipc_test_log("NATIVE_GPUD_LAUNCH_ABORT=PASS suspended-thread kill-wait rollback generation fd-reuse\n");
}

static void *controller_allocate(void *context, size_t size) {
    (void)context;
    size = (size + IPC_TEST_PAGE - 1) & ~(IPC_TEST_PAGE - 1);
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, size,
        IPC_TEST_RW, PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
    return address >= (long)IPC_TEST_PAGE ? (void *)(uintptr_t)address : NULL;
}

static void controller_free(void *context, void *pointer, size_t size) {
    (void)context;
    size = (size + IPC_TEST_PAGE - 1) & ~(IPC_TEST_PAGE - 1);
    ipc_test_unmap(pointer, size);
}

static void controller_launch_rounds(const void *image, size_t image_size) {
    const uint64_t before = ipc_test_used_fds();
    kb2_controller_t *controller = gpud_test_controller_create(controller_allocate, controller_free, NULL);
    IPC_CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);
    struct ph_ipc ipc = {0};
    struct gpud_native_launch launch = {0};
    struct gpud_launch_transaction previous = {0};
    int previous_process_fd = 0;
    for (uint64_t generation = 1; generation <= 3; ++generation) {
        IPC_CHECK(kb2_controller_generation(controller) == generation);
        if (previous.controller) IPC_CHECK(gpud_launch_step(&previous) == KB2_STATUS_STALE_GENERATION);
        uint64_t pair[2];
        IPC_CHECK(!pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uintptr_t)pair,
            PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE, 0));
        IPC_CHECK(!ph_ipc_init(&ipc, (int)pair[0], generation));
        /* This host-action fixture owns only a bootstrap channel and startup
         * copies, not a GPU resource set. No device reset is synthesized. */
        gpud_test_complete(controller, KB2_ACTION_ALLOCATE_RESOURCES, 50 + generation, 0);
        struct pacha_process_fd_grant grant = {.source_fd = pair[1], .target_fd = IPC_TEST_CHILD_FD,
            .rights = PH_IPC_CHANNEL_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC};
        struct ipc_test_config config = {.generation = generation, .mode = IPC_TEST_NORMAL};
        struct gpud_launch_blob blob = {.bytes = &config, .size = sizeof(config),
            .address = IPC_TEST_CONFIG};
        struct gpud_launch_request request = {.generation = generation,
            .image = image, .image_size = generation == 1 ? 1 : image_size,
            .stack_address = IPC_TEST_STACK, .stack_size = 16 * IPC_TEST_PAGE,
            .blobs = &blob, .blob_count = 1, .grants = &grant, .grant_count = 1};
        struct gpud_launch_resources resources = {.generation = generation,
            .resource_set_id = 50 + generation, .sandbox_id = 70 + generation, .request = &request};
        memset(resources.manifest_digest, 0x41, sizeof(resources.manifest_digest));
        struct gpud_launch_transaction transaction = {0};
        IPC_CHECK(gpud_launch_begin(&transaction, controller, &launch, &ipc, &resources) == KB2_STATUS_OK);
        ipc_test_close(pair[1]);
        kb2_status_t result = gpud_launch_step(&transaction);
        IPC_CHECK(transaction.completed);
        struct gpud_process_watch watch = {0};
        if (generation == 1) {
            IPC_CHECK(result == KB2_STATUS_HOST_FAILURE && transaction.launch_error && !launch.process.fd);
        } else {
            IPC_CHECK(result == KB2_STATUS_OK && launch.started);
            if (previous_process_fd) IPC_CHECK(launch.process.fd == previous_process_fd);
            previous_process_fd = launch.process.fd;
            IPC_CHECK(kb2_controller_state(controller) == KB2_STATE_STARTING);
            IPC_CHECK(kb2_action_type(kb2_controller_pending_action(controller)) == KB2_ACTION_TRANSFER_RESOURCES);
            IPC_CHECK(gpud_process_watch_init(&watch, controller, &launch.process, &ipc,
                resources.resource_set_id, resources.sandbox_id) == KB2_STATUS_OK);
            struct ph_ipc_packet ready = {0};
            IPC_CHECK(!ipc_test_receive(&ipc, &ready));
            IPC_CHECK(ready.operation == IPC_TEST_READY && !ready.fd_count);
            /* Force the TRANSFER action to fail before any GPU resource is
             * delegated. Fixture READY is not a Linux core READY message. */
            const kb2_action_t *action = kb2_controller_pending_action(controller);
            IPC_CHECK(kb2_controller_complete_action(controller, generation, kb2_action_token(action),
                KB2_STATUS_HOST_FAILURE, 0, 0) == KB2_STATUS_HOST_FAILURE);
        }
        IPC_CHECK((generation < 3 ? kb2_controller_restart(controller) :
            kb2_controller_stop(controller)) == KB2_STATUS_OK);
        if (watch.controller) {
            const kb2_action_t *action = kb2_controller_pending_action(controller);
            IPC_CHECK(kb2_action_type(action) == KB2_ACTION_TERMINATE_SANDBOX);
            uint64_t token = kb2_action_token(action);
            uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
            while ((result = gpud_process_action(&watch, generation, token, 9)) == KB2_STATUS_ACTION_PENDING)
                IPC_CHECK(ipc_test_now() < deadline);
            IPC_CHECK(result == KB2_STATUS_OK && launch.process.terminal);
            IPC_CHECK(launch.process.exit.state == GPUD_PROCESS_KILLED &&
                launch.process.exit.code == 9);
        }
        /* Actual channel references are gone before the resource completion. */
        IPC_CHECK(!ph_ipc_destroy(&ipc, generation));
        gpud_test_complete(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0);
        if (watch.controller) {
            const kb2_action_t *action = kb2_controller_pending_action(controller);
            IPC_CHECK(kb2_action_type(action) == KB2_ACTION_REAP_SANDBOX);
            IPC_CHECK(gpud_process_action(&watch, generation, kb2_action_token(action), 9) == KB2_STATUS_OK);
            IPC_CHECK(!gpud_native_launch_discard(&launch, generation));
        }
        gpud_test_complete(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0);
        previous = transaction;
        IPC_CHECK(ipc_test_used_fds() == before);
    }
    IPC_CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);
    kb2_controller_destroy(controller);
    IPC_CHECK(ipc_test_used_fds() == before);
    ipc_test_log("NATIVE_GPUD_CONTROLLER_LAUNCH=PASS failed-image launch terminate reap restart stale-action\n");
}

int main(void) {
    const uint64_t before = ipc_test_used_fds();
    backpressure();
    IPC_CHECK(ipc_test_used_fds() == before);
    const unsigned char *image;
    uint32_t image_size;
    IPC_CHECK(!seed0_bootfs_open_file("/tests/kobox2-ipc-child.elf", &image, &image_size));
    struct ph_ipc ipc = {0};
    int previous_fd = 0;
    shared_round(&ipc, 10, image, image_size, &previous_fd);
    IPC_CHECK(ipc_test_used_fds() == before);
    ipc_test_log("NATIVE_KOBOX2_IPC_ROUND=PASS generation=10 share=19 move=1 killed=1\n");
    shared_round(&ipc, 11, image, image_size, &previous_fd);
    IPC_CHECK(ipc_test_used_fds() == before);
    ipc_test_log("NATIVE_KOBOX2_IPC_ROUND=PASS generation=11 channel-fd-reused stale-local-refused\n");
    stale_peer_round(&ipc, image, image_size);
    bootstrap_round(&ipc, image, image_size, 0);
    IPC_CHECK(ipc_test_used_fds() == before);
    bootstrap_round(&ipc, image, image_size, 1);
    IPC_CHECK(ipc_test_used_fds() == before);
    for (unsigned int mode = IPC_TEST_PACKAGE; mode <= IPC_TEST_PACKAGE_OLD_GRANT; ++mode) {
        package_round(&ipc, image, image_size, mode);
        IPC_CHECK(ipc_test_used_fds() == before);
    }
    aborted_launch_rounds(image, image_size);
    controller_launch_rounds(image, image_size);
    IPC_CHECK(ipc_test_used_fds() == before);
    ipc_test_log("NATIVE_GPUD_LAUNCH=PASS native-elf explicit-grant config process thread no-fd-leak\n");
    ipc_test_log("NATIVE_GPUD_PROCESS=PASS wait-proof kill normal-exit stale-generation reap\n");
    ipc_test_log("NATIVE_KOBOX2_IPC=PASS separate-process rights share move death hangup generation no-fd-leak\n");
    return 0;
}
