/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_TTY_H
#define PACHA_INSTALLER_TTY_H

#include <stdio.h>
#include <stdint.h>
#include <termios.h>

struct installer_tty {
    int endpoint_fd;
    int page_fd;
    int notify_fd;
    int timeout_fd;
    void *page;
    uint64_t request_id;
    uint64_t handle;
    struct termios saved_termios;
    int raw_active;
};

/* This is a client of the existing Linux termd PTY, not a new TTY stack. */
int installer_tty_open(struct installer_tty *tty, int termd_fd,
    unsigned pts_index, FILE **output);
int installer_tty_get_size(struct installer_tty *tty,
    unsigned *rows, unsigned *columns);
int installer_tty_enter_ui(struct installer_tty *tty);
int installer_tty_leave_ui(struct installer_tty *tty);
int installer_tty_read_key(void *context, uint64_t timeout_ns);
void installer_tty_close(struct installer_tty *tty);

#endif
