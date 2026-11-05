package bootfs

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"os"

	"capabilityos/pack/internal/rootsync"
)

type StorageOptions struct {
	DiskPath                        string
	RootPartition                   int
	RAMBytes, QueueSize, ArenaBytes uint64
}

func storageEntries(opts StorageOptions) ([]entry, error) {
	// Zero RAM selects runtime discovery; the build host is not the target.
	if opts.ArenaBytes == 0 || opts.QueueSize < 2 ||
		opts.QueueSize > 32768 || opts.QueueSize&(opts.QueueSize-1) != 0 {
		return nil, fmt.Errorf("invalid storage RAM/queue policy")
	}
	// Only the explicitly configured pack target is opened, read-only. Read
	// the filesystem identity in place; never materialize a rootfs copy.
	disk, err := os.Open(opts.DiskPath)
	if err != nil {
		return nil, err
	}
	defer disk.Close()
	region, err := rootsync.OpenPartitionRegion(disk, opts.RootPartition)
	if err != nil {
		return nil, err
	}
	if region.PartitionUUID == [16]byte{} {
		return nil, fmt.Errorf("configured root PARTUUID is zero")
	}
	fstype, uuid, err := rootFilesystemIdentity(disk, region)
	if err != nil {
		return nil, err
	}
	policy := make([]byte, 24)
	binary.LittleEndian.PutUint64(policy[0:8], opts.RAMBytes)
	binary.LittleEndian.PutUint64(policy[8:16], opts.QueueSize)
	binary.LittleEndian.PutUint64(policy[16:24], opts.ArenaBytes)
	return []entry{
		{ImagePath: "/etc/pacha/root-partuuid", SourcePath: "generated:root-partuuid", Data: region.PartitionUUID[:]},
		{ImagePath: "/etc/pacha/root-uuid", SourcePath: "generated:root-uuid", Data: uuid},
		{ImagePath: "/etc/pacha/root-fstype", SourcePath: "generated:root-fstype", Data: []byte(fstype)},
		{ImagePath: "/etc/pacha/storage-policy", SourcePath: "generated:storage-policy", Data: policy},
	}, nil
}

// Probe only format-defined identity fields, within the configured partition.
// Linux validates the complete filesystem at mount. More than one signature
// means stale/ambiguous metadata and must never select a filesystem by order.
func rootFilesystemIdentity(disk *os.File, region rootsync.PartitionRegion) (string, []byte, error) {
	if region.FirstLBA > region.LastLBA || region.LastLBA >= math.MaxInt64/512 {
		return "", nil, fmt.Errorf("invalid root partition bounds")
	}
	partitionBytes := (region.LastLBA - region.FirstLBA + 1) * 512
	var fstype string
	var uuid []byte
	for _, format := range []struct {
		name                    string
		offset                  uint64
		magicOffset, uuidOffset int
		magic                   []byte
	}{
		{"ext4", 1024, 56, 104, []byte{0x53, 0xef}},
		{"xfs", 0, 0, 32, []byte("XFSB")},
		{"btrfs", 65536, 64, 32, []byte("_BHRfS_M")},
	} {
		if partitionBytes < format.offset+128 {
			continue
		}
		var superblock [128]byte
		if _, err := disk.ReadAt(superblock[:], int64(region.FirstLBA*512+format.offset)); err != nil {
			return "", nil, err
		}
		if !bytes.Equal(superblock[format.magicOffset:format.magicOffset+len(format.magic)], format.magic) {
			continue
		}
		if fstype != "" {
			return "", nil, fmt.Errorf("ambiguous root filesystem signatures")
		}
		fstype = format.name
		uuid = append([]byte(nil), superblock[format.uuidOffset:format.uuidOffset+16]...)
	}
	if fstype == "" {
		return "", nil, fmt.Errorf("root filesystem must be ext4, xfs or btrfs")
	}
	if bytes.Equal(uuid, make([]byte, 16)) {
		return "", nil, fmt.Errorf("configured root FS UUID is zero")
	}
	return fstype, uuid, nil
}
