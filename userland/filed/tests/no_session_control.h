/* SPDX-License-Identifier: MIT */
/* These tests exercise non-session control. Entering the new connection API
 * here is a regression, not a successful fake direct-queue installation. */
int filed_storage_owner_client_config(struct filed_storage_owner *owner, uint64_t client,
    uint64_t minimum, struct ph_fs_channel_config *config)
{ (void)owner; (void)client; (void)minimum; (void)config; abort(); }
int filed_storage_owner_client_bind(struct filed_storage_owner *owner,
    const struct ph_fs_channel_config *config, pacha_fd_t vmo, pacha_fd_t doorbell)
{ (void)owner; (void)config; (void)vmo; (void)doorbell; abort(); }
int filed_storage_owner_client_release(struct filed_storage_owner *owner, uint64_t id)
{ (void)owner; (void)id; abort(); }
int filed_storage_device_route(struct filed_runtime *rt,kb2_fs_request_t *r,
    const void *tail,size_t size,filed_storage_stat_t *stat,uint64_t *handle,uint8_t *wire)
{ (void)rt;(void)r;(void)tail;(void)size;(void)stat;(void)handle;(void)wire;abort(); }
