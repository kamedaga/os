/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/usbd/diagnostic.c"

static struct pacha_ipc_msg fragment(unsigned record, unsigned count,
    unsigned index, const char *text, unsigned bytes) {
    struct pacha_ipc_msg message = {.word0 = PACHA_LOG_WIRE_MAGIC,
        .word1 = ((uint64_t)PACHA_LOG_SOURCE_KOBOX2_USB << 56) |
            ((uint64_t)record << 24) | ((uint64_t)count << 16) |
            ((uint64_t)index << 8) | bytes};
    if (bytes <= 16) memcpy(&message.word2, text, bytes);
    return message;
}

int main(void) {
    static struct usbd_diagnostic log;
    struct pacha_ipc_msg first = fragment(1, 2, 0, "0123456789abcdef", 16);
    struct pacha_ipc_msg last = fragment(1, 2, 1, "ERROR", 5);
    accept(&log, &last);
    assert(log.used == 0);
    accept(&log, &first);
    assert(log.used == 22 && !memcmp(log.text, "0123456789abcdefERROR\n", 22));
    size_t used = log.used;
    const struct pacha_ipc_msg invalid[] = {
        fragment(0, 1, 0, "x", 1), fragment(2, 0, 0, "x", 1),
        fragment(2, 17, 0, "x", 1), fragment(2, 1, 1, "x", 1),
        fragment(2, 2, 0, "x", 1), fragment(2, 1, 0, "", 0),
        fragment(2, 1, 0, "", 17),
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) accept(&log, &invalid[i]);
    assert(log.used == used);
    /* Lost fragments never publish partial records or contaminate a later
     * one. Interleaved writers can complete in either order. */
    first = fragment(2, 2, 0, "lost fragment...!", 16);
    accept(&log, &first);
    first = fragment(3, 1, 0, "probe failed", 12);
    accept(&log, &first);
    assert(log.used == used + 13);
    assert(!memcmp(log.text + used, "probe failed\n", 13));
    for (unsigned i = 4; i < 5000; ++i) {
        first = fragment(i, 1, 0, "bounded log line", 16);
        accept(&log, &first);
        assert(log.used <= sizeof(log.text));
    }
    assert(log.dirty && log.text[log.used - 1] == '\n');
    puts("USBD_DIAGNOSTIC_FRAGMENT_VALIDATION_AND_BOUNDS=PASS");
    return 0;
}
