use crate::config::{DiskPartition, WorkspaceConfig};
use std::fs::{self, File, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

const SECTOR_BYTES: u64 = 512;
const PARTITION_ALIGNMENT_LBA: u64 = 2048;
const GPT_ENTRY_COUNT: u32 = 128;
const GPT_ENTRY_SIZE: u32 = 128;
const GPT_ENTRY_SECTORS: u64 =
    (GPT_ENTRY_COUNT as u64 * GPT_ENTRY_SIZE as u64).div_ceil(SECTOR_BYTES);
const GPT_PRIMARY_HEADER_LBA: u64 = 1;
const GPT_PRIMARY_ENTRIES_LBA: u64 = 2;
const DEFAULT_DISK_SIZE_MIB: u32 = 512;
const GPT_REVISION_1_0: u32 = 0x0001_0000;
const GPT_HEADER_SIZE: u32 = 92;
const PARTITION_NAME_MAX_CHARS: usize = 36;
const MIB_BYTES: u64 = 1024 * 1024;

pub struct DiskEnsureOutputs {
    pub disk_image: PathBuf,
    pub recreated: bool,
    pub size_bytes: u64,
}

#[derive(Copy, Clone)]
pub enum DiskEnsureMode {
    UseConfig,
    IfMissing,
    Always,
}

#[derive(Clone)]
struct PlannedPartition {
    config: DiskPartition,
    first_lba: u64,
    last_lba: u64,
}

pub fn ensure_disk_image(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    mode: DiskEnsureMode,
) -> Result<DiskEnsureOutputs, String> {
    let disk_image = workspace_root.join(&workspace.disk.image);
    if let Some(parent) = disk_image.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| format!("failed to create {}: {err}", parent.display()))?;
    }

    let should_recreate = match mode {
        DiskEnsureMode::Always => true,
        DiskEnsureMode::IfMissing => !disk_image.exists(),
        DiskEnsureMode::UseConfig => match workspace.disk.recreate.as_str() {
            "always" => true,
            "never" => !disk_image.exists(),
            "if-missing" | "" => !disk_image.exists(),
            other => {
                return Err(format!(
                    "unsupported [disk].recreate mode '{}': expected never, if-missing, or always",
                    other
                ));
            }
        },
    };

    if !should_recreate {
        let size_bytes = fs::metadata(&disk_image)
            .map_err(|err| format!("failed to stat {}: {err}", disk_image.display()))?
            .len();
        if size_bytes == 0 {
            return Err(format!(
                "existing disk image is empty: {} (run `pactl disk ensure --fresh`)",
                disk_image.display()
            ));
        }
        return Ok(DiskEnsureOutputs {
            disk_image,
            recreated: false,
            size_bytes,
        });
    }

    let size_mib = workspace.disk.size_mib.unwrap_or(DEFAULT_DISK_SIZE_MIB);
    let size_bytes = u64::from(size_mib) * MIB_BYTES;
    let total_sectors = size_bytes / SECTOR_BYTES;
    let planned = plan_partitions(workspace, total_sectors)?;

    let mut file = create_truncated_file(&disk_image, size_bytes)?;
    write_protective_mbr(&mut file, total_sectors)?;
    let disk_guid = make_disk_guid(workspace, &disk_image);
    let primary_entries = build_partition_entries(&planned, &disk_guid);
    let backup_entries = primary_entries.clone();

    let first_usable_lba = GPT_PRIMARY_ENTRIES_LBA + GPT_ENTRY_SECTORS;
    let backup_entries_lba = total_sectors
        .checked_sub(GPT_ENTRY_SECTORS + 1)
        .ok_or_else(|| format!("disk is too small for GPT: {} sectors", total_sectors))?;
    let last_usable_lba = backup_entries_lba
        .checked_sub(1)
        .ok_or_else(|| format!("disk is too small for GPT: {} sectors", total_sectors))?;

    write_at(&mut file, GPT_PRIMARY_ENTRIES_LBA * SECTOR_BYTES, &primary_entries)?;
    write_at(&mut file, backup_entries_lba * SECTOR_BYTES, &backup_entries)?;

    let primary_header = build_gpt_header(
        GPT_PRIMARY_HEADER_LBA,
        total_sectors - 1,
        first_usable_lba,
        last_usable_lba,
        GPT_PRIMARY_ENTRIES_LBA,
        &disk_guid,
        &primary_entries,
    );
    let backup_header = build_gpt_header(
        total_sectors - 1,
        GPT_PRIMARY_HEADER_LBA,
        first_usable_lba,
        last_usable_lba,
        backup_entries_lba,
        &disk_guid,
        &backup_entries,
    );

    write_at(&mut file, GPT_PRIMARY_HEADER_LBA * SECTOR_BYTES, &primary_header)?;
    write_at(&mut file, (total_sectors - 1) * SECTOR_BYTES, &backup_header)?;
    file.flush()
        .map_err(|err| format!("failed to flush {}: {err}", disk_image.display()))?;

    Ok(DiskEnsureOutputs {
        disk_image,
        recreated: true,
        size_bytes,
    })
}

fn create_truncated_file(path: &Path, size_bytes: u64) -> Result<File, String> {
    let file = OpenOptions::new()
        .create(true)
        .truncate(true)
        .read(true)
        .write(true)
        .open(path)
        .map_err(|err| format!("failed to create {}: {err}", path.display()))?;
    file.set_len(size_bytes)
        .map_err(|err| format!("failed to resize {} to {} bytes: {err}", path.display(), size_bytes))?;
    Ok(file)
}

fn plan_partitions(workspace: &WorkspaceConfig, total_sectors: u64) -> Result<Vec<PlannedPartition>, String> {
    if workspace.disk.partitions.is_empty() {
        return Err("no [[disk.partition]] entries configured".to_string());
    }
    if total_sectors <= (GPT_PRIMARY_ENTRIES_LBA + GPT_ENTRY_SECTORS) * 2 {
        return Err(format!("disk is too small for GPT: {} sectors", total_sectors));
    }

    let mut partitions = workspace.disk.partitions.clone();
    partitions.sort_by_key(|partition| partition.index);

    let first_usable_lba = GPT_PRIMARY_ENTRIES_LBA + GPT_ENTRY_SECTORS;
    let backup_entries_lba = total_sectors
        .checked_sub(GPT_ENTRY_SECTORS + 1)
        .ok_or_else(|| format!("disk is too small for GPT: {} sectors", total_sectors))?;
    let last_usable_lba = backup_entries_lba
        .checked_sub(1)
        .ok_or_else(|| format!("disk is too small for GPT: {} sectors", total_sectors))?;

    let mut cursor = align_up(first_usable_lba, PARTITION_ALIGNMENT_LBA);
    let mut planned = Vec::with_capacity(partitions.len());

    for partition in partitions {
        let first_lba = align_up(cursor, PARTITION_ALIGNMENT_LBA);
        if first_lba > last_usable_lba {
            return Err(format!(
                "partition '{}' does not fit in {} sector disk",
                partition.id, total_sectors
            ));
        }

        let last_lba = if partition.grow {
            last_usable_lba
        } else {
            let length_sectors = u64::from(partition.size_mib.unwrap_or(0))
                .checked_mul(MIB_BYTES / SECTOR_BYTES)
                .ok_or_else(|| format!("partition '{}' size is too large", partition.id))?;
            if length_sectors == 0 {
                return Err(format!("partition '{}' has invalid size 0 MiB", partition.id));
            }
            first_lba
                .checked_add(length_sectors - 1)
                .ok_or_else(|| format!("partition '{}' size overflows GPT layout", partition.id))?
        };

        if last_lba > last_usable_lba {
            return Err(format!(
                "partition '{}' exceeds disk size: end lba {} > {}",
                partition.id, last_lba, last_usable_lba
            ));
        }

        planned.push(PlannedPartition {
            config: partition,
            first_lba,
            last_lba,
        });
        cursor = last_lba
            .checked_add(1)
            .ok_or_else(|| "partition layout overflowed u64".to_string())?;
    }

    Ok(planned)
}

fn build_partition_entries(partitions: &[PlannedPartition], disk_guid: &[u8; 16]) -> Vec<u8> {
    let mut bytes = vec![0u8; GPT_ENTRY_COUNT as usize * GPT_ENTRY_SIZE as usize];
    for partition in partitions {
        let offset = (partition.config.index - 1) as usize * GPT_ENTRY_SIZE as usize;
        let entry = &mut bytes[offset..offset + GPT_ENTRY_SIZE as usize];
        entry[0..16].copy_from_slice(&partition_type_guid(&partition.config));
        entry[16..32].copy_from_slice(&make_partition_guid(disk_guid, &partition.config));
        write_u64_le(entry, 32, partition.first_lba);
        write_u64_le(entry, 40, partition.last_lba);
        write_u64_le(entry, 48, 0);
        write_utf16_name(entry, 56, partition_name(&partition.config));
    }
    bytes
}

fn partition_name(partition: &DiskPartition) -> &str {
    if partition.id == "esp" {
        "EFI System"
    } else {
        partition.id.as_str()
    }
}

fn partition_type_guid(partition: &DiskPartition) -> [u8; 16] {
    match partition.format.as_str() {
        "fat16" | "fat32" | "esp" | "efi" => guid_bytes("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"),
        _ => guid_bytes("0FC63DAF-8483-4772-8E79-3D69D8477DE4"),
    }
}

fn build_gpt_header(
    current_lba: u64,
    backup_lba: u64,
    first_usable_lba: u64,
    last_usable_lba: u64,
    entries_lba: u64,
    disk_guid: &[u8; 16],
    entries: &[u8],
) -> [u8; SECTOR_BYTES as usize] {
    let mut header = [0u8; SECTOR_BYTES as usize];
    header[0..8].copy_from_slice(b"EFI PART");
    write_u32_le(&mut header, 8, GPT_REVISION_1_0);
    write_u32_le(&mut header, 12, GPT_HEADER_SIZE);
    write_u32_le(&mut header, 16, 0);
    write_u64_le(&mut header, 24, current_lba);
    write_u64_le(&mut header, 32, backup_lba);
    write_u64_le(&mut header, 40, first_usable_lba);
    write_u64_le(&mut header, 48, last_usable_lba);
    header[56..72].copy_from_slice(disk_guid);
    write_u64_le(&mut header, 72, entries_lba);
    write_u32_le(&mut header, 80, GPT_ENTRY_COUNT);
    write_u32_le(&mut header, 84, GPT_ENTRY_SIZE);
    write_u32_le(&mut header, 88, crc32(entries));

    let header_crc = crc32(&header[..GPT_HEADER_SIZE as usize]);
    write_u32_le(&mut header, 16, header_crc);

    header
}

fn write_protective_mbr(file: &mut File, total_sectors: u64) -> Result<(), String> {
    let mut sector = [0u8; SECTOR_BYTES as usize];
    let entry = &mut sector[446..462];
    entry[0] = 0x00;
    entry[1..4].copy_from_slice(&[0x00, 0x02, 0x00]);
    entry[4] = 0xEE;
    entry[5..8].copy_from_slice(&[0xFF, 0xFF, 0xFF]);
    write_u32_le(entry, 8, 1);
    write_u32_le(entry, 12, u32::try_from(total_sectors.saturating_sub(1)).unwrap_or(u32::MAX));
    sector[510] = 0x55;
    sector[511] = 0xAA;
    write_at(file, 0, &sector)
}

fn write_at(file: &mut File, offset: u64, bytes: &[u8]) -> Result<(), String> {
    file.seek(SeekFrom::Start(offset))
        .map_err(|err| format!("failed to seek to {offset}: {err}"))?;
    file.write_all(bytes)
        .map_err(|err| format!("failed to write at {offset}: {err}"))
}

fn write_utf16_name(entry: &mut [u8], offset: usize, name: &str) {
    let mut out = [0u8; PARTITION_NAME_MAX_CHARS * 2];
    for (index, unit) in name.encode_utf16().take(PARTITION_NAME_MAX_CHARS).enumerate() {
        let start = index * 2;
        out[start..start + 2].copy_from_slice(&unit.to_le_bytes());
    }
    entry[offset..offset + out.len()].copy_from_slice(&out);
}

fn write_u32_le(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64_le(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn align_up(value: u64, alignment: u64) -> u64 {
    if alignment == 0 {
        value
    } else {
        value.div_ceil(alignment) * alignment
    }
}

fn make_disk_guid(workspace: &WorkspaceConfig, disk_image: &Path) -> [u8; 16] {
    let seed = format!(
        "{}:{}:{}",
        workspace.workspace.name,
        workspace.disk.image,
        disk_image.display()
    );
    hash_to_guid(&seed)
}

fn make_partition_guid(disk_guid: &[u8; 16], partition: &DiskPartition) -> [u8; 16] {
    let seed = format!(
        "{}:{}:{}:{}:{}",
        hex_bytes(disk_guid),
        partition.index,
        partition.id,
        partition.format,
        partition.size_mib.unwrap_or(0)
    );
    hash_to_guid(&seed)
}

fn hash_to_guid(seed: &str) -> [u8; 16] {
    let mut bytes = [0u8; 16];
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for (index, byte) in seed.bytes().enumerate() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x1000_0000_01b3);
        if index % 4 == 3 {
            let start = ((index / 4) * 8) % 16;
            bytes[start..start + 8].copy_from_slice(&hash.to_le_bytes());
        }
    }
    if bytes.iter().all(|byte| *byte == 0) {
        bytes[0] = 1;
    }
    bytes[7] = (bytes[7] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    bytes
}

fn hex_bytes(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(nibble_to_hex(byte >> 4));
        out.push(nibble_to_hex(byte & 0x0F));
    }
    out
}

fn nibble_to_hex(value: u8) -> char {
    match value {
        0..=9 => (b'0' + value) as char,
        10..=15 => (b'a' + (value - 10)) as char,
        _ => unreachable!(),
    }
}

fn guid_bytes(text: &str) -> [u8; 16] {
    let hex = text.replace('-', "");
    let mut raw = [0u8; 16];
    for (index, chunk) in hex.as_bytes().chunks(2).enumerate() {
        raw[index] = parse_hex_byte(chunk);
    }
    [
        raw[3], raw[2], raw[1], raw[0], raw[5], raw[4], raw[7], raw[6], raw[8], raw[9], raw[10],
        raw[11], raw[12], raw[13], raw[14], raw[15],
    ]
}

fn parse_hex_byte(chunk: &[u8]) -> u8 {
    fn nibble(byte: u8) -> u8 {
        match byte {
            b'0'..=b'9' => byte - b'0',
            b'a'..=b'f' => byte - b'a' + 10,
            b'A'..=b'F' => byte - b'A' + 10,
            _ => panic!("invalid hex"),
        }
    }
    (nibble(chunk[0]) << 4) | nibble(chunk[1])
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}
