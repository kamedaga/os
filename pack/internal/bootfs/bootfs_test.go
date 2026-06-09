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
