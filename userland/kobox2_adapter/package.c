/* SPDX-License-Identifier: MIT */
#include "package.h"
#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

#define PH_PACKAGE_PAGE 4096ul
_Static_assert(PH_BOOTSTRAP_MAX_ARTIFACTS == KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS,
    "native bootstrap and package artifact bounds");

static void unmap_or_fail(struct ph_package *package, void *address, size_t size) {
    long result = pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)address, size);
    if (result) {
        package->fatal_cleanup(package->fatal_context, result);
        __builtin_trap(); /* A returning fatal hook violates the ownership contract. */
    }
}

static void close_snapshot(void *context, struct kobox_boot_blob *blob) {
    struct ph_package *package = context;
    struct ph_package_snapshot *snapshot = blob->owner;
    unmap_or_fail(package, snapshot->address, snapshot->mapping_size);
    *snapshot = (struct ph_package_snapshot){0};
    *blob = (struct kobox_boot_blob){0};
}

static enum kobox_package_result open_snapshot(void *context, const void *input,
    size_t maximum, struct kobox_boot_blob *blob) {
    struct ph_package *package = context;
    const struct ph_bootstrap_item *item = input;
    struct pacha_fd_info info = {0};
    struct ph_package_snapshot *snapshot = NULL;

    if (!item || item->capability.fd < 16 || item->capability.fd >= PACHA_FD_TABLE_LIMIT)
        return KOBOX_PACKAGE_INVALID;
    if (!item->size || item->size > maximum || item->size > SIZE_MAX - PH_PACKAGE_PAGE + 1)
        return KOBOX_PACKAGE_TOO_LARGE;
    const size_t size = (size_t)item->size;
    const size_t mapping_size = (size + PH_PACKAGE_PAGE - 1) & ~(PH_PACKAGE_PAGE - 1);
    package->native_error = pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
        item->capability.fd, (uintptr_t)&info);
    if (package->native_error) return KOBOX_PACKAGE_IO;
    if (info.kind != PACHA_FD_KIND_VMO || info.rights != PH_BOOTSTRAP_BLOB_RIGHTS)
        return KOBOX_PACKAGE_INVALID;
    if (info.size < size) return KOBOX_PACKAGE_TOO_LARGE;
    for (size_t i = 0; i < 2 + PH_BOOTSTRAP_MAX_ARTIFACTS; ++i) {
        if (!package->snapshots[i].address) {
            snapshot = &package->snapshots[i];
            break;
        }
    }
    if (!snapshot) return KOBOX_PACKAGE_NO_MEMORY;

    long source_address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
        item->capability.fd, 0, mapping_size, PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    if (source_address < (long)PH_PACKAGE_PAGE) {
        package->native_error = source_address;
        return KOBOX_PACKAGE_IO;
    }
    void *source = (void *)(uintptr_t)source_address;
    long private_address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, mapping_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
    if (private_address < (long)PH_PACKAGE_PAGE) {
        package->native_error = private_address;
        unmap_or_fail(package, source, mapping_size);
        return KOBOX_PACKAGE_IO;
    }
    snapshot->address = (void *)(uintptr_t)private_address;
    snapshot->mapping_size = mapping_size;
    memcpy(snapshot->address, source, size);
    unmap_or_fail(package, source, mapping_size);
    package->native_error = pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT,
        (uintptr_t)snapshot->address, mapping_size, PACHA_PROT_READ);
    if (package->native_error) {
        unmap_or_fail(package, snapshot->address, mapping_size);
        *snapshot = (struct ph_package_snapshot){0};
        return KOBOX_PACKAGE_IO;
    }
    *blob = (struct kobox_boot_blob){.owner = snapshot, .data = snapshot->address, .size = size};
    return KOBOX_PACKAGE_OK;
}

static int package_error(enum kobox_package_result result, long native_error) {
    switch (result) {
    case KOBOX_PACKAGE_OK: return 0;
    case KOBOX_PACKAGE_INVALID: return -EINVAL;
    case KOBOX_PACKAGE_NO_MEMORY: return -ENOMEM;
    case KOBOX_PACKAGE_IO: return native_error == PACHA_SYSCALL_ERR_ALLOC ? -ENOMEM : -EIO;
    case KOBOX_PACKAGE_IMMUTABILITY: return -EPERM;
    case KOBOX_PACKAGE_TOO_LARGE: return -EFBIG;
    case KOBOX_PACKAGE_MALFORMED: return -EBADMSG;
    case KOBOX_PACKAGE_EXEC_FORMAT: return -ENOEXEC;
    case KOBOX_PACKAGE_STALE: return -ESTALE;
    }
    return -EIO;
}

static int handles_cover_grant(const kb2_resource_grant_t *grant, size_t count) {
    uint64_t used = 0;
    for (size_t i = 0; i < kb2_resource_grant_handle_binding_count(grant); ++i) {
        kb2_resource_grant_handle_binding_t binding;
        if (kb2_resource_grant_handle_binding(grant, i, &binding) != KB2_PROTOCOL_OK ||
            binding.transfer_handle_index >= count) return 0;
        used |= UINT64_C(1) << binding.transfer_handle_index;
    }
    const uint64_t expected = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
    return used == expected;
}

int ph_package_open(struct ph_package *package, const struct ph_bootstrap_receiver *bundle,
    const struct ph_package_identity *identity,
    bool (*elf_matches)(const void *data, size_t size, bool core),
    ph_package_fatal_fn fatal_cleanup, void *fatal_context) {
    if (!package || package->common.operations || package->fatal_cleanup || !bundle ||
        !bundle->complete || bundle->failed || !identity || !identity->generation ||
        !fatal_cleanup || !elf_matches || !bundle->artifact_count ||
        bundle->artifact_count > PH_BOOTSTRAP_MAX_ARTIFACTS ||
        bundle->resource_count > PH_BOOTSTRAP_MAX_RESOURCES ||
        bundle->received != 2 + bundle->artifact_count + bundle->resource_count) return -EINVAL;
    if (bundle->generation != identity->generation) return -ESTALE;
    const void *artifacts[PH_BOOTSTRAP_MAX_ARTIFACTS];
    for (size_t i = 0; i < bundle->artifact_count; ++i) artifacts[i] = &bundle->items[i + 2];
    struct kobox_boot_package_input input = {
        .expected_generation = identity->generation,
        .manifest = &bundle->items[0], .grant = &bundle->items[1],
        .artifacts = artifacts, .artifact_count = bundle->artifact_count,
        .operations = &package->operations, .context = package,
    };
    memcpy(input.expected_manifest_digest, identity->manifest_digest, 32);
    memcpy(input.expected_grant_digest, identity->grant_digest, 32);
    package->operations = (struct kobox_package_operations){
        .open = open_snapshot, .close = close_snapshot, .elf_matches = elf_matches};
    package->fatal_cleanup = fatal_cleanup;
    package->fatal_context = fatal_context;
    enum kobox_package_result result = kobox_boot_package_open(&input, &package->common);
    int error = package_error(result, package->native_error);
    if (!error && !handles_cover_grant(&package->common.grant, bundle->resource_count))
        error = -EBADMSG;
    if (error) ph_package_close(package);
    return error;
}

void ph_package_close(struct ph_package *package) {
    if (!package) return;
    kobox_boot_package_close(&package->common);
    *package = (struct ph_package){0};
}
