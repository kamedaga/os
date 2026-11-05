package rootsync

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"

	"capabilityos/pack/internal/progress"
	"github.com/hanwen/go-fuse/v2/fuse"
)

func TestPartitionViewNeverWritesOutsideBounds(t *testing.T) {
	disk, err := os.CreateTemp(t.TempDir(), "disk")
	if err != nil {
		t.Fatal(err)
	}
	defer disk.Close()
	before := bytes.Repeat([]byte{0x5a}, 4096)
	if _, err := disk.Write(before); err != nil {
		t.Fatal(err)
	}
	p := partitionView{disk: disk, offset: 512, size: 1024, writable: true}
	for _, off := range []int64{-1, 1020, 1024, 1 << 62} {
		if _, errno := p.Write(context.Background(), nil, []byte("overflow"), off); errno != syscall.EFBIG {
			t.Fatalf("write at %d: %v", off, errno)
		}
	}
	after, _ := os.ReadFile(disk.Name())
	if !bytes.Equal(before, after) {
		t.Fatal("invalid write modified disk")
	}
	if n, errno := p.Write(context.Background(), nil, []byte("data"), 1020); n != 4 || errno != 0 {
		t.Fatalf("bounded write: %d %v", n, errno)
	}
	after, _ = os.ReadFile(disk.Name())
	copy(before[1532:1536], "data")
	if !bytes.Equal(before, after) {
		t.Fatal("write escaped partition")
	}
	p.writable = false
	if _, errno := p.Write(context.Background(), nil, []byte("x"), 0); errno != syscall.EROFS {
		t.Fatal(errno)
	}
	var attrs fuse.AttrOut
	if errno := p.Getattr(context.Background(), nil, &attrs); errno != 0 || attrs.Size != 1024 {
		t.Fatal(attrs)
	}
}

func TestRootlessMkfsAndInPlaceCheck(t *testing.T) {
	if _, err := os.Stat("/dev/fuse"); err != nil {
		t.Skip(err)
	}
	if _, err := exec.LookPath("fusermount3"); err != nil {
		t.Skip(err)
	}
	for _, filesystem := range []string{"ext4", "xfs", "btrfs"} {
		t.Run(filesystem, func(t *testing.T) {
			checker, _, _ := filesystemCheckCommand(filesystem, "")
			tools := []string{"mkfs." + filesystem, checker}
			if filesystem == "ext4" {
				tools = append(tools, "debugfs")
			} else {
				tools = append(tools, "unshare")
			}
			for _, tool := range tools {
				if _, err := exec.LookPath(tool); err != nil {
					t.Skip(err)
				}
			}
			dir := t.TempDir()
			source := filepath.Join(dir, "source")
			if err := os.WriteFile(source, []byte("manifest payload\n"), 0751); err != nil {
				t.Fatal(err)
			}
			if err := os.Truncate(source, 8193); err != nil {
				t.Fatal(err)
			}
			sourceBefore, _ := os.Stat(source)
			disk, err := os.Create(filepath.Join(dir, "disk.img"))
			if err != nil {
				t.Fatal(err)
			}
			defer disk.Close()
			const size = 512 << 20
			if err := disk.Truncate(size + 4096); err != nil {
				t.Fatal(err)
			}
			region := PartitionRegion{FirstLBA: 4, LastLBA: size/512 + 3}
			guard := bytes.Repeat([]byte{0x5a}, 2048)
			if _, err := disk.WriteAt(guard, 0); err != nil {
				t.Fatal(err)
			}
			if _, err := disk.WriteAt(guard, size+2048); err != nil {
				t.Fatal(err)
			}
			manifest := Manifest{Dirs: []Directory{{Path: "/"}, {Path: "/bin"}, {Path: "/dev"}}, Files: []FileSpec{
				{ImagePath: "/bin/probe", SourcePath: source, Size: 8193},
				{ImagePath: "/bin/link", IsSymlink: true, SymlinkTarget: []byte("probe")},
				{ImagePath: "/dev/null", IsDeviceNode: true, DeviceType: 'c', DeviceMajor: 1, DeviceMinor: 3},
				{ImagePath: "/dev/disk", IsDeviceNode: true, DeviceType: 'b', DeviceMajor: 8, DeviceMinor: 0},
			}}
			writer := WriteRootlessWithProgress
			if filesystem == "ext4" {
				writer = func(disk *os.File, region PartitionRegion, _ string, manifest Manifest, _ progress.Span) (WriteResult, error) {
					return WriteExt4WithProgress(disk, region, manifest, nil)
				}
			}
			result, err := writer(disk, region, filesystem, manifest, nil)
			if err != nil {
				t.Fatal(err)
			}
			if result.Files != 4 || result.Bytes != 8198 {
				t.Fatal(result)
			}
			readonly, err := os.Open(disk.Name())
			if err != nil {
				t.Fatal(err)
			}
			defer readonly.Close()
			if err := CheckFilesystem(readonly, region, filesystem); err != nil {
				t.Fatal(err)
			}
			for _, off := range []int64{0, size + 2048} {
				got := make([]byte, len(guard))
				if _, err := disk.ReadAt(got, off); err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(got, guard) {
					t.Fatal("mkfs/check overwrote GPT/other partition")
				}
			}
			after, _ := os.Stat(source)
			if after.Mode() != sourceBefore.Mode() || after.Size() != sourceBefore.Size() {
				t.Fatal("manifest source metadata changed")
			}
			mounts, _ := filepath.Glob(filepath.Join(dir, ".pacgo-view-*"))
			if len(mounts) != 0 {
				t.Fatalf("view leaked: %v", mounts)
			}
		})
	}
}
