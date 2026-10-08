/* SPDX-License-Identifier: MIT */
#ifndef LPR_FS_SESSION_H
#define LPR_FS_SESSION_H

#include <kobox2_owner/fs_channel.h>

struct lpr_fs_buffer { const void *bytes; size_t length; };

/* Caller owns its input/output until return. Internally each publication has
 * persistent private inventory; an uncertain completion never leaves a stack
 * pointer in the channel. No queue lock is held across notification or wait. */
int lpr_fs_session_call(kb2_fs_request_t *request,
    const struct lpr_fs_buffer *input, unsigned input_count, kb2_fs_response_t *response,
    const struct lpr_fs_buffer *output, unsigned output_count, size_t *used);
int64_t lpr_fs_session_drop(void);
int lpr_fs_session_terminal(void);
void lpr_fs_session_after_fork_child(void);

#endif
