package diskimage

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"math"
	"os"
	"path/filepath"
	"sort"
	"unicode/utf16"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

const (
	sectorBytes       = 512
	gptEntryCount     = 128
	gptEntrySize      = 128
	primaryHeaderLBA  = 1
	primaryEntriesLBA = 2
	firstPartitionLBA = 2048
)

type Result struct {
	Path       string
	Created    bool
	SizeMiB    int
	Partitions int
}

type Options struct {
	Progress progress.Reporter
}

type partitionPlan struct {
	Name     string
	Index    int
	Format   string
	FirstLBA uint64
	LastLBA  uint64
	TypeGUID [16]byte
}

var (
	efiSystemTypeGUID = guid("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
	linuxDataTypeGUID = guid("0fc63daf-8483-4772-8e79-3d69d8477de4")
	diskGUID          = guid("c3992f6d-5d5f-4f4d-9406-c0a5f8220f01")
)

func Ensure(workspace *config.Workspace) (Result, error) {
	return EnsureWithOptions(workspace, Options{})
}

func EnsureWithOptions(workspace *config.Workspace, opts Options) (Result, error) {
	span := progress.Use(opts.Progress).Start("disk image", 5)
	defer span.Close()
	span.Set(1, "checking disk image")
	path := workspace.Path(workspace.Disk.Image)
	if _, err := os.Stat(path); err == nil {
		span.Done("disk image exists")
		return Result{Path: path, SizeMiB: workspace.Disk.SizeMiB, Partitions: len(workspace.Disk.Partitions)}, nil
	} else if !os.IsNotExist(err) {
		span.Fail("disk image check failed")
		return Result{}, err
	}
	if workspace.Disk.SizeMiB <= 0 {
		span.Fail("invalid disk size")
		return Result{}, fmt.Errorf("disk.sizeMiB must be positive")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		span.Fail("disk directory create failed")
		return Result{}, err
	}
	span.Set(2, "creating disk image")
	file, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_RDWR, 0o644)
	if err != nil {
		span.Fail("disk image create failed")
		return Result{}, err
	}
	defer file.Close()
	size := int64(workspace.Disk.SizeMiB) * 1024 * 1024
	span.Set(3, "sizing disk image")
	if err := file.Truncate(size); err != nil {
		span.Fail("disk image truncate failed")
		return Result{}, err
	}
	sectorCount := uint64(size / sectorBytes)
	span.Set(4, "planning partitions")
	plans, err := planPartitions(workspace, sectorCount)
	if err != nil {
		span.Fail("partition plan failed")
		return Result{}, err
	}
	span.Set(5, "writing GPT")
	if err := writeProtectiveMBR(file, sectorCount); err != nil {
		span.Fail("MBR write failed")
		return Result{}, err
	}
	if err := writeGPT(file, sectorCount, plans); err != nil {
		span.Fail("GPT write failed")
		return Result{}, err
	}
	span.Done("disk image created")
	return Result{Path: path, Created: true, SizeMiB: workspace.Disk.SizeMiB, Partitions: len(plans)}, nil
}

func planPartitions(workspace *config.Workspace, sectorCount uint64) ([]partitionPlan, error) {
	if sectorCount < 4096 {
		return nil, fmt.Errorf("disk too small")
	}
	lastUsable := sectorCount - 34
	names := make([]string, 0, len(workspace.Disk.Partitions))
	for name := range workspace.Disk.Partitions {
		names = append(names, name)
	}
	sort.Slice(names, func(i, j int) bool {
		return workspace.Disk.Partitions[names[i]].Index < workspace.Disk.Partitions[names[j]].Index
	})
	next := uint64(firstPartitionLBA)
	plans := make([]partitionPlan, 0, len(names))
	for i, name := range names {
		part := workspace.Disk.Partitions[name]
		if part.Index <= 0 {
			return nil, fmt.Errorf("disk partition %s has invalid index %d", name, part.Index)
		}
		first := next
		last := lastUsable
		if part.SizeMiB > 0 && !part.Grow && i != len(names)-1 {
			sizeSectors := uint64(part.SizeMiB) * 1024 * 1024 / sectorBytes
			if sizeSectors == 0 {
				return nil, fmt.Errorf("disk partition %s is too small", name)
			}
			last = first + sizeSectors - 1
		}
		if first > last || last > lastUsable {
			return nil, fmt.Errorf("disk partition %s does not fit", name)
		}
		typeGUID := linuxDataTypeGUID
		if name == "esp" || part.Format == "fat16" {
			typeGUID = efiSystemTypeGUID
		}
		plans = append(plans, partitionPlan{
			Name:     name,
			Index:    part.Index,
			Format:   part.Format,
			FirstLBA: first,
			LastLBA:  last,
			TypeGUID: typeGUID,
		})
		next = last + 1
	}
	return plans, nil
}

func writeProtectiveMBR(file *os.File, sectorCount uint64) error {
	mbr := make([]byte, sectorBytes)
	mbr[446+4] = 0xEE
	binary.LittleEndian.PutUint32(mbr[446+8:446+12], 1)
	length := sectorCount - 1
	if length > math.MaxUint32 {
		length = math.MaxUint32
	}
	binary.LittleEndian.PutUint32(mbr[446+12:446+16], uint32(length))
	mbr[510] = 0x55
	mbr[511] = 0xAA
	_, err := file.WriteAt(mbr, 0)
	return err
}

func writeGPT(file *os.File, sectorCount uint64, plans []partitionPlan) error {
	entries := make([]byte, gptEntryCount*gptEntrySize)
	for _, plan := range plans {
		offset := (plan.Index - 1) * gptEntrySize
		if offset < 0 || offset+gptEntrySize > len(entries) {
			return fmt.Errorf("partition index %d exceeds GPT entry table", plan.Index)
		}
		entry := entries[offset : offset+gptEntrySize]
		copy(entry[0:16], plan.TypeGUID[:])
		unique := partitionGUID(plan.Name)
		copy(entry[16:32], unique[:])
		binary.LittleEndian.PutUint64(entry[32:40], plan.FirstLBA)
		binary.LittleEndian.PutUint64(entry[40:48], plan.LastLBA)
		writeUTF16Name(entry[56:128], plan.Name)
	}
	entriesCRC := crc32.ChecksumIEEE(entries)
	if _, err := file.WriteAt(entries, int64(primaryEntriesLBA*sectorBytes)); err != nil {
		return err
	}
	backupEntriesLBA := sectorCount - 33
	if _, err := file.WriteAt(entries, int64(backupEntriesLBA*sectorBytes)); err != nil {
		return err
	}
	lastUsable := sectorCount - 34
	if err := writeGPTHeader(file, primaryHeaderLBA, sectorCount-1, primaryHeaderLBA, sectorCount-1, 34, lastUsable, primaryEntriesLBA, entriesCRC); err != nil {
		return err
	}
	return writeGPTHeader(file, sectorCount-1, primaryHeaderLBA, primaryHeaderLBA, sectorCount-1, 34, lastUsable, backupEntriesLBA, entriesCRC)
}

func writeGPTHeader(file *os.File, currentLBA, backupLBA, firstUsable, lastDiskLBA, firstPartLBA, lastUsable, entriesLBA uint64, entriesCRC uint32) error {
	header := make([]byte, sectorBytes)
	copy(header[0:8], "EFI PART")
	binary.LittleEndian.PutUint32(header[8:12], 0x00010000)
	binary.LittleEndian.PutUint32(header[12:16], 92)
	binary.LittleEndian.PutUint64(header[24:32], currentLBA)
	binary.LittleEndian.PutUint64(header[32:40], backupLBA)
	binary.LittleEndian.PutUint64(header[40:48], firstPartLBA)
	binary.LittleEndian.PutUint64(header[48:56], lastUsable)
	copy(header[56:72], diskGUID[:])
	binary.LittleEndian.PutUint64(header[72:80], entriesLBA)
	binary.LittleEndian.PutUint32(header[80:84], gptEntryCount)
	binary.LittleEndian.PutUint32(header[84:88], gptEntrySize)
	binary.LittleEndian.PutUint32(header[88:92], entriesCRC)
	crc := crc32.ChecksumIEEE(header[:92])
	binary.LittleEndian.PutUint32(header[16:20], crc)
	_, err := file.WriteAt(header, int64(currentLBA*sectorBytes))
	_ = lastDiskLBA
	return err
}

func partitionGUID(name string) [16]byte {
	sum := sha256.Sum256([]byte("CapabilityOS:" + name))
	var id [16]byte
	copy(id[:], sum[:16])
	id[6] = (id[6] & 0x0f) | 0x40
	id[8] = (id[8] & 0x3f) | 0x80
	return id
}

func guid(text string) [16]byte {
	var out [16]byte
	var a uint32
	var b, c uint16
	var d [8]byte
	if _, err := fmt.Sscanf(text, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]); err != nil {
		panic(err)
	}
	binary.LittleEndian.PutUint32(out[0:4], a)
	binary.LittleEndian.PutUint16(out[4:6], b)
	binary.LittleEndian.PutUint16(out[6:8], c)
	copy(out[8:16], d[:])
	return out
}

func writeUTF16Name(dst []byte, name string) {
	encoded := utf16.Encode([]rune(name))
	for i, ch := range encoded {
		if i*2+2 > len(dst) {
			return
		}
		binary.LittleEndian.PutUint16(dst[i*2:i*2+2], ch)
	}
}
