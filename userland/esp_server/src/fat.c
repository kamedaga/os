#include "fat_backend.h"

static u16 load_le16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 load_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int is_power_of_two_u16(u16 value) {
    return value != 0 && (value & (u16)(value - 1)) == 0;
}

int fat_parse_bpb(const u8 *sector, struct fat_bpb_info *out) {
    if (!sector || !out) return 0;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    const u16 bytes_per_sector = load_le16(sector + 11);
    const u8 sectors_per_cluster = sector[13];
    const u16 reserved_sector_count = load_le16(sector + 14);
    const u8 fat_count = sector[16];
    const u16 root_entry_count = load_le16(sector + 17);
    const u16 total_sectors_16 = load_le16(sector + 19);
    const u16 sectors_per_fat_16 = load_le16(sector + 22);
    const u32 total_sectors_32 = load_le32(sector + 32);
    const u32 sectors_per_fat_32 = load_le32(sector + 36);
    const u32 root_cluster = load_le32(sector + 44);

    if (!is_power_of_two_u16(bytes_per_sector)) return 0;
    if (bytes_per_sector < 512 || bytes_per_sector > 4096) return 0;
    if (sectors_per_cluster == 0 || (sectors_per_cluster & (u8)(sectors_per_cluster - 1)) != 0) return 0;
    if (reserved_sector_count == 0 || fat_count == 0) return 0;

    const u32 total_sectors = total_sectors_16 != 0 ? (u32)total_sectors_16 : total_sectors_32;
    const u32 sectors_per_fat = sectors_per_fat_16 != 0 ? (u32)sectors_per_fat_16 : sectors_per_fat_32;
    if (total_sectors == 0 || sectors_per_fat == 0) return 0;

    const u32 root_dir_sectors = (((u32)root_entry_count * 32u) + ((u32)bytes_per_sector - 1u)) / (u32)bytes_per_sector;
    const u32 first_fat_sector = reserved_sector_count;
    const u32 first_root_dir_sector = first_fat_sector + ((u32)fat_count * sectors_per_fat);
    const u32 first_data_sector = first_root_dir_sector + root_dir_sectors;
    if (first_data_sector >= total_sectors) return 0;

    const u32 data_sectors = total_sectors - first_data_sector;
    const u32 cluster_count = data_sectors / (u32)sectors_per_cluster;

    enum fat_type type = FAT_TYPE_INVALID;
    if (cluster_count < 4085u) {
        type = FAT_TYPE_12;
    } else if (cluster_count < 65525u) {
        type = FAT_TYPE_16;
    } else {
        type = FAT_TYPE_32;
    }

    if (type == FAT_TYPE_32 && root_cluster < 2u) return 0;
    if (type != FAT_TYPE_32 && root_entry_count == 0) return 0;

    out->type = type;
    out->bytes_per_sector = bytes_per_sector;
    out->sectors_per_cluster = sectors_per_cluster;
    out->reserved_sector_count = reserved_sector_count;
    out->fat_count = fat_count;
    out->root_entry_count = root_entry_count;
    out->total_sectors = total_sectors;
    out->sectors_per_fat = sectors_per_fat;
    out->root_dir_sectors = root_dir_sectors;
    out->first_fat_sector = first_fat_sector;
    out->first_root_dir_sector = first_root_dir_sector;
    out->first_data_sector = first_data_sector;
    out->cluster_count = cluster_count;
    out->root_cluster = type == FAT_TYPE_32 ? root_cluster : 0;
    return 1;
}

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int valid_short_char(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@':
    case '~': case '`': case '!': case '(': case ')': case '{':
    case '}': case '^': case '#': case '&':
        return 1;
    default:
        return 0;
    }
}

int fat_make_short_name(const char *path, u16 path_len, char out_name[11]) {
    if (!path || !out_name || path_len == 0) return 0;
    while (path_len > 0 && *path == '/') {
        path++;
        path_len--;
    }
    if (path_len == 0) return 0;

    for (int i = 0; i < 11; i++) out_name[i] = ' ';

    u16 i = 0;
    int base = 0;
    while (i < path_len && path[i] != '.' && path[i] != '/') {
        if (base >= 8) return 0;
        char c = ascii_upper(path[i++]);
        if (!valid_short_char(c)) return 0;
        out_name[base++] = c;
    }

    if (i < path_len && path[i] == '.') {
        i++;
        int ext = 8;
        while (i < path_len && path[i] != '/') {
            if (ext >= 11) return 0;
            char c = ascii_upper(path[i++]);
            if (!valid_short_char(c)) return 0;
            out_name[ext++] = c;
        }
    }

    if (i != path_len) return 0;
    return base > 0;
}
