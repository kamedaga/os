/* SPDX-License-Identifier: MIT */
/* A separate, fixed-address native test image with an explicit one-FD grant.
 * No parent clone, Linux ABI runtime or controller is linked here. The GPL
 * package verifier is linked only in this child, never in its parent. */
#define IPC_TEST_CHILD
#include "native_kobox2_ipc_common.h"
#include "../userland/kobox2_adapter/package.h"
#include "arch/x86_64/elf.h"
#include <kobox2/sha256.h>

static void package_cleanup_fatal(void *context, long status) {
    (void)context; (void)status;
    ipc_test_fail(__LINE__);
}

static int package_round(struct ph_ipc *ipc, const struct ipc_test_config *config) {
    struct ph_bootstrap_receiver bundle = {0};
    IPC_CHECK(!ph_bootstrap_receiver_init(&bundle, ipc->generation, 2, 0));
    uint64_t deadline = ipc_test_now() + UINT64_C(10000000000);
    while (!bundle.complete) {
        int result = ph_bootstrap_receive_next(&bundle, ipc);
        IPC_CHECK(!result || result == -EAGAIN);
        IPC_CHECK(ipc_test_now() < deadline);
    }
    struct ph_package package = {0};
    int result = ph_package_open(&package, &bundle, &config->identity,
        kobox_x86_64_elf_matches, package_cleanup_fatal, NULL);
    if (config->mode != IPC_TEST_PACKAGE) {
        IPC_CHECK(result == (config->mode == IPC_TEST_PACKAGE_CORRUPT ? -EBADMSG : -ESTALE));
        IPC_CHECK(!package.common.operations && !package.fatal_cleanup);
        IPC_CHECK(!ph_bootstrap_release(&bundle));
        IPC_CHECK(ipc_test_used_fds() == 17);
        struct ph_ipc_packet reply = {.operation = IPC_TEST_PACKAGE_REJECTED,
            .generation = ipc->generation, .value = (uint64_t)-result};
        ipc_test_send(ipc, &reply);
        IPC_CHECK(!ph_ipc_destroy(ipc, ipc->generation));
        return 0;
    }
    IPC_CHECK(!result && package.common.artifact_count == 2);
    IPC_CHECK(package.common.artifacts[0].size > 1024 * 1024); /* Actual fixed core, not an ELF header. */
    struct ph_ipc_packet reply = {.operation = IPC_TEST_PACKAGE_SNAPSHOT,
        .generation = ipc->generation};
    ipc_test_send(ipc, &reply);
    IPC_CHECK(!ipc_test_receive(ipc, &reply));
    IPC_CHECK(reply.operation == IPC_TEST_PACKAGE_MUTATED && !reply.fd_count);
    IPC_CHECK(!ph_bootstrap_release(&bundle)); /* All original package FDs may now disappear. */
    IPC_CHECK(ipc_test_used_fds() == 17);
    uint8_t digest[32];
    kb2_sha256(package.common.manifest_blob.data, package.common.manifest_blob.size, digest);
    IPC_CHECK(!memcmp(digest, config->identity.manifest_digest, 32));
    kb2_sha256(package.common.grant_blob.data, package.common.grant_blob.size, digest);
    IPC_CHECK(!memcmp(digest, config->identity.grant_digest, 32));
    for (size_t i = 0; i < 2; ++i) {
        kb2_closure_manifest_artifact_t artifact;
        IPC_CHECK(kb2_closure_manifest_artifact(&package.common.manifest, i, &artifact) == KB2_PROTOCOL_OK);
        kb2_sha256(package.common.artifacts[i].data, package.common.artifacts[i].size, digest);
        IPC_CHECK(!memcmp(digest, artifact.content_digest, 32));
    }
    ph_package_close(&package);
    IPC_CHECK(!package.common.operations && !package.fatal_cleanup);
    IPC_CHECK(!ph_ipc_destroy(ipc, ipc->generation));
    ipc_test_log("NATIVE_KOBOX2_PACKAGE_SNAPSHOT=PASS actual-core-module external-write-isolated inputs-closed\n");
    return 0;
}

static int bootstrap_round(struct ph_ipc *ipc, uint64_t mode) {
    struct ph_bootstrap_receiver receiver = {0};
    IPC_CHECK(!ph_bootstrap_receiver_init(&receiver, ipc->generation,
        IPC_TEST_BOOTSTRAP_ARTIFACTS, IPC_TEST_BOOTSTRAP_RESOURCES));
    uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
    while (!receiver.complete) {
        int result = ph_bootstrap_receive_next(&receiver, ipc);
        if (result == -EAGAIN) {
            IPC_CHECK(ipc_test_now() < deadline);
            continue;
        }
        if (mode == IPC_TEST_BOOTSTRAP_BAD_ORDER && result == -EPROTO) {
            IPC_CHECK(!receiver.generation && !ipc->admitted);
            IPC_CHECK(ipc_test_used_fds() == 17);
            IPC_CHECK(!ph_ipc_destroy(ipc, ipc->generation));
            IPC_CHECK(ipc_test_used_fds() == 16);
            ipc_test_log("NATIVE_KOBOX2_BOOTSTRAP_REJECT=PASS partial-bundle-reclaimed\n");
            return 0;
        }
        IPC_CHECK(!result);
    }
    IPC_CHECK(mode == IPC_TEST_BOOTSTRAP);
    IPC_CHECK(receiver.received == IPC_TEST_BOOTSTRAP_ITEMS);
    IPC_CHECK(ipc_test_used_fds() == 17 + IPC_TEST_BOOTSTRAP_ITEMS);
    /* These are transport sample bytes, not a fabricated valid Linux package. */
    for (size_t i = 0; i < receiver.received; ++i) {
        struct ph_bootstrap_item *item = &receiver.items[i];
        IPC_CHECK(item->size == (i < 2 + IPC_TEST_BOOTSTRAP_ARTIFACTS ? 37 + i : 0));
        uint64_t *data = ipc_test_map(item->capability.fd, IPC_TEST_PAGE, PACHA_PROT_READ);
        IPC_CHECK(data[0] == ipc->generation * 100 + i);
        ipc_test_unmap(data, IPC_TEST_PAGE);
    }
    IPC_CHECK(!ph_bootstrap_release(&receiver));
    IPC_CHECK(ipc_test_used_fds() == 17);
    struct ph_ipc_packet reply = {.operation = IPC_TEST_BOOTSTRAP_DONE,
        .generation = ipc->generation, .value = IPC_TEST_BOOTSTRAP_ITEMS};
    ipc_test_send(ipc, &reply);
    IPC_CHECK(!ph_ipc_destroy(ipc, ipc->generation));
    IPC_CHECK(ipc_test_used_fds() == 16);
    return 0;
}

int main(void) {
    const struct ipc_test_config *config = (void *)IPC_TEST_CONFIG;
    const uint64_t generation = config->generation;
    struct ph_ipc ipc = {0};
    struct ph_ipc_packet packet = {.operation = IPC_TEST_READY, .generation = generation};
    IPC_CHECK(ipc_test_used_fds() == 17); /* Reserved slots + channel only. */
    IPC_CHECK(!ph_ipc_init(&ipc, IPC_TEST_CHILD_FD, generation));
    ipc_test_send(&ipc, &packet);
    if (config->mode == IPC_TEST_TIMED) {
        packet = (struct ph_ipc_packet){0};
        IPC_CHECK(!ipc_test_receive(&ipc, &packet));
        uint64_t until = ipc_test_now() + UINT64_C(20000000);
        while (ipc_test_now() < until) __asm__ volatile("pause");
        long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE,
            IPC_TEST_PAGE, IPC_TEST_VMO_RIGHTS, 0);
        IPC_CHECK(fd >= 16);
        packet = (struct ph_ipc_packet){.operation = IPC_TEST_RETURN,
            .generation = generation, .fd_count = 1,
            .fds = {{.fd = (uint64_t)fd, .rights = IPC_TEST_VMO_RIGHTS,
                .transfer_flags = PACHA_IPC_TRANSFER_MOVE}}};
        ipc_test_send(&ipc, &packet);
        packet = (struct ph_ipc_packet){0};
        IPC_CHECK(!ipc_test_receive(&ipc, &packet));
        until = ipc_test_now() + UINT64_C(20000000);
        while (ipc_test_now() < until) __asm__ volatile("pause");
        return 0; /* Actual process exit closes the last peer reference. */
    }
    if (config->mode >= IPC_TEST_PACKAGE) return package_round(&ipc, config);
    if (config->mode == IPC_TEST_BOOTSTRAP || config->mode == IPC_TEST_BOOTSTRAP_BAD_ORDER)
        return bootstrap_round(&ipc, config->mode);
    packet = (struct ph_ipc_packet){0};
    int result = ipc_test_receive(&ipc, &packet);
    if (config->mode == IPC_TEST_STALE) {
        IPC_CHECK(result == -ESTALE && !packet.fd_count && !ipc.rejected.fd_count);
        IPC_CHECK(ipc_test_used_fds() == 17);
        IPC_CHECK(!ph_ipc_destroy(&ipc, generation));
        IPC_CHECK(ipc_test_used_fds() == 16);
        ipc_test_log("NATIVE_KOBOX2_IPC_STALE=PASS received-fds-reclaimed\n");
        return 0;
    }
    IPC_CHECK(!result && packet.operation == IPC_TEST_SHARE && packet.correlation == 1);
    IPC_CHECK(packet.fd_count == PACHA_IPC_MAX_TRANSFER_FDS);
    IPC_CHECK(ipc_test_used_fds() == 17 + PACHA_IPC_MAX_TRANSFER_FDS);
    for (size_t i = 0; i < packet.fd_count; ++i) {
        struct pacha_fd_info info;
        IPC_CHECK(!pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
            packet.fds[i].fd, (uintptr_t)&info));
        const uint64_t rights = IPC_TEST_VMO_RIGHTS &
            (i ? UINT64_MAX : ~PACHA_FD_RIGHT_MAP_WRITE);
        IPC_CHECK(info.kind == PACHA_FD_KIND_VMO && info.rights == rights);
        IPC_CHECK(info.flags == PACHA_FD_FLAG_CLOEXEC);
        if (!i) {
            long refused = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
                packet.fds[i].fd, 0, IPC_TEST_PAGE, IPC_TEST_RW, PACHA_MMAP_SHARED, 0);
            IPC_CHECK(refused == PACHA_SYSCALL_ERR_INVALID);
        }
        uint64_t *data = ipc_test_map(packet.fds[i].fd, IPC_TEST_PAGE,
            i ? IPC_TEST_RW : PACHA_PROT_READ);
        IPC_CHECK(data[0] == packet.value + i);
        if (i) data[0] ^= IPC_TEST_MARK;
        ipc_test_unmap(data, IPC_TEST_PAGE);
        packet.fds[i].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    }
    packet.operation = IPC_TEST_RETURN;
    ipc_test_send(&ipc, &packet);
    for (size_t i = 0; i < packet.fd_count; ++i) IPC_CHECK(packet.fds[i].fd == PH_IPC_NO_FD);
    IPC_CHECK(!ph_ipc_packet_release(&packet));
    IPC_CHECK(ipc_test_used_fds() == 17);

    IPC_CHECK(!ipc_test_receive(&ipc, &packet));
    IPC_CHECK(packet.operation == IPC_TEST_MOVE && packet.correlation == 2 && packet.fd_count == 1);
    uint64_t *held = ipc_test_map(packet.fds[0].fd, IPC_TEST_PAGE, IPC_TEST_RW);
    IPC_CHECK(held[0] == packet.value);
    held[0] ^= IPC_TEST_MARK;
    struct ph_ipc_packet reply = {.operation = IPC_TEST_HELD, .generation = generation,
        .correlation = 2, .value = held[0]};
    ipc_test_send(&ipc, &reply);
    /* The parent kills this process while it owns the received VMO FD and
     * mapping. Do not cooperatively clean up or simulate process death. */
    for (;;) __asm__ volatile("pause" ::: "memory");
}

__attribute__((noreturn)) void ipc_child_start(void) {
    int status = main();
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, (uint64_t)status);
    __builtin_trap();
}

__asm__(".section .text.entry,\"ax\",@progbits\n.global _start\n"
    "_start: and $-16,%rsp; call ipc_child_start; ud2\n"
    ".section .note.GNU-stack,\"\",@progbits\n");
