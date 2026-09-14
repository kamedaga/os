/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_GPU_QUERY_H
#define PACHA_KOBOX_GPU_QUERY_H

#include "boot/drm_query.h"
#include "gpu_query_message.h"
#include "lifecycle.h"

/* Sandbox-only native adapter, not linked into the Apache controller. */
struct ph_gpu_query {
    uint64_t generation, session_id, correlation;
    void *mapping;
    void *aux;
    size_t aux_size, aux_mapping_size;
    struct kobox_drm_query plan;
    struct kobox_drm_query_result result;
    struct kobox_drm_query_api api;
    size_t reply_size;
    int terminal_after_completion;
    unsigned char request[PH_GPU_QUERY_PAGE];
    unsigned char output[PH_GPU_QUERY_PAGE];
    unsigned char reply[PH_GPU_QUERY_PAGE];
};

/* session_id may be zero only while a session-aware service is unbound. It
 * must select an authenticated, open session before preparing any command. */
int ph_gpu_query_init(struct ph_gpu_query *query,
                      uint64_t generation,
                      uint64_t session_id,
                      struct ph_lifecycle_service *service);

/* Receiver has already copied the full request into query->request. Only
 * private bytes are decoded. expected_correlation is zero for a ring request
 * whose envelope is authoritative, nonzero for the legacy packet fixture.
 */
int ph_gpu_query_prepare_snapshot(struct ph_gpu_query *query,
                                  size_t size,
                                  uint64_t expected_correlation,
                                  uint32_t queue_class);

/* Drop the private native staging area only after completion publication. */
int ph_gpu_query_release_aux(struct ph_gpu_query *query);
int ph_gpu_query_dispatch(struct ph_gpu_query *query,
                          struct kobox_linux_drm_service *service,
                          uint64_t file_cookie,
                          struct kobox_linux_drm_file *file);
int ph_gpu_query_publish_attachment(struct ph_gpu_query *query,
                                    struct ph_ipc *ipc);

#endif
