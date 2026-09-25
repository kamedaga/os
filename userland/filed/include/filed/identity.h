#pragma once
#include <stdint.h>

/* Accepted only on the launch manager's endpoint. No ordinary request carries
 * caller credentials. The returned private connection is the authority. */
typedef struct filed_identity {
    uint64_t generation;
    int32_t pid;
    uint32_t uid, gid, euid, egid, suid, sgid;
    uint32_t rights;
} filed_identity_t;
_Static_assert(sizeof(filed_identity_t) == 40, "filed manager identity");

typedef struct filed_identity_update {
    uint64_t client;
    filed_identity_t identity;
} filed_identity_update_t;

struct filed_client {
    struct filed_client *next;
    uint64_t id;
    int fd;
    filed_identity_t identity;
};

/* A lease capability authorizes exactly this handle for a destination client.
 * IDs identify destinations; only calls through the lease grant access. */
#define FILED_LEASE_MAGIC UINT64_C(0x31455341454c4446)
struct filed_runtime;
struct pacha_service_envelope;
int filed_client_create(struct filed_runtime *, const filed_identity_t *, int *, uint64_t *);
int filed_client_credentials(struct filed_runtime *, const filed_identity_update_t *);
void filed_client_release(struct filed_runtime *, struct filed_client *);
int filed_client_authorize(struct filed_runtime *, uint32_t, const void *, uint64_t, uint64_t);
int filed_lease_receive(struct filed_runtime *, uint32_t);
