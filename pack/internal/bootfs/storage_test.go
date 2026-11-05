package bootfs

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"capabilityos/pack/internal/rootsync"
)

func TestStoragePolicyUsesConfiguredPartitionWithoutCopy(t *testing.T) {
	image := make([]byte, 8192)
	copy(image[512:], "EFI PART")
	binary.LittleEndian.PutUint64(image[512+72:], 2)
	binary.LittleEndian.PutUint32(image[512+80:], 2)
	binary.LittleEndian.PutUint32(image[512+84:], 128)
	entry := image[1024+128:]
	entry[0] = 1
	partuuid := []byte{0x33, 0x22, 0x11, 0, 0x55, 0x44, 0x77, 0x46, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}
	copy(entry[16:32], partuuid)
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
	if len(entries) != 4 || entries[0].ImagePath != "/etc/pacha/root-partuuid" ||
		!bytes.Equal(entries[0].Data, partuuid) ||
		entries[1].ImagePath != "/etc/pacha/root-uuid" ||
		!bytes.Equal(entries[1].Data, superblock[104:120]) ||
		entries[2].ImagePath != "/etc/pacha/root-fstype" || string(entries[2].Data) != "ext4" {
		t.Fatal("partition and filesystem UUIDs were not preserved separately")
	}
	if binary.LittleEndian.Uint64(entries[3].Data[0:8]) != opts.RAMBytes ||
		binary.LittleEndian.Uint64(entries[3].Data[8:16]) != opts.QueueSize ||
		binary.LittleEndian.Uint64(entries[3].Data[16:24]) != opts.ArenaBytes {
		t.Fatal("wrong queue policy")
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(image, after) {
		t.Fatal("configured image was modified")
	}
	// Auto policy must survive packaging as zero for target-side discovery.
	opts.RAMBytes = 0
	auto, err := storageEntries(opts)
	if err != nil || binary.LittleEndian.Uint64(auto[3].Data[0:8]) != 0 {
		t.Fatalf("runtime RAM policy lost: %v", err)
	}
	// Reformatting changes the cache identity, not the boot partition selector.
	superblock[104] ^= 0xff
	if err := os.WriteFile(path, image, 0600); err != nil {
		t.Fatal(err)
	}
	reformatted, err := storageEntries(opts)
	if err != nil || !bytes.Equal(reformatted[0].Data, partuuid) ||
		bytes.Equal(reformatted[1].Data, entries[1].Data) {
		t.Fatalf("reformat changed PARTUUID or retained old FS UUID: %v", err)
	}
	clear(entry[16:32])
	if err := os.WriteFile(path, image, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := storageEntries(opts); err == nil {
		t.Fatal("accepted zero PARTUUID")
	}
	copy(entry[16:32], partuuid)
	if err := os.WriteFile(path, image, 0600); err != nil {
		t.Fatal(err)
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
	// The configured GPT entry may move; its GUID is the runtime selector.
	superblock[104] = 1
	copy(image[1024:1152], entry[:128])
	clear(entry[:128])
	if err := os.WriteFile(path, image, 0600); err != nil {
		t.Fatal(err)
	}
	opts.RootPartition = 1
	moved, err := storageEntries(opts)
	if err != nil || !bytes.Equal(moved[0].Data, partuuid) {
		t.Fatalf("lost PARTUUID after GPT entry moved: %v", err)
	}
	// Launch policy cannot be supplied by an ordinary manifest, even when
	// the caller attempts to replace only the new selector.
	for _, name := range []string{"root-partuuid", "root-uuid", "root-fstype", "storage-policy"} {
		manifest := filepath.Join(filepath.Dir(path), "manifest")
		if err := os.WriteFile(manifest, []byte("/etc/pacha/"+name+"="+path+"\n"), 0600); err != nil {
			t.Fatal(err)
		}
		if _, _, err := renderManifest(manifest, Options{Storage: &opts}, nil); err == nil {
			t.Fatalf("accepted manifest override of %s", name)
		}
	}
}

func TestRootFilesystemIdentity(t *testing.T) {
	identity := []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	for _, name := range []string{"ext4", "xfs", "btrfs"} {
		t.Run(name, func(t *testing.T) {
			image := make([]byte, 128<<10)
			region := rootsync.PartitionRegion{FirstLBA: 4, LastLBA: 200}
			start := int(region.FirstLBA * 512)
			switch name {
			case "ext4":
				copy(image[start+1024+56:], []byte{0x53, 0xef})
				copy(image[start+1024+104:], identity)
			case "xfs":
				copy(image[start:], "XFSB")
				copy(image[start+32:], identity)
			case "btrfs":
				copy(image[start+65536+64:], "_BHRfS_M")
				copy(image[start+65536+32:], identity)
				// Metadata UUID is a different identity; FileD needs FSID.
				copy(image[start+65536+571:], bytes.Repeat([]byte{0xff}, 16))
			}
			path := filepath.Join(t.TempDir(), "partition.img")
			if err := os.WriteFile(path, image, 0600); err != nil {
				t.Fatal(err)
			}
			file, err := os.Open(path)
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			kind, uuid, err := rootFilesystemIdentity(file, region)
			if err != nil || kind != name || !bytes.Equal(uuid, identity) {
				t.Fatalf("identity: %s %x %v", kind, uuid, err)
			}
			// Signatures outside the configured partition cannot identify it.
			if _, _, err := rootFilesystemIdentity(file, rootsync.PartitionRegion{LastLBA: 1}); err == nil {
				t.Fatal("identified a signature outside the root partition")
			}
			if name != "ext4" {
				copy(image[start+1024+56:], []byte{0x53, 0xef})
				copy(image[start+1024+104:], identity)
				if err := os.WriteFile(path, image, 0600); err != nil {
					t.Fatal(err)
				}
				if _, _, err := rootFilesystemIdentity(file, region); err == nil {
					t.Fatal("accepted ambiguous filesystem signatures")
				}
			}
		})
	}
}

func TestRootFilesystemIdentityFromMkfs(t *testing.T) {
	for _, name := range []string{"ext4", "xfs", "btrfs"} {
		t.Run(name, func(t *testing.T) {
			tool, err := exec.LookPath("mkfs." + name)
			if err != nil {
				t.Skip(err)
			}
			path := filepath.Join(t.TempDir(), "filesystem.img")
			file, err := os.Create(path)
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			const size = 512 << 20
			if err := file.Truncate(size); err != nil {
				t.Fatal(err)
			}
			id := "00112233-4455-4677-8899-aabbccddeeff"
			args := []string{"-q", "-f", "-U", id, path}
			if name == "ext4" {
				args = []string{"-q", "-F", "-U", id, path}
			}
			if name == "xfs" {
				args = []string{"-q", "-f", "-m", "uuid=" + id, path}
			}
			if output, err := exec.Command(tool, args...).CombinedOutput(); err != nil {
				t.Fatalf("mkfs.%s: %s %v", name, output, err)
			}
			kind, uuid, err := rootFilesystemIdentity(file, rootsync.PartitionRegion{LastLBA: size/512 - 1})
			expected := []byte{0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x46, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}
			if err != nil || kind != name || !bytes.Equal(uuid, expected) {
				t.Fatalf("mkfs identity: %s %x %v", kind, uuid, err)
			}
		})
	}
}
