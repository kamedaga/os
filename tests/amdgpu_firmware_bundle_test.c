/* SPDX-License-Identifier: MIT */
#include "firmware_bundle.h"
#include <kobox2/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inspect(unsigned char *bytes, size_t size, int expected_result,
    size_t expected_count)
{
    struct kobox_linux_firmware_file files[KOBOX_FIRMWARE_MAX_FILES] = {0};
    uint8_t digest[32];
    size_t count = 0;

    kb2_sha256(bytes, size, digest);
    int result = ph_firmware_bundle_open(bytes, size, digest, files, &count);
    return result == expected_result && count == expected_count ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *source = fopen(argv[1], "rb");
    if (!source || fseek(source, 0, SEEK_END))
        return 2;
    long length = ftell(source);
    if (length <= (long)sizeof(struct ph_firmware_bundle) ||
        fseek(source, 0, SEEK_SET))
        return 2;
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, source) != (size_t)length)
        return 2;
    fclose(source);
    struct ph_firmware_bundle *bundle = (void *)bytes;
    struct ph_firmware_bundle_entry original = bundle->entries[0];
    uint64_t second_offset = bundle->entries[1].offset;
    uint8_t digest[32];
    struct kobox_linux_firmware_file files[KOBOX_FIRMWARE_MAX_FILES] = {0};
    size_t count = 0;
    kb2_sha256(bytes, (size_t)length, digest);

    if (inspect(bytes, (size_t)length, 0, 15))
        return 1;
    digest[0] ^= 1;
    if (ph_firmware_bundle_open(bytes, (size_t)length, digest, files,
                                &count) != -EBADMSG || count)
        return 1;
    memcpy(bundle->entries[0].name, "../escape", sizeof("../escape"));
    if (inspect(bytes, (size_t)length, -EINVAL, 0))
        return 1;
    bundle->entries[0] = original;
    bundle->entries[1].offset = original.offset;
    if (inspect(bytes, (size_t)length, -EINVAL, 0))
        return 1;
    bundle->entries[1].offset = second_offset;
    bytes[original.offset] ^= 1;
    if (inspect(bytes, (size_t)length, -EBADMSG, 0))
        return 1;
    puts("AMDGPU_FIRMWARE_BUNDLE=PASS digest traversal overlap file-hash");
    free(bytes);
    return 0;
}
