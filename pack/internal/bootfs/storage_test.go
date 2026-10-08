package bootfs

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestStoragePolicyUsesConfiguredPartitionWithoutCopy(t *testing.T) {
	image := make([]byte, 8192)
	copy(image[512:], "EFI PART")
	binary.LittleEndian.PutUint64(image[512+72:], 2)
	binary.LittleEndian.PutUint32(image[512+80:], 2)
	binary.LittleEndian.PutUint32(image[512+84:], 128)
	entry := image[1024+128:]
	entry[0] = 1
	binary.LittleEndian.PutUint64(entry[32:], 4)
	binary.LittleEndian.PutUint64(entry[40:], 15)
	superblock := image[4*512+1024:]
	binary.LittleEndian.PutUint16(superblock[56:], 0xef53)
	for i := 0; i < 16; i++ {
		superblock[104+i] = byte(i + 1)
	}
	path := filepath.Join(t.TempDir(), "configured.img")
	if err := os.WriteFile(path, image, 0600); err != nil {
		t.Fatal(err)
	}
	opts := StorageOptions{DiskPath: path, RootPartition: 2,
		RAMBytes: 512 << 20, QueueSize: 64, ArenaBytes: 16 << 20}
	entries, err := storageEntries(opts)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 2 || !bytes.Equal(entries[0].Data, superblock[104:120]) {
		t.Fatal("wrong root UUID")
	}
	if binary.LittleEndian.Uint64(entries[1].Data[0:8]) != opts.RAMBytes ||
		binary.LittleEndian.Uint64(entries[1].Data[8:16]) != opts.QueueSize ||
		binary.LittleEndian.Uint64(entries[1].Data[16:24]) != opts.ArenaBytes {
		t.Fatal("wrong queue policy")
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(image, after) {
		t.Fatal("configured image was modified")
	}
	opts.RootPartition = 1
	if _, err := storageEntries(opts); err == nil {
		t.Fatal("accepted unused other partition")
	}
	opts.RootPartition = 2
	opts.QueueSize = 3
	if _, err := storageEntries(opts); err == nil {
		t.Fatal("accepted invalid queue size")
	}
	opts.QueueSize = 64
	clear(superblock[104:120])
	if err := os.WriteFile(path, image, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := storageEntries(opts); err == nil {
		t.Fatal("accepted zero root UUID")
	}
}
