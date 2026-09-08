#pragma once

/* The native LPR is a self-contained ET_DYN, not an interpreter-managed
 * Linux DSO. Relocate its private template before publishing any child maps.
 * Ordinary Linux executables/interpreters still perform their own linking. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static uint64_t lpr_reloc_u64(const unsigned char *p)
{
    uint64_t v; memcpy(&v, p, sizeof(v)); return v;
}

static uint32_t lpr_reloc_u32(const unsigned char *p)
{
    uint32_t v; memcpy(&v, p, sizeof(v)); return v;
}

static int lpr_reloc_range(const unsigned char *phdrs, uint16_t phnum,
    uint16_t phent, uint64_t va, uint64_t size, int writable, int file_backed)
{
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (size_t)i * phent;
        if (lpr_reloc_u32(ph) != 1 ||
            (writable && !(lpr_reloc_u32(ph + 4) & 2))) continue;
        uint64_t start = lpr_reloc_u64(ph + 16);
        uint64_t bytes = lpr_reloc_u64(ph + (file_backed ? 32 : 40));
        if (va >= start && va - start <= bytes && size <= bytes - (va - start))
            return 1;
    }
    return 0;
}

static unsigned char *lpr_reloc_address(unsigned char *mapped, uint64_t size,
    uint64_t span_base, uint64_t bias, uint64_t va, uint64_t bytes)
{
    if (va > UINT64_MAX - bias) return NULL;
    uint64_t target = bias + va;
    if (target < span_base || target - span_base > size ||
        bytes > size - (target - span_base)) return NULL;
    return mapped + (target - span_base);
}

static int lpr_reloc_overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
    return n && m && (a >= b ? a - b < m : b - a < n);
}

static int lpr_exec_relocate_runtime(unsigned char *mapped, uint64_t span_size,
    uint64_t span_base, uint64_t bias, const unsigned char *phdrs,
    uint16_t phnum, uint16_t phent)
{
    if (!mapped || !phdrs || phent < 56) return -8;
    unsigned char *dynamic = NULL;
    uint64_t dynamic_va = 0, dynamic_size = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (size_t)i * phent;
        if (lpr_reloc_u32(ph) != 2) continue;
        if (dynamic) return -8;
        dynamic_va = lpr_reloc_u64(ph + 16);
        dynamic_size = lpr_reloc_u64(ph + 32);
        if (!dynamic_size || dynamic_size % 16 ||
            !lpr_reloc_range(phdrs, phnum, phent, dynamic_va, dynamic_size, 0, 1)) return -8;
        dynamic = lpr_reloc_address(mapped, span_size, span_base, bias, dynamic_va, dynamic_size);
        if (!dynamic) return -8;
    }
    if (!dynamic) return 0;
    uint64_t rela_va = 0, rela_size = 0, rela_ent = 0;
    int ended = 0, seen = 0;
    for (uint64_t off = 0; off < dynamic_size; off += 16) {
        uint64_t tag = lpr_reloc_u64(dynamic + off);
        uint64_t value = lpr_reloc_u64(dynamic + off + 8);
        if (!tag) { ended = 1; break; }
        if (tag == 7) { if (seen & 1) return -8; seen |= 1; rela_va = value; }
        if (tag == 8) { if (seen & 2) return -8; seen |= 2; rela_size = value; }
        if (tag == 9) { if (seen & 4) return -8; seen |= 4; rela_ent = value; }
        /* No symbol resolver, dependencies, REL, PLT or packed RELR contract.
         * Fail closed rather than launch a partially linked native runtime. */
        if (tag == 1 || tag == 17 || tag == 18 || tag == 19 || tag == 20 ||
            tag == 23 || tag == 35 || tag == 36 || tag == 37 || tag == 22 ||
            (tag == 2 && value) || (tag == 30 && (value & 4))) return -8;
    }
    if (!ended) return -8;
    if (!seen) return 0;
    if (seen != 7 || rela_ent != 24 || rela_size % 24 ||
        !lpr_reloc_range(phdrs, phnum, phent, rela_va, rela_size, 0, 1)) return -8;
    unsigned char *rela = lpr_reloc_address(mapped, span_size, span_base, bias, rela_va, rela_size);
    if (!rela) return -8;
    /* Validate the complete table before changing the template. Metadata may
     * not be relocation targets: writes cannot change entries still to apply. */
    for (int apply = 0; apply < 2; ++apply) {
        for (uint64_t off = 0; off < rela_size; off += 24) {
            uint64_t target = lpr_reloc_u64(rela + off);
            uint64_t info = lpr_reloc_u64(rela + off + 8);
            uint64_t addend = lpr_reloc_u64(rela + off + 16);
            if (info == 0) continue;
            if (info != 8 || !lpr_reloc_range(phdrs, phnum, phent, target, 8, 1, 0) ||
                lpr_reloc_overlap(target, 8, dynamic_va, dynamic_size) ||
                lpr_reloc_overlap(target, 8, rela_va, rela_size)) return -8;
            unsigned char *dest = lpr_reloc_address(mapped, span_size, span_base, bias, target, 8);
            uint64_t value;
            if (addend >> 63) {
                uint64_t magnitude = ~addend + 1;
                if (bias < magnitude) return -8;
                value = bias - magnitude;
            } else {
                if (bias > UINT64_MAX - addend) return -8;
                value = bias + addend;
            }
            if (!dest) return -8;
            if (apply) memcpy(dest, &value, sizeof(value));
        }
    }
    return 0;
}
