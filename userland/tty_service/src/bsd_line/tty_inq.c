/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from FreeBSD sys/kern/tty_inq.c.
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

static u64 bsd_ttyinq_capacity(void) {
    return (u64)BSD_TTYINQ_BLOCKS * BSD_TTYINQ_DATASIZE;
}

static void bsd_ttyinq_zero_quotes(struct bsd_ttyinq_block *block) {
    for (u64 i = 0; i < BSD_TTYINQ_QUOTE_WORDS; i++) block->quotes[i] = 0;
}

static void bsd_ttyinq_set_quote(struct bsd_ttyinq_block *block, u64 offset, int quote) {
    const u32 bit = (u32)(1u << (offset % 32));
    const u64 word = offset / 32;
    if (quote) {
        block->quotes[word] |= bit;
    } else {
        block->quotes[word] &= ~bit;
    }
}

static void bsd_ttyinq_init(struct bsd_ttyinq *inq) {
    for (u64 i = 0; i < BSD_TTYINQ_BLOCKS; i++) bsd_ttyinq_zero_quotes(&inq->blocks[i]);
    bsd_ttyinq_reset(inq);
}

static void bsd_ttyinq_reset(struct bsd_ttyinq *inq) {
    inq->begin = 0;
    inq->linestart = 0;
    inq->reprint = 0;
    inq->end = 0;
    for (u64 i = 0; i < BSD_TTYINQ_BLOCKS; i++) bsd_ttyinq_zero_quotes(&inq->blocks[i]);
}

static u64 bsd_ttyinq_bytes_used(const struct bsd_ttyinq *inq) {
    return inq->end - inq->begin;
}

static u64 bsd_ttyinq_bytes_canonicalized(const struct bsd_ttyinq *inq) {
    return inq->linestart - inq->begin;
}

static void bsd_ttyinq_drop_oldest(struct bsd_ttyinq *inq) {
    if (bsd_ttyinq_bytes_used(inq) == 0) return;
    inq->begin++;
    if (inq->linestart < inq->begin) inq->linestart = inq->begin;
    if (inq->reprint < inq->begin) inq->reprint = inq->begin;
}

static void bsd_ttyinq_write_byte(struct bsd_ttyinq *inq, u8 byte, int quote) {
    if (bsd_ttyinq_bytes_used(inq) >= bsd_ttyinq_capacity()) bsd_ttyinq_drop_oldest(inq);

    const u64 pos = inq->end % bsd_ttyinq_capacity();
    const u64 block_index = pos / BSD_TTYINQ_DATASIZE;
    const u64 block_offset = pos % BSD_TTYINQ_DATASIZE;
    struct bsd_ttyinq_block *block = &inq->blocks[block_index];
    block->data[block_offset] = byte;
    bsd_ttyinq_set_quote(block, block_offset, quote);
    inq->end++;

    if (!quote && byte == '\n') {
        inq->linestart = inq->end;
        inq->reprint = inq->end;
    }
}

static void bsd_ttyinq_canonicalize(struct bsd_ttyinq *inq) {
    inq->linestart = inq->end;
    inq->reprint = inq->end;
}

static int bsd_ttyinq_unput(struct bsd_ttyinq *inq, u8 *byte_out) {
    if (inq->end == inq->linestart) return 0;
    inq->end--;
    const u64 pos = inq->end % bsd_ttyinq_capacity();
    const u64 block_index = pos / BSD_TTYINQ_DATASIZE;
    const u64 block_offset = pos % BSD_TTYINQ_DATASIZE;
    *byte_out = inq->blocks[block_index].data[block_offset];
    bsd_ttyinq_set_quote(&inq->blocks[block_index], block_offset, 0);
    if (inq->reprint > inq->end) inq->reprint = inq->end;
    return 1;
}

static int bsd_ttyinq_peek_last(const struct bsd_ttyinq *inq, u8 *byte_out) {
    if (inq->end == inq->linestart) return 0;
    const u64 pos = (inq->end - 1) % bsd_ttyinq_capacity();
    const u64 block_index = pos / BSD_TTYINQ_DATASIZE;
    const u64 block_offset = pos % BSD_TTYINQ_DATASIZE;
    *byte_out = inq->blocks[block_index].data[block_offset];
    return 1;
}

static u64 bsd_ttyinq_read(struct bsd_ttyinq *inq, volatile u8 *dst, u64 max_len) {
    const u64 used = bsd_ttyinq_bytes_used(inq);
    const u64 n = min_u64(used, max_len);
    for (u64 i = 0; i < n; i++) {
        const u64 pos = (inq->begin + i) % bsd_ttyinq_capacity();
        const u64 block_index = pos / BSD_TTYINQ_DATASIZE;
        const u64 block_offset = pos % BSD_TTYINQ_DATASIZE;
        dst[i] = inq->blocks[block_index].data[block_offset];
    }
    inq->begin += n;
    if (inq->begin == inq->end) {
        bsd_ttyinq_reset(inq);
    } else {
        if (inq->linestart < inq->begin) inq->linestart = inq->begin;
        if (inq->reprint < inq->begin) inq->reprint = inq->begin;
    }
    return n;
}

static u64 bsd_ttyinq_read_canonical(struct bsd_ttyinq *inq, volatile u8 *dst, u64 max_len) {
    const u64 available = bsd_ttyinq_bytes_canonicalized(inq);
    const u64 n = min_u64(available, max_len);
    for (u64 i = 0; i < n; i++) {
        const u64 pos = (inq->begin + i) % bsd_ttyinq_capacity();
        const u64 block_index = pos / BSD_TTYINQ_DATASIZE;
        const u64 block_offset = pos % BSD_TTYINQ_DATASIZE;
        dst[i] = inq->blocks[block_index].data[block_offset];
    }
    inq->begin += n;
    if (inq->begin == inq->end) {
        bsd_ttyinq_reset(inq);
    } else {
        if (inq->linestart < inq->begin) inq->linestart = inq->begin;
        if (inq->reprint < inq->begin) inq->reprint = inq->begin;
    }
    return n;
}
