#include "lpr_error.h"
#include "support/string.h"

#include <pacha/error_conveyor.h>

#include <stddef.h>
#include <stdint.h>

enum {
    LPR_ERRCONV_LAST_FRAMES = 32,
};

static uint64_t lpr_errconv_token = 0x4c50524500000001ull;
static int64_t lpr_errconv_root_status;
static uint64_t lpr_errconv_frame_count;
static uint64_t lpr_errconv_lost_frames;
static pacha_errconv_frame_t lpr_errconv_frames[LPR_ERRCONV_LAST_FRAMES];

static void lpr_errconv_copy_text(char dst[PACHA_ERRCONV_TEXT_BYTES], const char *text)
{
    lpr_memset(dst, 0, PACHA_ERRCONV_TEXT_BYTES);
    if (text == 0) {
        return;
    }
    for (size_t i = 0; i + 1u < PACHA_ERRCONV_TEXT_BYTES && text[i] != '\0'; ++i) {
        dst[i] = text[i];
    }
}

static void lpr_errconv_reset(int64_t root_status)
{
    lpr_errconv_token++;
    if (lpr_errconv_token == 0) {
        lpr_errconv_token = 0x4c50524500000001ull;
    }
    lpr_errconv_root_status = root_status;
    lpr_errconv_frame_count = 0;
    lpr_errconv_lost_frames = 0;
    lpr_memset(lpr_errconv_frames, 0, sizeof(lpr_errconv_frames));
}

void lpr_errconv_record(
    uint64_t domain,
    uint64_t op,
    uint64_t stage,
    int64_t status,
    int64_t raw_status,
    uint64_t request_id,
    uint64_t fd_count,
    uint64_t subject,
    uint64_t child_token,
    const char *text)
{
    if (lpr_errconv_frame_count == 0) {
        lpr_errconv_reset(status);
    }
    if (lpr_errconv_frame_count >= LPR_ERRCONV_LAST_FRAMES) {
        lpr_errconv_lost_frames++;
        return;
    }
    pacha_errconv_frame_t *frame = &lpr_errconv_frames[lpr_errconv_frame_count++];
    lpr_memset(frame, 0, sizeof(*frame));
    frame->domain = domain;
    frame->component = PACHA_ERRCONV_COMPONENT_LPR_RUNTIME;
    frame->op = op;
    frame->stage = stage;
    frame->status = status;
    frame->raw_status = raw_status;
    frame->request_id = request_id;
    frame->fd_count = fd_count;
    frame->subject = subject;
    frame->child_token = child_token;
    lpr_errconv_copy_text(frame->text, text);
}
