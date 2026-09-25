/* SPDX-License-Identifier: MIT */
/* Actual package verifier/codecs and native backend; POSIX mappings model
 * native VMOs solely for syscall failure injection and protection checks. */
#define _GNU_SOURCE
#include "../userland/kobox2_adapter/package.h"
#include "arch/x86_64/elf.h"
#include <pacha/syscall.h>
#include <kobox2/sha256.h>
#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct source { int fd; unsigned char *data; size_t size; };
static struct source sources[4];
static struct { void *address; size_t size; } mappings[16];
static unsigned int live_mappings;
static int map_fail, protect_fail, info_fail, unmap_fail, wrong_kind;
static unsigned int fatal_expected;

static int fail_now(int *counter) {
    if (*counter < 0) return 0;
    if (!*counter) { *counter = -1; return 1; }
    --*counter;
    return 0;
}

static void fatal_cleanup(void *context, long status) {
    assert(context == &fatal_expected && fatal_expected && status == PACHA_SYSCALL_ERR_MAP);
    _exit(77);
}

long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    if (nr == PACHA_FD_SYSCALL_GET_INFO) {
        if (fail_now(&info_fail)) return PACHA_SYSCALL_ERR_ALLOC;
        assert(a0 >= 100 && a0 < 104);
        *(struct pacha_fd_info *)(uintptr_t)a1 = (struct pacha_fd_info){
            .kind = wrong_kind ? PACHA_FD_KIND_EVENT : PACHA_FD_KIND_VMO,
            .rights = PH_BOOTSTRAP_BLOB_RIGHTS, .size = 4096};
        return 0;
    }
    assert(nr == PACHA_VM_SYSCALL_MUNMAP);
    if (fail_now(&unmap_fail)) return PACHA_SYSCALL_ERR_MAP;
    for (size_t i = 0; i < 16; ++i) {
        if (mappings[i].address != (void *)(uintptr_t)a0) continue;
        assert(mappings[i].size == a1 && !munmap(mappings[i].address, mappings[i].size));
        mappings[i].address = NULL;
        assert(live_mappings); --live_mappings;
        return 0;
    }
    abort();
}

long pacha_syscall3(uint64_t nr, uint64_t address, uint64_t size, uint64_t prot) {
    assert(nr == PACHA_VM_SYSCALL_MPROTECT && prot == PACHA_PROT_READ);
    if (fail_now(&protect_fail)) return PACHA_SYSCALL_ERR_INVALID;
    assert(!mprotect((void *)(uintptr_t)address, size, PROT_READ));
    return 0;
}

long pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address, uint64_t size,
    uint64_t prot, uint64_t flags, uint64_t offset) {
    assert(nr == PACHA_VM_SYSCALL_MMAP && !address && !offset && size == 4096);
    if (fail_now(&map_fail)) return PACHA_SYSCALL_ERR_ALLOC;
    int native_flags, native_fd;
    if (fd) {
        assert(fd >= 100 && fd < 104 && flags == PACHA_MMAP_SHARED && prot == PACHA_PROT_READ);
        native_flags = MAP_SHARED;
        native_fd = sources[fd - 100].fd;
    } else {
        assert(flags == (PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS));
        assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE));
        native_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        native_fd = -1;
    }
    void *mapped = mmap(NULL, size, fd ? PROT_READ : PROT_READ | PROT_WRITE,
        native_flags, native_fd, 0);
    assert(mapped != MAP_FAILED);
    for (size_t i = 0; i < 16; ++i) {
        if (mappings[i].address) continue;
        mappings[i].address = mapped;
        mappings[i].size = size;
        ++live_mappings;
        return (long)(uintptr_t)mapped;
    }
    abort();
}

static void source_set(size_t index, const void *bytes, size_t size) {
    assert(size && size <= 4096);
    struct source *source = &sources[index];
    source->fd = memfd_create("native-package-unit", MFD_CLOEXEC);
    assert(source->fd >= 0 && !ftruncate(source->fd, 4096));
    source->data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, source->fd, 0);
    assert(source->data != MAP_FAILED);
    memcpy(source->data, bytes, size);
    source->size = size;
}

static struct ph_package_identity identity;
static struct ph_bootstrap_receiver bundle;

static void setup(int wrong_elf, uint64_t grant_generation) {
    assert(!live_mappings);
    map_fail = protect_fail = info_fail = unmap_fail = -1;
    wrong_kind = fatal_expected = 0;
    identity = (struct ph_package_identity){.generation = 17};
    bundle = (struct ph_bootstrap_receiver){.generation = 17, .artifact_count = 2,
        .received = 4, .complete = 1};
    kb2_closure_manifest_artifact_t artifacts[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        Elf64_Ehdr header = {.e_type = i ? ET_REL : ET_DYN,
            .e_machine = wrong_elf ? EM_AARCH64 : EM_X86_64,
            .e_version = EV_CURRENT, .e_ehsize = sizeof(header)};
        memcpy(header.e_ident, ELFMAG, SELFMAG);
        header.e_ident[EI_CLASS] = ELFCLASS64;
        header.e_ident[EI_DATA] = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        source_set(i + 2, &header, sizeof(header));
        artifacts[i] = (kb2_closure_manifest_artifact_t){.node_id = i + 1,
            .kind = i ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE : KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX | (i ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0),
            .content_size = sizeof(header), .namespace_name = {.data = i ? "module" : "core",
                .length = i ? 6 : 4}};
        kb2_sha256(&header, sizeof(header), artifacts[i].content_digest);
    }
    kb2_closure_manifest_dependency_t dependency = {.consumer_node_id = 2, .provider_node_id = 1};
    kb2_closure_manifest_source_t manifest = {.artifacts = artifacts, .artifact_count = 2,
        .dependencies = &dependency, .dependency_count = 1};
    uint8_t bytes[4096]; size_t written;
    assert(kb2_closure_manifest_encode(bytes, sizeof(bytes), &written, &manifest) == KB2_PROTOCOL_OK);
    source_set(0, bytes, written);
    kb2_sha256(bytes, written, identity.manifest_digest);
    kb2_resource_grant_source_t grant = {.generation = grant_generation};
    memcpy(grant.closure_manifest_digest, identity.manifest_digest, 32);
    assert(kb2_resource_grant_encode(bytes, sizeof(bytes), &written, &grant) == KB2_PROTOCOL_OK);
    source_set(1, bytes, written);
    kb2_sha256(bytes, written, identity.grant_digest);
    for (size_t i = 0; i < 4; ++i) bundle.items[i] = (struct ph_bootstrap_item){
        .size = sources[i].size, .capability = {.fd = 100 + i,
            .rights = PH_BOOTSTRAP_BLOB_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}};
}

static void finish(void) {
    assert(!live_mappings);
    for (size_t i = 0; i < 4; ++i) {
        assert(!munmap(sources[i].data, 4096));
        assert(!close(sources[i].fd));
    }
}

static void expect_failure(int error) {
    struct ph_package package = {0};
    assert(ph_package_open(&package, &bundle, &identity, kobox_x86_64_elf_matches,
        fatal_cleanup, &fatal_expected) == error);
    assert(!package.common.operations && !package.fatal_cleanup && !live_mappings);
    for (size_t i = 0; i < 4; ++i) assert(bundle.items[i].capability.fd == 100 + i);
}

static void snapshot_and_protection(void) {
    setup(0, 17);
    struct ph_package package = {0};
    assert(!ph_package_open(&package, &bundle, &identity, kobox_x86_64_elf_matches,
        fatal_cleanup, &fatal_expected));
    assert(live_mappings == 4 && package.common.artifact_count == 2);
    assert(ph_package_open(&package, &bundle, &identity, kobox_x86_64_elf_matches,
        fatal_cleanup, &fatal_expected) == -EINVAL);
    const struct kobox_boot_blob *blobs[] = {&package.common.manifest_blob,
        &package.common.grant_blob, &package.common.artifacts[0], &package.common.artifacts[1]};
    for (size_t i = 0; i < 4; ++i) {
        uint8_t expected[32], actual[32];
        kb2_sha256(sources[i].data, sources[i].size, expected);
        memset(sources[i].data, 0x96, sources[i].size); /* Publisher still has a writer. */
        kb2_sha256(blobs[i]->data, blobs[i]->size, actual);
        assert(!memcmp(expected, actual, 32));
    }
    pid_t writer = fork();
    assert(writer >= 0);
    if (!writer) {
        signal(SIGSEGV, SIG_DFL);
        *(volatile unsigned char *)package.common.artifacts[0].data = 0;
        _exit(66);
    }
    int status;
    assert(waitpid(writer, &status, 0) == writer && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    /* Close all inputs while package-owned bytes are still needed. */
    for (size_t i = 0; i < 4; ++i) {
        assert(!munmap(sources[i].data, 4096));
        assert(!close(sources[i].fd));
    }
    assert(((const Elf64_Ehdr *)package.common.artifacts[1].data)->e_type == ET_REL);
    ph_package_close(&package);
    ph_package_close(&package);
    assert(!live_mappings);
}

static void failures(void) {
    setup(0, 17);
    for (int i = 0; i < 8; ++i) { map_fail = i; expect_failure(-ENOMEM); assert(map_fail == -1); }
    for (int i = 0; i < 4; ++i) { protect_fail = i; expect_failure(-EIO); assert(protect_fail == -1); }
    for (int i = 0; i < 4; ++i) { info_fail = i; expect_failure(-ENOMEM); assert(info_fail == -1); }
    wrong_kind = 1; expect_failure(-EINVAL); wrong_kind = 0;
    uint64_t saved_size = bundle.items[0].size;
    bundle.items[0].size = UINT64_MAX; expect_failure(-EFBIG);
    bundle.items[0].size = saved_size;
    identity.generation++; expect_failure(-ESTALE); identity.generation--;
    identity.manifest_digest[0] ^= 1; expect_failure(-EBADMSG); identity.manifest_digest[0] ^= 1;
    identity.grant_digest[0] ^= 1; expect_failure(-EBADMSG); identity.grant_digest[0] ^= 1;
    sources[3].data[0] ^= 1; expect_failure(-EBADMSG); sources[3].data[0] ^= 1;
    bundle.resource_count = 1; bundle.received = 5;
    expect_failure(-EBADMSG); /* No grant binding for an extra native handle. */
    bundle.resource_count = 0; bundle.received = 4;
    finish();
    setup(1, 17); expect_failure(-ENOEXEC); finish();
    setup(0, 16); expect_failure(-ESTALE); finish();
}

static void fatal_rollback(void) {
    setup(0, 17);
    for (int fail_after = 0; fail_after < 2; ++fail_after) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            struct ph_package package = {0};
            fatal_expected = 1;
            if (!fail_after) unmap_fail = 0; /* Source view rollback cannot finish. */
            assert(!ph_package_open(&package, &bundle, &identity, kobox_x86_64_elf_matches,
                fatal_cleanup, &fatal_expected));
            unmap_fail = 0; /* Snapshot close cannot finish. */
            ph_package_close(&package);
            _exit(66);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    }
    finish();
}

int main(void) {
    snapshot_and_protection();
    failures();
    fatal_rollback();
    puts("kobox2 native package snapshot/validation unit: PASS (not a core boot Gate)");
    return 0;
}
