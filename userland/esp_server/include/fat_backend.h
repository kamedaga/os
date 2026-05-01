#ifndef CAPABILITYOS_FAT_BACKEND_H
#define CAPABILITYOS_FAT_BACKEND_H

#include "fs_server_abi.h"

enum fat_type {
    FAT_TYPE_INVALID = 0,
    FAT_TYPE_12 = 12,
    FAT_TYPE_16 = 16,
    FAT_TYPE_32 = 32,
};

struct fat_bpb_info {
    enum fat_type type;
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u16 reserved_sector_count;
    u8 fat_count;
    u16 root_entry_count;
    u32 total_sectors;
    u32 sectors_per_fat;
    u32 root_dir_sectors;
    u32 first_fat_sector;
    u32 first_root_dir_sector;
    u32 first_data_sector;
    u32 cluster_count;
    u32 root_cluster;
};

int fat_parse_bpb(const u8 *sector, struct fat_bpb_info *out);
int fat_make_short_name(const char *path, u16 path_len, char out_name[11]);

#endif
