/* Native VMA metadata is copied into this formatter; it never retains a
 * kernel pointer or infers mappings from the current stack pointer. */
static uint64_t lpr_proc_maps_hex(char *out, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i)
        out[i] = digits[(value >> ((15u - i) * 4u)) & 15u];
    return 16;
}

static int64_t lpr_proc_maps_line(const pacha_vm_mapping_info_t *record,
    uint64_t cursor, char *line, uint64_t *next)
{
    if (!record->size || record->start > UINT64_MAX - record->size ||
        record->start + record->size <= cursor || (record->prot & ~7ull))
        return -LPR_LINUX_EIO;
    /* A concurrent merge may make the next VMA contain the old cursor.
     * Emit its remaining interval, keeping this non-atomic walk monotonic. */
    uint64_t start = record->start < cursor ? cursor : record->start;
    uint64_t skipped = start - record->start;
    if (record->offset > UINT64_MAX - skipped) return -LPR_LINUX_EIO;
    *next = record->start + record->size;
    uint64_t n = lpr_proc_maps_hex(line, start);
    line[n++] = '-';
    n += lpr_proc_maps_hex(line + n, *next);
    line[n++] = ' ';
    line[n++] = record->prot & 1u ? 'r' : '-';
    line[n++] = record->prot & 2u ? 'w' : '-';
    line[n++] = record->prot & 4u ? 'x' : '-';
    line[n++] = record->flags & 8u ? 's' : 'p';
    line[n++] = ' ';
    n += lpr_proc_maps_hex(line + n, record->offset + skipped);
    /* Native VMOs have no filesystem inode/path identity. Do not invent a
     * filename or label private LPR mappings as Linux file-backed mappings. */
    const char suffix[] = " 00:00 0\n";
    for (unsigned i = 0; i < sizeof(suffix) - 1u; ++i) line[n++] = suffix[i];
    return (int64_t)n;
}
