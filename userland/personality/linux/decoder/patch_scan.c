#include "patch_scan.h"

#include <Zydis/Zydis.h>
#include <personality/zpoline.h>

#define LPR_RESYNC_WINDOW_BYTES 256u
#define LPR_SITE_PLAN_BYTES 256u
#define LPR_SITE_PLAN_BITS (LPR_SITE_PLAN_BYTES * 8u)

typedef enum lpr_candidate_class {
    LPR_CANDIDATE_INTERIOR,
    LPR_CANDIDATE_SITE,
    LPR_CANDIDATE_AMBIGUOUS,
} lpr_candidate_class_t;

static int lpr_decode(const ZydisDecoder *decoder, const uint8_t *bytes,
                      uint64_t size, uint64_t pos,
                      ZydisDecodedInstruction *instruction)
{
    if (decoder == 0 || bytes == 0 || instruction == 0 || pos >= size) return 0;
    if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(
            decoder, ZYAN_NULL, bytes + pos, size - pos, instruction)) ||
        instruction->length == 0 || instruction->length > size - pos)
    {
        return 0;
    }
    return (int)instruction->length;
}
static int lpr_is_syscall(const ZydisDecodedInstruction *instruction)
{
    return instruction->mnemonic == ZYDIS_MNEMONIC_SYSCALL;
}

static int lpr_is_raw_candidate(const uint8_t *bytes, uint64_t size, uint64_t pos)
{
    return pos + 1u < size && bytes[pos] == LPR_ZPOLINE_PATCH_FROM0 &&
           bytes[pos + 1u] == LPR_ZPOLINE_PATCH_FROM1;
}

typedef uint64_t lpr_unaligned_u64
    __attribute__((__aligned__(1), __may_alias__));

/* This is only a byte prefilter. A match is never patched until Zydis proves
 * that the bytes terminate a decoded SYSCALL instruction. */
static uint64_t lpr_find_raw_candidate(const uint8_t *bytes, uint64_t size,
                                       uint64_t start)
{
    const uint64_t ones = UINT64_C(0x0101010101010101);
    const uint64_t highs = UINT64_C(0x8080808080808080);
    const uint64_t needle = UINT64_C(0x0f0f0f0f0f0f0f0f);
    uint64_t pos = start;
    while (pos <= size && size - pos >= sizeof(uint64_t)) {
        const uint64_t word =
            *(const lpr_unaligned_u64 *)(const void *)(bytes + pos);
        const uint64_t different = word ^ needle;
        if (((different - ones) & ~different & highs) == 0) {
            pos += sizeof(uint64_t);
            continue;
        }
        for (uint64_t lane = 0; lane < sizeof(uint64_t); ++lane) {
            if (lpr_is_raw_candidate(bytes, size, pos + lane)) {
                return pos + lane;
            }
        }
        pos += sizeof(uint64_t);
    }
    while (pos + 1u < size) {
        if (lpr_is_raw_candidate(bytes, size, pos)) return pos;
        pos++;
    }
    return UINT64_MAX;
}

static lpr_candidate_class_t lpr_classify_candidate(
    const ZydisDecoder *decoder, const uint8_t *bytes, uint64_t size,
    uint64_t candidate, uint64_t *decoded_bytes)
{
    /* The mapping might not begin at an instruction boundary. For contiguous
     * code, one of the first 16 offsets in this window is a genuine boundary
     * because an x86-64 instruction is at most 15 bytes. A path that reaches
     * an invalid encoding is not a surviving instruction stream. Accept a
     * site only when every path that reaches the candidate agrees. */
    const uint64_t window = candidate > LPR_RESYNC_WINDOW_BYTES
        ? candidate - LPR_RESYNC_WINDOW_BYTES
        : 0;
    const uint64_t last_start = window == 0 ? 0 : window + 15u;
    uint64_t site_paths = 0;
    uint64_t interior_paths = 0;
    for (uint64_t start = window; start <= last_start && start <= candidate; ++start) {
        uint64_t pos = start;
        while (pos < candidate + 2u) {
            ZydisDecodedInstruction instruction;
            const int instruction_length = lpr_decode(
                decoder, bytes, size, pos, &instruction);
            if (instruction_length <= 0) break;
            if (decoded_bytes != 0) {
                const uint64_t length = (uint64_t)instruction_length;
                *decoded_bytes = *decoded_bytes > UINT64_MAX - length
                    ? UINT64_MAX
                    : *decoded_bytes + length;
            }
            const uint64_t end = pos + (uint64_t)instruction_length;
            if (end <= candidate) {
                pos = end;
                continue;
            }
            if (end == candidate + 2u && lpr_is_syscall(&instruction) &&
                bytes[end - 2u] == LPR_ZPOLINE_PATCH_FROM0 &&
                bytes[end - 1u] == LPR_ZPOLINE_PATCH_FROM1)
            {
                site_paths++;
            } else {
                interior_paths++;
            }
            break;
        }
    }
    if (site_paths != 0 && interior_paths == 0) return LPR_CANDIDATE_SITE;
    if (site_paths == 0 && interior_paths != 0) return LPR_CANDIDATE_INTERIOR;
    return LPR_CANDIDATE_AMBIGUOUS;
}

static int lpr_site_has_decodable_successor(const ZydisDecoder *decoder,
                                            const uint8_t *bytes,
                                            uint64_t size, uint64_t opcode)
{
    ZydisDecodedInstruction next;
    return opcode + 2u < size &&
        lpr_decode(decoder, bytes, size, opcode + 2u, &next) > 0;
}

static void lpr_plan_set(uint8_t plan[LPR_SITE_PLAN_BYTES], uint64_t index)
{
    plan[index >> 3] |= (uint8_t)(1u << (index & 7u));
}

static int lpr_plan_get(const uint8_t plan[LPR_SITE_PLAN_BYTES], uint64_t index)
{
    return (plan[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0;
}

static uint64_t lpr_count_raw_candidates(const uint8_t *bytes, uint64_t size)
{
    uint64_t count = 0;
    uint64_t search = 0;
    for (;;) {
        const uint64_t candidate = lpr_find_raw_candidate(bytes, size, search);
        if (candidate == UINT64_MAX) return count;
        count++;
        search = candidate + 2u;
    }
}

/* Build a complete plan before changing bytes. This keeps a decode failure or
 * ambiguous candidate transactional even though LPR has no allocator. The
 * small plan covers normal Linux images; larger inputs use a verified second
 * pass rather than acquiring a fixed capacity limit. */
static int lpr_plan_local(const ZydisDecoder *decoder, const uint8_t *bytes,
                          uint64_t size, uint8_t *plan,
                          uint64_t *out_skipped)
{
    uint64_t candidate_index = 0;
    uint64_t search = 0;
    *out_skipped = 0;
    for (;;) {
        const uint64_t candidate = lpr_find_raw_candidate(bytes, size, search);
        if (candidate == UINT64_MAX) return 0;
        const lpr_candidate_class_t classification = lpr_classify_candidate(
            decoder, bytes, size, candidate, 0);
        if (classification == LPR_CANDIDATE_AMBIGUOUS) return -1;
        if (classification == LPR_CANDIDATE_SITE) {
            if (lpr_site_has_decodable_successor(decoder, bytes, size, candidate)) {
                if (plan != 0) lpr_plan_set(plan, candidate_index);
            } else {
                (*out_skipped)++;
            }
        }
        candidate_index++;
        search = candidate + 2u;
    }
}

static int lpr_plan_full(const ZydisDecoder *decoder, const uint8_t *bytes,
                         uint64_t size, uint8_t *plan,
                         uint64_t *out_skipped)
{
    uint64_t candidate = lpr_find_raw_candidate(bytes, size, 0);
    uint64_t candidate_index = 0;
    uint64_t pending_site = UINT64_MAX;
    *out_skipped = 0;
    for (uint64_t pos = 0; pos < size;) {
        ZydisDecodedInstruction instruction;
        const int decoded = lpr_decode(decoder, bytes, size, pos, &instruction);
        if (decoded <= 0) return -1;
        if (pending_site != UINT64_MAX) {
            if (plan != 0) lpr_plan_set(plan, pending_site);
            pending_site = UINT64_MAX;
        }
        const uint64_t end = pos + (uint64_t)decoded;
        while (candidate != UINT64_MAX && candidate < end) {
            if (candidate >= pos && candidate + 2u == end &&
                lpr_is_syscall(&instruction))
            {
                pending_site = candidate_index;
            }
            candidate_index++;
            candidate = lpr_find_raw_candidate(bytes, size, candidate + 2u);
        }
        pos = end;
    }
    if (pending_site != UINT64_MAX) (*out_skipped)++;
    return candidate == UINT64_MAX ? 0 : -1;
}

static void lpr_apply_plan(uint8_t *bytes, uint64_t size,
                           const uint8_t plan[LPR_SITE_PLAN_BYTES],
                           lpr_patch_scan_result_t *result)
{
    uint64_t candidate_index = 0;
    uint64_t search = 0;
    for (;;) {
        const uint64_t candidate = lpr_find_raw_candidate(bytes, size, search);
        if (candidate == UINT64_MAX) return;
        if (lpr_plan_get(plan, candidate_index)) {
            bytes[candidate] = LPR_ZPOLINE_PATCH_TO0;
            bytes[candidate + 1u] = LPR_ZPOLINE_PATCH_TO1;
            result->patched_sites++;
        }
        candidate_index++;
        search = candidate + 2u;
    }
}

static int lpr_apply_local_second_pass(const ZydisDecoder *decoder,
                                       uint8_t *bytes, uint64_t size,
                                       lpr_patch_scan_result_t *result)
{
    uint64_t search = 0;
    for (;;) {
        const uint64_t candidate = lpr_find_raw_candidate(bytes, size, search);
        if (candidate == UINT64_MAX) return 0;
        const lpr_candidate_class_t classification = lpr_classify_candidate(
            decoder, bytes, size, candidate, 0);
        if (classification == LPR_CANDIDATE_AMBIGUOUS) return -1;
        if (classification == LPR_CANDIDATE_SITE &&
            lpr_site_has_decodable_successor(decoder, bytes, size, candidate))
        {
            bytes[candidate] = LPR_ZPOLINE_PATCH_TO0;
            bytes[candidate + 1u] = LPR_ZPOLINE_PATCH_TO1;
            result->patched_sites++;
        }
        search = candidate + 2u;
    }
}

static int lpr_apply_full_second_pass(const ZydisDecoder *decoder,
                                      uint8_t *bytes, uint64_t size,
                                      lpr_patch_scan_result_t *result)
{
    uint64_t pending_opcode = UINT64_MAX;
    for (uint64_t pos = 0; pos < size;) {
        ZydisDecodedInstruction instruction;
        const int decoded = lpr_decode(decoder, bytes, size, pos, &instruction);
        if (decoded <= 0) return -1;
        if (pending_opcode != UINT64_MAX) {
            bytes[pending_opcode] = LPR_ZPOLINE_PATCH_TO0;
            bytes[pending_opcode + 1u] = LPR_ZPOLINE_PATCH_TO1;
            result->patched_sites++;
            pending_opcode = UINT64_MAX;
        }
        const uint64_t end = pos + (uint64_t)decoded;
        if (lpr_is_syscall(&instruction) && end >= 2u &&
            lpr_is_raw_candidate(bytes, size, end - 2u))
        {
            pending_opcode = end - 2u;
        }
        pos = end;
    }
    return 0;
}

int lpr_patch_scan_syscalls(uint8_t *bytes, uint64_t size, uint64_t flags,
                            lpr_patch_scan_result_t *result)
{
    if (bytes == 0 || result == 0) return -1;
    result->patched_sites = 0;
    result->skipped_sites = 0;
    result->failed_sites = 0;
    if (size < 2u) return 0;
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                     ZYDIS_STACK_WIDTH_64)))
    {
        result->failed_sites = 1;
        return -1;
    }
    const uint64_t candidate_count = lpr_count_raw_candidates(bytes, size);
    if (candidate_count == 0) return 0;
    uint8_t site_plan[LPR_SITE_PLAN_BYTES] = {0};
    uint8_t *const plan = candidate_count <= LPR_SITE_PLAN_BITS ? site_plan : 0;
    uint64_t skipped = 0;
    const int start_is_boundary = (flags & LPR_PATCH_SCAN_START_BOUNDARY) != 0;
    const int plan_status = start_is_boundary
        ? lpr_plan_full(&decoder, bytes, size, plan, &skipped)
        : lpr_plan_local(&decoder, bytes, size, plan, &skipped);
    if (plan_status != 0) {
        result->failed_sites = 1;
        return -1;
    }
    result->skipped_sites = skipped;
    if (plan != 0) {
        lpr_apply_plan(bytes, size, plan, result);
        return 0;
    }
    const int apply_status = start_is_boundary
        ? lpr_apply_full_second_pass(&decoder, bytes, size, result)
        : lpr_apply_local_second_pass(&decoder, bytes, size, result);
    if (apply_status != 0) {
        result->failed_sites = 1;
        return -1;
    }
    return 0;
}
