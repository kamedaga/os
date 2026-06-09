package bootfs

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"capabilityos/pack/internal/progress"
	"capabilityos/pack/internal/rootsync"
)

const (
	magic          = 0x53465442
	version        = 1
	kindRegular    = 1
	flagExecutable = 1 << 0
	dataAlign      = 16
	headerBytes    = 104
	entryBytes     = 48
)

type Result struct {
	Path    string
	Entries int
	Bytes   int
	Skipped bool
}

type Options struct {
	Progress progress.Reporter
}

type entry struct {
	ImagePath  string
	SourcePath string
	Data       []byte
	PathOffset uint32
	DataOffset uint64
}

func BuildImage(manifestPath string, outputPath string) (Result, error) {
	return BuildImageWithOptions(manifestPath, outputPath, Options{})
}

func BuildImageWithOptions(manifestPath string, outputPath string, opts Options) (Result, error) {
	span := progress.Use(opts.Progress).Start("bootfs image", 4)
	defer span.Close()
	span.Set(1, "loading bootfs manifest")
	manifest, err := rootsync.LoadManifestWithOptionsProgress(manifestPath, rootsync.LoadOptions{CheckSymlinks: true}, span)
	if err != nil {
		span.Fail("bootfs manifest failed")
		return Result{}, err
	}
	span.SetTotal(int64(len(manifest.Files) + 4))
	items := make([]entry, 0, len(manifest.Files))
	for index, spec := range manifest.Files {
		span.Set(int64(index), spec.ImagePath)
		if spec.IsSymlink {
			span.Fail("bootfs symlink unsupported")
			return Result{}, fmt.Errorf("bootfs does not support symlink entry: %s", spec.ImagePath)
		}
		data, err := os.ReadFile(spec.SourcePath)
		if err != nil {
			span.Fail("bootfs file read failed")
			return Result{}, err
		}
		items = append(items, entry{
			ImagePath:  spec.ImagePath,
			SourcePath: spec.SourcePath,
			Data:       data,
		})
	}
	span.Set(int64(len(items)+1), "rendering bootfs image")
	sort.Slice(items, func(i, j int) bool { return items[i].ImagePath < items[j].ImagePath })
	image, err := render(items)
	if err != nil {
		span.Fail("bootfs render failed")
		return Result{}, err
	}
	span.Set(int64(len(items)+2), "checking existing bootfs image")
	if existing, err := os.ReadFile(outputPath); err == nil && bytes.Equal(existing, image) {
		span.Done("bootfs up-to-date")
		return Result{Path: outputPath, Entries: len(items), Bytes: len(image), Skipped: true}, nil
	}
	if err := os.MkdirAll(filepath.Dir(outputPath), 0o755); err != nil {
		span.Fail("bootfs directory create failed")
		return Result{}, err
	}
	span.Set(int64(len(items)+3), "writing bootfs image")
	if err := os.WriteFile(outputPath, image, 0o644); err != nil {
		span.Fail("bootfs write failed")
		return Result{}, err
	}
	span.Done("bootfs built")
	return Result{Path: outputPath, Entries: len(items), Bytes: len(image)}, nil
}

func render(items []entry) ([]byte, error) {
	stringBytes := 0
	for i := range items {
		if err := validateImagePath(items[i].ImagePath); err != nil {
			return nil, err
		}
		items[i].PathOffset = uint32(stringBytes)
		stringBytes += len(items[i].ImagePath)
	}
	entryTableOffset := uint64(headerBytes)
	stringTableOffset := entryTableOffset + uint64(entryBytes*len(items))
	dataOffset := align(stringTableOffset+uint64(stringBytes), dataAlign)
	cursor := dataOffset
	for i := range items {
		cursor = align(cursor, dataAlign)
		items[i].DataOffset = cursor
		cursor += uint64(len(items[i].Data))
	}
	image := make([]byte, cursor)
	binary.LittleEndian.PutUint32(image[0:4], magic)
	binary.LittleEndian.PutUint16(image[4:6], version)
	binary.LittleEndian.PutUint16(image[6:8], headerBytes)
	binary.LittleEndian.PutUint64(image[8:16], uint64(len(image)))
	binary.LittleEndian.PutUint32(image[16:20], uint32(len(items)))
	binary.LittleEndian.PutUint32(image[20:24], uint32(entryBytes*len(items)))
	binary.LittleEndian.PutUint64(image[24:32], entryTableOffset)
	binary.LittleEndian.PutUint64(image[32:40], stringTableOffset)
	binary.LittleEndian.PutUint64(image[40:48], uint64(stringBytes))
	binary.LittleEndian.PutUint64(image[48:56], dataOffset)
	binary.LittleEndian.PutUint64(image[56:64], uint64(len(image))-dataOffset)
	for index, item := range items {
		offset := int(entryTableOffset) + entryBytes*index
		binary.LittleEndian.PutUint32(image[offset:offset+4], item.PathOffset)
		binary.LittleEndian.PutUint16(image[offset+4:offset+6], uint16(len(item.ImagePath)))
		image[offset+6] = kindRegular
		image[offset+7] = flagsForPath(item.ImagePath)
		binary.LittleEndian.PutUint64(image[offset+8:offset+16], item.DataOffset)
		binary.LittleEndian.PutUint64(image[offset+16:offset+24], uint64(len(item.Data)))
		binary.LittleEndian.PutUint32(image[offset+24:offset+28], modeBitsForPath(item.ImagePath))
	}
	for _, item := range items {
		stringOffset := int(stringTableOffset) + int(item.PathOffset)
		copy(image[stringOffset:stringOffset+len(item.ImagePath)], item.ImagePath)
		copy(image[int(item.DataOffset):int(item.DataOffset)+len(item.Data)], item.Data)
	}
	return image, nil
}

func validateImagePath(path string) error {
	if len(path) < 2 || path[0] != '/' {
		return fmt.Errorf("invalid bootfs image path: %s", path)
	}
	if strings.HasSuffix(path, "/") || strings.Contains(path, "//") || strings.Contains(path, "/./") || strings.Contains(path, "/../") || strings.HasSuffix(path, "/.") || strings.HasSuffix(path, "/..") {
		return fmt.Errorf("invalid bootfs image path: %s", path)
	}
	return nil
}

func modeBitsForPath(path string) uint32 {
	if strings.HasSuffix(path, ".elf") {
		return 0o555
	}
	return 0o444
}

func flagsForPath(path string) byte {
	if strings.HasSuffix(path, ".elf") {
		return flagExecutable
	}
	return 0
}

func align(value uint64, boundary uint64) uint64 {
	return (value + boundary - 1) &^ (boundary - 1)
}
