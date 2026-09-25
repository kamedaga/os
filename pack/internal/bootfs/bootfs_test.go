package bootfs

import (
	"encoding/binary"
	"testing"
)

func TestRenderMatchesBootFsAbiLayout(t *testing.T) {
	items := []entry{
		{ImagePath: "/srv/fat_server.elf", Data: []byte("fat")},
		{ImagePath: "/srv/virtio_blk.elf", Data: []byte("blk")},
	}
	image, err := render(items)
	if err != nil {
		t.Fatal(err)
	}

	if got := binary.LittleEndian.Uint16(image[6:8]); got != headerBytes {
		t.Fatalf("header bytes = %d, want %d", got, headerBytes)
	}
	if got := binary.LittleEndian.Uint32(image[20:24]); got != entryBytes*uint32(len(items)) {
		t.Fatalf("entry bytes total = %d, want %d", got, entryBytes*uint32(len(items)))
	}

	entryTableOffset := binary.LittleEndian.Uint64(image[24:32])
	stringTableOffset := binary.LittleEndian.Uint64(image[32:40])
	for index, wantPath := range []string{"/srv/fat_server.elf", "/srv/virtio_blk.elf"} {
		entryOffset := int(entryTableOffset) + entryBytes*index
		pathOffset := binary.LittleEndian.Uint32(image[entryOffset : entryOffset+4])
		pathBytes := binary.LittleEndian.Uint16(image[entryOffset+4 : entryOffset+6])
		gotPath := string(image[int(stringTableOffset)+int(pathOffset) : int(stringTableOffset)+int(pathOffset)+int(pathBytes)])
		if gotPath != wantPath {
			t.Fatalf("entry %d path = %q, want %q", index, gotPath, wantPath)
		}
		if got := image[entryOffset+6]; got != kindRegular {
			t.Fatalf("entry %d kind = %d, want %d", index, got, kindRegular)
		}
	}
}

func TestLiveExecutablePathsKeepExecutableMetadata(t *testing.T) {
	for _, path := range []string{
		"/bin/ash",
		"/bin/sh",
		"/bin/busybox",
		"/usr/bin/dropbearkey",
		"/usr/sbin/dropbear",
		"/lib/pacha/lpr-linux-x86_64.so",
		"/lib/ld-musl-x86_64.so.1",
		"/lib/linux/ld-musl-x86_64.so.1",
	} {
		if modeBitsForPath(path) != 0o555 || flagsForPath(path) != flagExecutable {
			t.Fatalf("%s is not executable in bootfs metadata", path)
		}
	}
	for _, path := range []string{"/etc/passwd", "/usr/lib/kobox/linux_tty_core.ko"} {
		if modeBitsForPath(path) != 0o444 || flagsForPath(path) != 0 {
			t.Fatalf("%s unexpectedly executable in bootfs metadata", path)
		}
	}
}

func TestRenderSharesSameSourceBytesAcrossAliases(t *testing.T) {
	items := []entry{
		{ImagePath: "/bin/ash", SourcePath: "busybox", Data: []byte("busybox")},
		{ImagePath: "/bin/sh", SourcePath: "busybox", Data: []byte("busybox")},
	}
	image, err := render(items)
	if err != nil {
		t.Fatal(err)
	}
	first := binary.LittleEndian.Uint64(image[headerBytes+8 : headerBytes+16])
	second := binary.LittleEndian.Uint64(image[headerBytes+entryBytes+8 : headerBytes+entryBytes+16])
	if first != second {
		t.Fatalf("aliases have different data offsets: %d, %d", first, second)
	}
	if len(image) != int(first)+len("busybox") {
		t.Fatalf("image contains duplicate payload: %d bytes", len(image))
	}
}
