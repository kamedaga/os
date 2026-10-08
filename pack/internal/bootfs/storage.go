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
	if opts.RAMBytes == 0 || opts.ArenaBytes == 0 || opts.QueueSize < 2 ||
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
	if region.FirstLBA > (math.MaxInt64-1024)/512 ||
		region.LastLBA-region.FirstLBA < 3 {
		return nil, fmt.Errorf("root partition cannot contain an ext4 superblock")
	}
	var superblock [1024]byte
	if _, err := disk.ReadAt(superblock[:], int64(region.FirstLBA*512+1024)); err != nil {
		return nil, err
	}
	// These are ext4 on-disk format fields, not a private Linux object layout.
	if binary.LittleEndian.Uint16(superblock[56:58]) != 0xef53 {
		return nil, fmt.Errorf("configured root partition is not ext4")
	}
	uuid := append([]byte(nil), superblock[104:120]...)
	if bytes.Equal(uuid, make([]byte, 16)) {
		return nil, fmt.Errorf("configured root UUID is zero")
	}
	policy := make([]byte, 24)
	binary.LittleEndian.PutUint64(policy[0:8], opts.RAMBytes)
	binary.LittleEndian.PutUint64(policy[8:16], opts.QueueSize)
	binary.LittleEndian.PutUint64(policy[16:24], opts.ArenaBytes)
	return []entry{
		{ImagePath: "/etc/pacha/root-uuid", SourcePath: "generated:root-uuid", Data: uuid},
		{ImagePath: "/etc/pacha/storage-policy", SourcePath: "generated:storage-policy", Data: policy},
	}, nil
}
