package diskimage

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"os"
	"path/filepath"
	"testing"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/rootsync"
)

func TestNewDisksHaveIndependentGUIDsAndExistingDiskIsUntouched(t *testing.T) {
	seen := map[[16]byte]bool{}
	for i := 0; i < 2; i++ {
		workspace := &config.Workspace{Root: t.TempDir(), Disk: config.Disk{
			Image: "disk.img", SizeMiB: 8, Partitions: map[string]config.Partition{
				"esp":    {Index: 1, Format: "fat32", SizeMiB: 1},
				"rootfs": {Index: 2, Format: "ext4", Grow: true},
			},
		}}
		result, err := Ensure(workspace)
		if err != nil || !result.Created {
			t.Fatalf("create disk: %+v %v", result, err)
		}
		path := filepath.Join(workspace.Root, "disk.img")
		before, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		primary := before[512:1024]
		backup := before[len(before)-512:]
		var diskID [16]byte
		copy(diskID[:], primary[56:72])
		ids := [][16]byte{diskID}
		file, err := os.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		for index := 1; index <= 2; index++ {
			region, err := rootsync.OpenPartitionRegion(file, index)
			if err != nil {
				t.Fatal(err)
			}
			ids = append(ids, region.PartitionUUID)
		}
		file.Close()
		for _, id := range ids {
			if id == [16]byte{} || seen[id] || id[7]>>4 != 4 || id[8]>>6 != 2 {
				t.Fatalf("invalid or reused GPT GUID: %x", id)
			}
			seen[id] = true
		}
		if !bytes.Equal(primary[56:72], backup[56:72]) {
			t.Fatal("primary and backup disk GUIDs differ")
		}
		for _, header := range [][]byte{primary, backup} {
			crc := binary.LittleEndian.Uint32(header[16:20])
			copyHeader := append([]byte(nil), header[:92]...)
			clear(copyHeader[16:20])
			if crc32.ChecksumIEEE(copyHeader) != crc {
				t.Fatal("invalid GPT header CRC")
			}
			lba := binary.LittleEndian.Uint64(header[72:80])
			entries := before[lba*512 : lba*512+gptEntryCount*gptEntrySize]
			if crc32.ChecksumIEEE(entries) != binary.LittleEndian.Uint32(header[88:92]) {
				t.Fatal("invalid GPT entry CRC")
			}
			if !bytes.Equal(entries, before[1024:1024+gptEntryCount*gptEntrySize]) {
				t.Fatal("primary and backup entries differ")
			}
		}
		// Existing media may have the previous deterministic IDs: Ensure must
		// preserve arbitrary existing bytes rather than migrating their GUIDs.
		before[512+56] ^= 0xff
		if err := os.WriteFile(path, before, 0600); err != nil {
			t.Fatal(err)
		}
		again, err := Ensure(workspace)
		if err != nil || again.Created {
			t.Fatalf("reopen disk: %+v %v", again, err)
		}
		after, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(before, after) {
			t.Fatal("existing disk modified")
		}
	}
}
