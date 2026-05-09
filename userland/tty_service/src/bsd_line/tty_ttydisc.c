/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from FreeBSD sys/kern/tty_ttydisc.c.
 *
 * Copyright (c) 2008 Ed Schouten <ed@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

enum {
    TTY_IFLAG_ICRNL = 1 << 0,
    TTY_IFLAG_INLCR = 1 << 1,
    TTY_IFLAG_IGNCR = 1 << 2,
    TTY_OFLAG_OPOST = 1 << 0,
    TTY_OFLAG_ONLCR = 1 << 1,
    TTY_LFLAG_ISIG = 1 << 0,
    TTY_LFLAG_ICANON = 1 << 1,
    TTY_LFLAG_ECHO = 1 << 2,
    TTY_LFLAG_ECHOE = 1 << 3,
    TTY_LFLAG_ECHOK = 1 << 4,
    TTY_LFLAG_ECHONL = 1 << 5,
    TTY_LFLAG_IEXTEN = 1 << 6,
    TTY_CC_COUNT = 32,
    TTY_CC_VINTR = 0,
    TTY_CC_VQUIT = 1,
    TTY_CC_VERASE = 2,
    TTY_CC_VKILL = 3,
    TTY_CC_VEOF = 4,
    TTY_CC_VTIME = 5,
    TTY_CC_VMIN = 6,
    TTY_CC_VSTART = 8,
    TTY_CC_VSTOP = 9,
    TTY_CC_VSUSP = 10,
    TTY_CC_VEOL = 11,
    TTY_CC_VREPRINT = 12,
    TTY_CC_VWERASE = 14,
    TTY_SIGINT = 2,
    TTY_SIGQUIT = 3,
    TTY_SIGTSTP = 20,
};

struct bsd_tty_termios_state {
    u32 iflag;
    u32 oflag;
    u32 lflag;
    u8 cc[TTY_CC_COUNT];
    u16 columns;
    u16 rows;
};

struct bsd_tty {
    struct bsd_tty_termios_state termios;
    struct bsd_ttyinq inq;
    int input_prev_was_cr;
    int output_prev_was_cr;
    int eof_pending;
    u8 pending_signal;
    char echo_line[TTY_INPUT_RING_BYTES];
    u64 echo_line_len;
};

static void bsd_ttydisc_init_defaults(struct bsd_tty *tp) {
    bsd_ttyinq_init(&tp->inq);
    tp->termios.iflag = TTY_IFLAG_ICRNL;
    tp->termios.oflag = TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR;
    tp->termios.lflag = TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO |
        TTY_LFLAG_ECHOE | TTY_LFLAG_ECHOK | TTY_LFLAG_IEXTEN;
    for (u64 i = 0; i < sizeof(tp->termios.cc); i++) tp->termios.cc[i] = 0;
    tp->termios.cc[TTY_CC_VINTR] = 3;
    tp->termios.cc[TTY_CC_VQUIT] = 28;
    tp->termios.cc[TTY_CC_VERASE] = 127;
    tp->termios.cc[TTY_CC_VKILL] = 21;
    tp->termios.cc[TTY_CC_VEOF] = 4;
    tp->termios.cc[TTY_CC_VTIME] = 0;
    tp->termios.cc[TTY_CC_VMIN] = 1;
    tp->termios.cc[TTY_CC_VSTART] = 17;
    tp->termios.cc[TTY_CC_VSTOP] = 19;
    tp->termios.cc[TTY_CC_VSUSP] = 26;
    tp->termios.cc[TTY_CC_VREPRINT] = 18;
    tp->termios.cc[TTY_CC_VWERASE] = 23;
    tp->termios.columns = 120;
    tp->termios.rows = 40;
    tp->input_prev_was_cr = 0;
    tp->output_prev_was_cr = 0;
    tp->eof_pending = 0;
    tp->pending_signal = 0;
    tp->echo_line_len = 0;
}

static void bsd_ttydisc_reset_session(struct bsd_tty *tp) {
    tp->input_prev_was_cr = 0;
    tp->output_prev_was_cr = 0;
    tp->eof_pending = 0;
    tp->pending_signal = 0;
    tp->echo_line_len = 0;
    bsd_ttyinq_reset(&tp->inq);
}

static int bsd_ttydisc_is_cc(const struct bsd_tty *tp, u64 index, u8 byte) {
    return index < TTY_CC_COUNT && tp->termios.cc[index] != 0 && tp->termios.cc[index] == byte;
}

static u64 bsd_ttydisc_output(struct bsd_tty *tp, const char *bytes, u64 len) {
    char out[256];
    u64 done = 0;
    u64 out_len = 0;
    const int onlcr = (tp->termios.oflag & (TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR)) == (TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR);

    while (done < len) {
        const char b = bytes[done];
        if (onlcr && b == '\n' && !tp->output_prev_was_cr) {
            if (out_len == sizeof(out)) {
                if (backend_write_bytes(out, out_len) != out_len) return done;
                out_len = 0;
            }
            out[out_len++] = '\r';
        }
        if (out_len == sizeof(out)) {
            if (backend_write_bytes(out, out_len) != out_len) return done;
            out_len = 0;
        }
        out[out_len++] = b;
        tp->output_prev_was_cr = b == '\r';
        done++;
    }
    if (out_len != 0 && backend_write_bytes(out, out_len) != out_len) return done;
    return done;
}

static void bsd_ttydisc_echo_push(struct bsd_tty *tp, u8 byte) {
    if ((tp->termios.lflag & TTY_LFLAG_ECHO) == 0 &&
        ((tp->termios.lflag & TTY_LFLAG_ECHONL) == 0 || byte != '\n')) {
        return;
    }
    if (tp->echo_line_len < sizeof(tp->echo_line)) tp->echo_line[tp->echo_line_len++] = (char)byte;
}

static void bsd_ttydisc_echo_flush(struct bsd_tty *tp) {
    if (tp->echo_line_len == 0) return;
    (void)bsd_ttydisc_output(tp, tp->echo_line, tp->echo_line_len);
    tp->echo_line_len = 0;
}

static void bsd_ttydisc_echo_erase(struct bsd_tty *tp) {
    if ((tp->termios.lflag & (TTY_LFLAG_ECHO | TTY_LFLAG_ECHOE)) != (TTY_LFLAG_ECHO | TTY_LFLAG_ECHOE)) return;
    if (tp->echo_line_len != 0) {
        tp->echo_line_len--;
        return;
    }
    (void)bsd_ttydisc_output(tp, "\b \b", 3);
}

static void bsd_ttydisc_rubword(struct bsd_tty *tp) {
    u8 c = 0;
    while (bsd_ttyinq_peek_last(&tp->inq, &c) && (c == ' ' || c == '\t')) {
        (void)bsd_ttyinq_unput(&tp->inq, &c);
        bsd_ttydisc_echo_erase(tp);
    }
    while (bsd_ttyinq_peek_last(&tp->inq, &c) && c != ' ' && c != '\t') {
        (void)bsd_ttyinq_unput(&tp->inq, &c);
        bsd_ttydisc_echo_erase(tp);
    }
}

static void bsd_ttydisc_rubline(struct bsd_tty *tp) {
    u8 c = 0;
    while (bsd_ttyinq_unput(&tp->inq, &c)) {
        (void)c;
        bsd_ttydisc_echo_erase(tp);
    }
}

static void bsd_ttydisc_queue_signal(struct bsd_tty *tp, u8 signo, const char *echo, u64 echo_len) {
    tp->pending_signal = signo;
    bsd_ttydisc_rubline(tp);
    tp->echo_line_len = 0;
    if (echo_len != 0) (void)bsd_ttydisc_output(tp, echo, echo_len);
}

static void bsd_ttydisc_rint(struct bsd_tty *tp, u8 byte) {
    const int canonical = (tp->termios.lflag & TTY_LFLAG_ICANON) != 0;
    if (byte == 0) return;

    if ((tp->termios.lflag & TTY_LFLAG_ISIG) != 0) {
        if (bsd_ttydisc_is_cc(tp, TTY_CC_VINTR, byte)) {
            bsd_ttydisc_queue_signal(tp, TTY_SIGINT, "^C\n", 3);
            return;
        }
        if (bsd_ttydisc_is_cc(tp, TTY_CC_VQUIT, byte)) {
            bsd_ttydisc_queue_signal(tp, TTY_SIGQUIT, "^\\\n", 3);
            return;
        }
        if (bsd_ttydisc_is_cc(tp, TTY_CC_VSUSP, byte)) {
            bsd_ttydisc_queue_signal(tp, TTY_SIGTSTP, "^Z\n", 3);
            return;
        }
    }

    if (canonical && bsd_ttydisc_is_cc(tp, TTY_CC_VERASE, byte)) {
        u8 erased = 0;
        if (bsd_ttyinq_unput(&tp->inq, &erased)) {
            (void)erased;
            bsd_ttydisc_echo_erase(tp);
        }
        return;
    }
    if (canonical && bsd_ttydisc_is_cc(tp, TTY_CC_VKILL, byte)) {
        bsd_ttydisc_rubline(tp);
        tp->echo_line_len = 0;
        return;
    }
    if (canonical && (tp->termios.lflag & TTY_LFLAG_IEXTEN) && bsd_ttydisc_is_cc(tp, TTY_CC_VWERASE, byte)) {
        bsd_ttydisc_rubword(tp);
        return;
    }
    if (canonical && bsd_ttydisc_is_cc(tp, TTY_CC_VEOF, byte)) {
        if (bsd_ttyinq_bytes_used(&tp->inq) == 0) {
            tp->eof_pending = 1;
        } else {
            bsd_ttyinq_canonicalize(&tp->inq);
        }
        return;
    }

    if (byte == '\r') {
        if (tp->termios.iflag & TTY_IFLAG_IGNCR) return;
        if (tp->termios.iflag & TTY_IFLAG_ICRNL) byte = '\n';
        tp->input_prev_was_cr = 1;
    } else if (byte == '\n') {
        if ((tp->termios.iflag & TTY_IFLAG_ICRNL) && tp->input_prev_was_cr) {
            tp->input_prev_was_cr = 0;
            return;
        }
        if (tp->termios.iflag & TTY_IFLAG_INLCR) byte = '\r';
        tp->input_prev_was_cr = 0;
    } else {
        tp->input_prev_was_cr = 0;
    }

    bsd_ttyinq_write_byte(&tp->inq, byte, 0);
    bsd_ttydisc_echo_push(tp, byte);
    bsd_ttydisc_echo_flush(tp);
}

static void bsd_ttydisc_rint_bypass(struct bsd_tty *tp, volatile u8 *raw, u64 raw_len) {
    for (u64 i = 0; i < raw_len; i++) bsd_ttydisc_rint(tp, raw[i]);
}

static int bsd_ttydisc_readable(const struct bsd_tty *tp) {
    const int canonical = (tp->termios.lflag & TTY_LFLAG_ICANON) != 0;
    if (canonical) return bsd_ttyinq_bytes_canonicalized(&tp->inq) != 0 || tp->eof_pending;
    return bsd_ttyinq_bytes_used(&tp->inq) != 0;
}

static u8 bsd_ttydisc_peek_signal(const struct bsd_tty *tp) {
    return tp->pending_signal;
}

static u8 bsd_ttydisc_take_signal(struct bsd_tty *tp) {
    const u8 signo = tp->pending_signal;
    tp->pending_signal = 0;
    return signo;
}

static u64 bsd_ttydisc_read(struct bsd_tty *tp, volatile u8 *dst, u64 max_len) {
    const int canonical = (tp->termios.lflag & TTY_LFLAG_ICANON) != 0;
    if (canonical) {
        if (bsd_ttyinq_bytes_canonicalized(&tp->inq) == 0 && tp->eof_pending) {
            tp->eof_pending = 0;
            return 0;
        }
        return bsd_ttyinq_read_canonical(&tp->inq, dst, max_len);
    }
    return bsd_ttyinq_read(&tp->inq, dst, max_len);
}

static u64 bsd_ttydisc_write(struct bsd_tty *tp, volatile u8 *payload, u64 len) {
    char buf[256];
    u64 done = 0;
    while (done < len) {
        const u64 chunk = min_u64(len - done, sizeof(buf));
        for (u64 i = 0; i < chunk; i++) buf[i] = (char)payload[done + i];
        const u64 written = bsd_ttydisc_output(tp, buf, chunk);
        if (written != chunk) break;
        done += chunk;
    }
    return done;
}
