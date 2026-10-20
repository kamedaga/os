#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#pragma once

#include <storage/bootstrap.h>
#include <kobox2/filesystem.h>
#include <stddef.h>
#include <stdint.h>

struct filed_storage_owner;
struct ph_fs_channel_config;

int filed_storage_owner_create(pacha_fd_t bootfs_fd, uint64_t bootfs_size,
    const storage_filed_bootstrap_t *bootstrap, struct filed_storage_owner **out);
int filed_storage_owner_start(struct filed_storage_owner *owner);
/* Serialized control-plane calls. IDs are fresh even after detach; caller
 * retains transferred backing/endpoint inventory until a matching release
 * ACK or confirmed sandbox death. Enqueue success alone is not attachment.
 * Config reservation charges backing + worst-case snapshot + ring inventory
 * against 1/4 of immutable sandbox RAM and at most half the native FD table.
 * ENOMEM means the requested lease cannot fit; EAGAIN means live reservations
 * currently consume the budget. Neither error publishes or faults a request. */
int filed_storage_owner_client_config(struct filed_storage_owner *, uint64_t client, uint64_t minimum_arena,
    struct ph_fs_channel_config *);
int filed_storage_owner_client_bind(struct filed_storage_owner *,
    const struct ph_fs_channel_config *, pacha_fd_t vmo_fd, pacha_fd_t doorbell_fd);
int filed_storage_owner_client_release(struct filed_storage_owner *, uint64_t channel_id);
int filed_storage_owner_call(struct filed_storage_owner *owner,
    const uint8_t *request, size_t request_size, uint8_t *response,
    size_t response_capacity, size_t *used);
/* Trusted management and exec readers explicitly choose the owner lane.
 * The opcode alone must never promote an ordinary relayed request: PREAD,
 * STATX and CLOSE also exist in the isolated private exec namespace. */
int filed_storage_owner_control_call(struct filed_storage_owner *owner,
    const uint8_t *request, size_t request_size, uint8_t *response,
    size_t response_capacity, size_t *used);
/* Host-local scatter destinations, not a new sandbox ABI. Payload is copied
 * only after the fixed private header passes matching and bounds checks. */
int filed_storage_owner_call_split(struct filed_storage_owner *,
    const uint8_t *, size_t, kb2_fs_response_t *, void *, size_t, size_t *);
int filed_storage_owner_control_call_split(struct filed_storage_owner *,
    const uint8_t *, size_t, kb2_fs_response_t *, void *, size_t, size_t *);
/* Stops Linux, revokes DMA and proves terminal native WAIT before unmapping.
 * Failure retains this object and its remaining inventory for a retry. */
int filed_storage_owner_destroy(struct filed_storage_owner *owner);
