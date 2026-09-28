/* SPDX-License-Identifier: MIT */
#include "inspect_policy.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int parse_digest(const unsigned char **cursor, const unsigned char *end,
    const char *key, uint8_t digest[32]) {
    size_t key_size = strlen(key);
    if ((size_t)(end - *cursor) < key_size + 65 ||
        memcmp(*cursor, key, key_size)) return -EINVAL;
    const unsigned char *hex = *cursor + key_size;
    for (size_t i = 0; i < 32; ++i) {
        int high = hex_digit(hex[i * 2]);
        int low = hex_digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return -EINVAL;
        digest[i] = (uint8_t)((high << 4) | low);
    }
    if (hex[64] != '\n') return -EINVAL;
    *cursor = hex + 65;
    return 0;
}

int gpud_inspect_policy_parse(const void *bytes, size_t size,
    struct gpud_boot_config *config) {
    static const char root_key[] = "package_root=";
    const unsigned char *cursor = bytes;
    const unsigned char *end;
    const unsigned char *newline;
    uint8_t firmware[32], manifest[32], sandbox[32];

    if (!bytes || !config || size > 384 ||
        size < sizeof(root_key) ||
        memcmp(cursor, root_key, sizeof(root_key) - 1)) return -EINVAL;
    end = cursor + size;
    cursor += sizeof(root_key) - 1;
    newline = memchr(cursor, '\n', (size_t)(end - cursor));
    if (!newline || (size_t)(newline - cursor) != strlen(config->package_root) ||
        memcmp(cursor, config->package_root, (size_t)(newline - cursor)))
        return -EINVAL;
    cursor = newline + 1;
    if (parse_digest(&cursor, end, "firmware_sha256=", firmware) ||
        parse_digest(&cursor, end, "manifest_sha256=", manifest) ||
        parse_digest(&cursor, end, "sandbox_sha256=", sandbox) ||
        cursor != end) return -EINVAL;
    memcpy(config->firmware_sha256, firmware, sizeof(firmware));
    memcpy(config->manifest_sha256, manifest, sizeof(manifest));
    memcpy(config->sandbox_sha256, sandbox, sizeof(sandbox));
    return 0;
}
