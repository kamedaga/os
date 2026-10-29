#pragma once

/* Non-audio control tests must not create a PCM session as a side effect. */
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair, uint64_t rights, uint32_t flags)
{ (void)pair; (void)rights; (void)flags; abort(); }
int pacha_ipc_call(pacha_fd_t fd, const struct pacha_ipc_msg *request,
    struct pacha_ipc_msg *reply, uint64_t timeout, int *published)
{ (void)fd; (void)request; (void)reply; (void)timeout; (void)published; abort(); }
