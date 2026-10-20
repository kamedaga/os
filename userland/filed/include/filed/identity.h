#include <pacha/abi.h>
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <kobox2/filesystem.h>

/* These values are supplied only by the trusted launch manager. Linux owns
 * discretionary access checks; filed owns capability and connection policy. */
typedef struct filed_identity {
    uint64_t generation;
    int32_t pid;
    uint32_t uid, gid, euid, egid, suid, sgid, fsuid, fsgid;
    uint32_t rights, securebits, umask;
    uint64_t cap_permitted, cap_effective, cap_inheritable, cap_ambient;
    const uint32_t *groups;
    size_t group_count;
} filed_identity_t;
typedef struct filed_identity_update {
    uint64_t client;
    filed_identity_t identity;
} filed_identity_update_t;
struct filed_client {
    struct filed_client *next;
    uint64_t id, credential_generation, subject_generation;
    pacha_fd_t fd;
    uint32_t rights;
    int32_t pid;
    int retiring, revoked;
};
#define FILED_LEASE_MAGIC UINT64_C(0x31455341454c4446)
struct filed_runtime;
int filed_client_create(struct filed_runtime *, const kb2_fs_request_t *,
                        const uint8_t *, size_t, pacha_fd_t *, uint64_t *);
int filed_client_credentials(struct filed_runtime *, const kb2_fs_request_t *,
                             const uint8_t *, size_t);
int filed_client_release(struct filed_runtime *, struct filed_client *);
