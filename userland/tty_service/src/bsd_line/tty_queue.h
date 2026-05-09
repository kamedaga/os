/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from FreeBSD sys/sys/ttyqueue.h.
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

#ifndef CAPABILITYOS_TTY_BSD_LINE_TTY_QUEUE_H
#define CAPABILITYOS_TTY_BSD_LINE_TTY_QUEUE_H

enum {
    BSD_TTYINQ_DATASIZE = 128,
    BSD_TTYINQ_BLOCKS = TTY_INPUT_RING_BYTES / BSD_TTYINQ_DATASIZE,
    BSD_TTYINQ_QUOTE_WORDS = BSD_TTYINQ_DATASIZE / 32,
};

struct bsd_ttyinq_block {
    u32 quotes[BSD_TTYINQ_QUOTE_WORDS];
    u8 data[BSD_TTYINQ_DATASIZE];
};

struct bsd_ttyinq {
    struct bsd_ttyinq_block blocks[BSD_TTYINQ_BLOCKS];
    u64 begin;
    u64 linestart;
    u64 reprint;
    u64 end;
};

static void bsd_ttyinq_init(struct bsd_ttyinq *inq);
static void bsd_ttyinq_reset(struct bsd_ttyinq *inq);
static u64 bsd_ttyinq_capacity(void);
static u64 bsd_ttyinq_bytes_used(const struct bsd_ttyinq *inq);
static u64 bsd_ttyinq_bytes_canonicalized(const struct bsd_ttyinq *inq);
static void bsd_ttyinq_canonicalize(struct bsd_ttyinq *inq);
static int bsd_ttyinq_unput(struct bsd_ttyinq *inq, u8 *byte_out);
static int bsd_ttyinq_peek_last(const struct bsd_ttyinq *inq, u8 *byte_out);
static void bsd_ttyinq_write_byte(struct bsd_ttyinq *inq, u8 byte, int quote);
static u64 bsd_ttyinq_read(struct bsd_ttyinq *inq, volatile u8 *dst, u64 max_len);
static u64 bsd_ttyinq_read_canonical(struct bsd_ttyinq *inq, volatile u8 *dst, u64 max_len);

#endif
