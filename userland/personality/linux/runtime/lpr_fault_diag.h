#ifndef LPR_FAULT_DIAG_H
#define LPR_FAULT_DIAG_H
#include <stddef.h>
#include <stdint.h>

/* Kept independent of the runtime so host tests exercise the exact formatter. */
struct lpr_fault_diag {
    uint64_t sig, vector, error, address, rip, rsp;
    uint64_t linux_pid, linux_tid, native_pid, native_tid;
    uint64_t map_start, map_offset;
    const char *file, *comm;
    unsigned async, mapped, byte_count;
    unsigned char bytes[16];
};

static inline size_t lpr_fault_byte_limit(uint64_t rip)
{
    size_t n = 4096u - (size_t)(rip & 4095u);
    return n < 16u ? n : 16u;
}

struct lpr_fault_line { char *data; size_t length, capacity; };
static inline void lpr_fault_text(struct lpr_fault_line *b, const char *s, size_t max)
{
    while (s && max-- && *s && b->length + 1 < b->capacity) {
        unsigned char c = (unsigned char)*s++;
        /* A pathname must never inject a second log line or field. */
        b->data[b->length++] = c <= 32 || c >= 127 ? '?' : (char)c;
    }
}
static inline void lpr_fault_literal(struct lpr_fault_line *b, const char *s)
{
    while (*s && b->length + 1 < b->capacity) b->data[b->length++] = *s++;
}
static inline void lpr_fault_number(struct lpr_fault_line *b, uint64_t v, unsigned base)
{
    char digits[20];
    size_t n = 0;
    do { digits[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (n && b->length + 1 < b->capacity) b->data[b->length++] = digits[--n];
}
static inline void lpr_fault_field(struct lpr_fault_line *b, const char *key,
    uint64_t value, unsigned base)
{
    lpr_fault_literal(b, key);
    if (base == 16) lpr_fault_literal(b, "0x");
    lpr_fault_number(b, value, base);
}
static inline size_t lpr_fault_format(char *line, size_t capacity,
    const struct lpr_fault_diag *d)
{
    struct lpr_fault_line b = {line, 0, capacity};
    lpr_fault_literal(&b, d->async ? "[lpr-fault] src=async" : "[lpr-fault] src=fault");
    lpr_fault_field(&b, " sig=", d->sig, 10);
    if (!d->async) {
        lpr_fault_field(&b, " vector=", d->vector, 10);
        lpr_fault_field(&b, " err=", d->error, 16);
        lpr_fault_field(&b, " address=", d->address, 16);
        if (d->vector == 14) lpr_fault_field(&b, " cr2=", d->address, 16);
        else lpr_fault_literal(&b, " cr2=NA");
    } else lpr_fault_literal(&b, " sender=unknown");
    lpr_fault_field(&b, " rip=", d->rip, 16);
    lpr_fault_field(&b, " rsp=", d->rsp, 16);
    lpr_fault_field(&b, " linux_pid=", d->linux_pid, 10);
    lpr_fault_field(&b, " linux_tid=", d->linux_tid, 10);
    lpr_fault_field(&b, " native_pid=", d->native_pid, 10);
    lpr_fault_field(&b, " native_tid=", d->native_tid, 10);
    lpr_fault_literal(&b, " comm=");
    lpr_fault_text(&b, d->comm ? d->comm : "unknown", 32);
    lpr_fault_literal(&b, " file=");
    lpr_fault_text(&b, d->file ? d->file : "unknown", 192);
    if (d->mapped) {
        lpr_fault_field(&b, " map_start=", d->map_start, 16);
        lpr_fault_field(&b, " offset=", d->map_offset, 16);
        lpr_fault_literal(&b, d->file ? " offset_kind=file" : " offset_kind=vmo");
    } else lpr_fault_literal(&b, " offset=NA");
    if (!d->async && d->vector == 6) {
        lpr_fault_literal(&b, " bytes=");
        unsigned count = d->byte_count;
        if (count > lpr_fault_byte_limit(d->rip)) count = (unsigned)lpr_fault_byte_limit(d->rip);
        if (!count) lpr_fault_literal(&b, "unavailable");
        for (unsigned i = 0; i < count; ++i) {
            char hex[3] = {"0123456789abcdef"[d->bytes[i] >> 4],
                "0123456789abcdef"[d->bytes[i] & 15], 0};
            lpr_fault_literal(&b, hex);
        }
    }
    if (capacity) line[b.length++] = '\n';
    return b.length;
}

/* Normal mapping/bootstrap paths publish immutable diagnostic snapshots.
 * Fault readers never wait for a writer or borrow a reclaimable pointer. */
uint64_t lpr_fault_map_begin(uint64_t start, uint64_t length);
void lpr_fault_map_end(void);
void lpr_fault_map_record(uint64_t start, uint64_t length, uint64_t offset, uint64_t handle, uint64_t epoch);
void lpr_fault_comm_record(const char *comm);
#endif
