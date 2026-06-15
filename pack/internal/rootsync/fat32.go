package rootsync

import (
	"bufio"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"hash"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf16"

	"capabilityos/pack/internal/progress"
)

const (
	sectorBytes              = 512
	defaultSectorsPerCluster = 8
	reservedSectorCount      = 32
	fatCount                 = 2
	mediaDescriptor          = 0xF8
	fat32EOC                 = 0x0FFFFFFF
	bootSignature            = 0x29
	volumeID                 = 0x52465331
	dirAttr                  = 0x10
	archiveAttr              = 0x20
	symlinkAttr              = 0x24
	lfnAttr                  = 0x0F
	fixedDate                = uint16(((2026 - 1980) << 9) | (1 << 5) | 1)
	fixedTime                = uint16(0)
	symlinkMarker            = "CAPABILITYOS_ROOTFS_SYMLINK\n"
)

type PartitionRegion struct {
	FirstLBA uint64
	LastLBA  uint64
}

type Layout struct {
	PartitionFirstLBA uint64
	TotalSectors      uint32
	SectorsPerCluster uint32
	SectorsPerFAT     uint32
	TotalClusters     uint32
	FATStartSector    uint32
	DataStartSector   uint32
	RootCluster       uint32
}

type Directory struct {
	Path         string
	ParentIndex  int
	Leaf         string
	ShortName    [11]byte
	Cluster      uint32
	ClusterCount uint32
}

type FileSpec struct {
	ImagePath      string
	SourcePath     string
	Leaf           string
	ParentDirIndex int
	ShortName      [11]byte
	Size           uint64
	IsSymlink      bool
	SymlinkTarget  []byte
	StartCluster   uint32
	ClusterCount   uint32
	DirentOffset   int64
}

type Manifest struct {
	Path  string
	Dirs  []Directory
	Files []FileSpec
}

type LoadOptions struct {
	CheckSymlinks bool
}

type WriteResult struct {
	Files      int
	Dirs       int
	Bytes      uint64
	Clusters   uint32
	FirstLBA   uint64
	LastLBA    uint64
	Filesystem string
}

func OpenPartitionRegion(file *os.File, partitionIndex int) (PartitionRegion, error) {
	if partitionIndex <= 0 {
		return PartitionRegion{}, fmt.Errorf("invalid partition index: %d", partitionIndex)
	}
	header := make([]byte, sectorBytes)
	if _, err := file.ReadAt(header, sectorBytes); err != nil {
		return PartitionRegion{}, err
	}
	if string(header[:8]) != "EFI PART" {
		return PartitionRegion{}, errors.New("invalid GPT header")
	}
	entryLBA := binary.LittleEndian.Uint64(header[72:80])
	entryCount := binary.LittleEndian.Uint32(header[80:84])
	entrySize := binary.LittleEndian.Uint32(header[84:88])
	if uint32(partitionIndex) > entryCount || entrySize < 48 {
		return PartitionRegion{}, fmt.Errorf("invalid partition index: %d", partitionIndex)
	}
	entry := make([]byte, entrySize)
	offset := int64(entryLBA*sectorBytes + uint64(partitionIndex-1)*uint64(entrySize))
	if _, err := file.ReadAt(entry, offset); err != nil {
		return PartitionRegion{}, err
	}
	unused := true
	for _, b := range entry[:16] {
		if b != 0 {
			unused = false
			break
		}
	}
	if unused {
		return PartitionRegion{}, fmt.Errorf("partition %d is unused", partitionIndex)
	}
	first := binary.LittleEndian.Uint64(entry[32:40])
	last := binary.LittleEndian.Uint64(entry[40:48])
	if last < first {
		return PartitionRegion{}, errors.New("invalid partition region")
	}
	return PartitionRegion{FirstLBA: first, LastLBA: last}, nil
}

func ComputeLayout(region PartitionRegion) (Layout, error) {
	totalSectors64 := region.LastLBA - region.FirstLBA + 1
	if totalSectors64 > math.MaxUint32 {
		return Layout{}, errors.New("partition too large")
	}
	totalSectors := uint32(totalSectors64)
	for _, sectorsPerCluster := range []uint32{defaultSectorsPerCluster, 4, 2, 1} {
		sectorsPerFAT := uint32(1)
		for {
			dataSectors := totalSectors - reservedSectorCount - fatCount*sectorsPerFAT
			clusterCount := dataSectors / sectorsPerCluster
			neededFATBytes := (clusterCount + 2) * 4
			neededFATSectors := divCeil32(neededFATBytes, sectorBytes)
			if neededFATSectors == sectorsPerFAT {
				if clusterCount < 65525 {
					break
				}
				return Layout{
					PartitionFirstLBA: region.FirstLBA,
					TotalSectors:      totalSectors,
					SectorsPerCluster: sectorsPerCluster,
					SectorsPerFAT:     sectorsPerFAT,
					TotalClusters:     clusterCount,
					FATStartSector:    reservedSectorCount,
					DataStartSector:   reservedSectorCount + fatCount*sectorsPerFAT,
					RootCluster:       2,
				}, nil
			}
			sectorsPerFAT = neededFATSectors
		}
	}
	return Layout{}, errors.New("unsupported FAT32 layout")
}

func LoadManifest(manifestPath string) (Manifest, error) {
	return LoadManifestWithOptions(manifestPath, LoadOptions{CheckSymlinks: true})
}

func LoadManifestWithOptions(manifestPath string, opts LoadOptions) (Manifest, error) {
	return LoadManifestWithOptionsProgress(manifestPath, opts, nil)
}

func LoadManifestWithOptionsProgress(manifestPath string, opts LoadOptions, span progress.Span) (Manifest, error) {
	file, err := os.Open(manifestPath)
	if err != nil {
		return Manifest{}, err
	}
	defer file.Close()
	manifest := Manifest{Path: manifestPath}
	manifest.Dirs = append(manifest.Dirs, Directory{
		Path:        "/",
		ParentIndex: -1,
		Leaf:        "",
		Cluster:     2,
	})
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)
	serial := 1
	lineNumber := 0
	for scanner.Scan() {
		lineNumber++
		if span != nil && lineNumber%128 == 0 {
			span.Message(fmt.Sprintf("loading manifest line %d", lineNumber))
		}
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		imagePath, sourcePath, ok := strings.Cut(line, "=")
		if !ok {
			return Manifest{}, fmt.Errorf("%s:%d: invalid manifest line", manifestPath, lineNumber)
		}
		imagePath = strings.TrimSpace(imagePath)
		sourcePath = strings.TrimSpace(sourcePath)
		if err := validateImagePath(imagePath); err != nil {
			return Manifest{}, fmt.Errorf("%s:%d: %w", manifestPath, lineNumber, err)
		}
		if sourcePath == "@dir" {
			if _, err := ensureDirectory(&manifest.Dirs, imagePath); err != nil {
				return Manifest{}, err
			}
			continue
		}
		parent, leaf := splitParent(imagePath)
		parentIndex, err := ensureDirectory(&manifest.Dirs, parent)
		if err != nil {
			return Manifest{}, err
		}
		resolved := sourcePath
		if !filepath.IsAbs(resolved) {
			resolved = filepath.Join(filepath.Dir(manifestPath), resolved)
		}
		spec, err := fileSpec(imagePath, resolved, leaf, parentIndex, serial, opts)
		if err != nil {
			return Manifest{}, fmt.Errorf("%s:%d: %w", manifestPath, lineNumber, err)
		}
		manifest.Files = append(manifest.Files, spec)
		serial++
	}
	if err := scanner.Err(); err != nil {
		return Manifest{}, err
	}
	return manifest, nil
}

func WriteFAT32(file *os.File, layout Layout, manifest Manifest) (WriteResult, error) {
	return WriteFAT32WithProgress(file, layout, manifest, nil)
}

func WriteFAT32WithProgress(file *os.File, layout Layout, manifest Manifest, span progress.Span) (WriteResult, error) {
	dirs := manifest.Dirs
	files := manifest.Files
	if span != nil {
		span.SetTotal(int64(len(dirs) + len(files) + 5))
		span.Set(0, "planning FAT32 layout")
	}
	for i := range dirs {
		count := directoryClusterCount(layout, i, dirs, files)
		dirs[i].ClusterCount = count
	}
	dirs[0].Cluster = layout.RootCluster
	nextCluster := layout.RootCluster + dirs[0].ClusterCount
	for i := 1; i < len(dirs); i++ {
		dirs[i].Cluster = nextCluster
		nextCluster += dirs[i].ClusterCount
	}
	var totalBytes uint64
	for i := range files {
		clusterCount := divCeil64(files[i].Size, layout.ClusterBytes())
		if clusterCount > math.MaxUint32 {
			return WriteResult{}, fmt.Errorf("file too large: %s", files[i].ImagePath)
		}
		files[i].ClusterCount = uint32(clusterCount)
		if files[i].ClusterCount != 0 {
			files[i].StartCluster = nextCluster
			nextCluster += files[i].ClusterCount
		}
		totalBytes += files[i].Size
	}
	if nextCluster-2 > layout.TotalClusters {
		return WriteResult{}, errors.New("rootfs partition has no space left")
	}
	if span != nil {
		span.Message("building FAT table")
	}
	fat := make([]uint32, layout.TotalClusters+2)
	fat[0] = 0x0FFFFF00 | mediaDescriptor
	for _, dir := range dirs {
		chainFAT(fat, dir.Cluster, dir.ClusterCount)
	}
	for _, spec := range files {
		if spec.ClusterCount != 0 {
			chainFAT(fat, spec.StartCluster, spec.ClusterCount)
		}
	}
	if span != nil {
		span.Add(1, "writing boot sector")
	}
	if err := writeBootSector(file, layout); err != nil {
		return WriteResult{}, err
	}
	if span != nil {
		span.Add(1, "writing FAT tables")
	}
	if err := writeFATsWithProgress(file, layout, fat, span); err != nil {
		return WriteResult{}, err
	}
	for i := range dirs {
		if span != nil && i%32 == 0 {
			span.Set(int64(2+i), fmt.Sprintf("writing directories %d/%d", i, len(dirs)))
		}
		if err := writeDirectory(file, layout, i, dirs, files); err != nil {
			return WriteResult{}, err
		}
	}
	if span != nil {
		span.Set(int64(2+len(dirs)), "writing file data")
	}
	if err := writeFileDataWithProgress(file, layout, files, span, int64(2+len(dirs))); err != nil {
		return WriteResult{}, err
	}
	if span != nil {
		span.Set(int64(len(dirs)+len(files)+4), "syncing disk")
	}
	return WriteResult{
		Files:      len(files),
		Dirs:       len(dirs),
		Bytes:      totalBytes,
		Clusters:   nextCluster - 2,
		FirstLBA:   layout.PartitionFirstLBA,
		LastLBA:    layout.PartitionFirstLBA + uint64(layout.TotalSectors) - 1,
		Filesystem: "fat32",
	}, file.Sync()
}

func Fingerprint(manifest Manifest) ([]byte, error) {
	h := sha256.New()
	writeStringHash(h, "fat32-v1\n")
	for _, dir := range manifest.Dirs {
		writeStringHash(h, "d\x00"+dir.Path+"\n")
	}
	for _, spec := range manifest.Files {
		info, err := os.Stat(spec.SourcePath)
		if err != nil {
			return nil, err
		}
		writeStringHash(h, "f\x00")
		writeStringHash(h, spec.ImagePath)
		writeStringHash(h, "\x00")
		writeStringHash(h, spec.SourcePath)
		writeStringHash(h, "\x00")
		writeStringHash(h, fmt.Sprintf("%d:%d:%t\n", info.Size(), info.ModTime().UnixNano(), spec.IsSymlink))
	}
	return h.Sum(nil), nil
}

func fileSpec(imagePath, sourcePath, leaf string, parentIndex int, serial int, opts LoadOptions) (FileSpec, error) {
	info, err := os.Stat(sourcePath)
	if err != nil {
		return FileSpec{}, err
	}
	if info.IsDir() {
		return FileSpec{}, fmt.Errorf("source is a directory but manifest entry is file: %s", sourcePath)
	}
	spec := FileSpec{
		ImagePath:      imagePath,
		SourcePath:     sourcePath,
		Leaf:           leaf,
		ParentDirIndex: parentIndex,
		ShortName:      makeShortName(serial, false, extensionOf(leaf)),
		Size:           uint64(info.Size()),
	}
	if !opts.CheckSymlinks {
		return spec, nil
	}
	marker := make([]byte, len(symlinkMarker))
	f, err := os.Open(sourcePath)
	if err != nil {
		return FileSpec{}, err
	}
	defer f.Close()
	n, _ := io.ReadFull(f, marker)
	if n == len(marker) && string(marker) == symlinkMarker {
		target, err := io.ReadAll(f)
		if err != nil {
			return FileSpec{}, err
		}
		spec.IsSymlink = true
		spec.SymlinkTarget = target
		spec.Size = uint64(len(target))
	}
	return spec, nil
}

func ensureDirectory(dirs *[]Directory, path string) (int, error) {
	for i, dir := range *dirs {
		if dir.Path == path {
			return i, nil
		}
	}
	parent, leaf := splitParent(path)
	parentIndex, err := ensureDirectory(dirs, parent)
	if err != nil {
		return 0, err
	}
	index := len(*dirs)
	*dirs = append(*dirs, Directory{
		Path:        path,
		ParentIndex: parentIndex,
		Leaf:        leaf,
		ShortName:   makeShortName(index, true, ""),
	})
	return index, nil
}

func validateImagePath(path string) error {
	if len(path) < 2 || path[0] != '/' || strings.HasSuffix(path, "/") {
		return fmt.Errorf("invalid image path: %s", path)
	}
	for _, part := range strings.Split(path[1:], "/") {
		if part == "" || part == "." || part == ".." {
			return fmt.Errorf("invalid image path: %s", path)
		}
		if len(part) >= 128 {
			return fmt.Errorf("image path component too long: %s", part)
		}
	}
	return nil
}

func splitParent(path string) (string, string) {
	index := strings.LastIndex(path, "/")
	if index <= 0 {
		return "/", path[1:]
	}
	return path[:index], path[index+1:]
}

func directoryClusterCount(layout Layout, dirIndex int, dirs []Directory, files []FileSpec) uint32 {
	count := 0
	if dirIndex != 0 {
		count = 2
	}
	for i, dir := range dirs {
		if i != 0 && dir.ParentIndex == dirIndex {
			count += lfnEntryCount(dir.Leaf) + 1
		}
	}
	for _, spec := range files {
		if spec.ParentDirIndex == dirIndex {
			count += lfnEntryCount(spec.Leaf) + 1
		}
	}
	bytes := count * 32
	clusters := divCeil32(uint32(bytes), uint32(layout.ClusterBytes()))
	if clusters == 0 {
		return 1
	}
	return clusters
}

func writeBootSector(file *os.File, layout Layout) error {
	sector := make([]byte, sectorBytes)
	sector[0], sector[1], sector[2] = 0xEB, 0x58, 0x90
	copy(sector[3:11], "CAPOS   ")
	putU16(sector, 11, sectorBytes)
	sector[13] = byte(layout.SectorsPerCluster)
	putU16(sector, 14, reservedSectorCount)
	sector[16] = fatCount
	putU16(sector, 17, 0)
	putU16(sector, 19, 0)
	sector[21] = mediaDescriptor
	putU16(sector, 22, 0)
	putU16(sector, 24, 63)
	putU16(sector, 26, 255)
	putU32(sector, 28, uint32(layout.PartitionFirstLBA))
	putU32(sector, 32, layout.TotalSectors)
	putU32(sector, 36, layout.SectorsPerFAT)
	putU16(sector, 40, 0)
	putU16(sector, 42, 0)
	putU32(sector, 44, layout.RootCluster)
	putU16(sector, 48, 1)
	putU16(sector, 50, 6)
	sector[64] = 0x80
	sector[66] = bootSignature
	putU32(sector, 67, volumeID)
	copy(sector[71:82], "CAPROOTFS  ")
	copy(sector[82:90], "FAT32   ")
	sector[510], sector[511] = 0x55, 0xAA
	if _, err := file.WriteAt(sector, partitionOffset(layout, 0)); err != nil {
		return err
	}
	fsinfo := make([]byte, sectorBytes)
	putU32(fsinfo, 0, 0x41615252)
	putU32(fsinfo, 484, 0x61417272)
	putU32(fsinfo, 488, 0xFFFFFFFF)
	putU32(fsinfo, 492, 0xFFFFFFFF)
	putU32(fsinfo, 508, 0xAA550000)
	if _, err := file.WriteAt(fsinfo, partitionOffset(layout, 1)); err != nil {
		return err
	}
	_, err := file.WriteAt(sector, partitionOffset(layout, 6))
	return err
}

func writeFATs(file *os.File, layout Layout, fat []uint32) error {
	return writeFATsWithProgress(file, layout, fat, nil)
}

func writeFATsWithProgress(file *os.File, layout Layout, fat []uint32, span progress.Span) error {
	sector := make([]byte, sectorBytes)
	total := fatCount * layout.SectorsPerFAT
	for table := uint32(0); table < fatCount; table++ {
		for sectorIndex := uint32(0); sectorIndex < layout.SectorsPerFAT; sectorIndex++ {
			if span != nil && sectorIndex%128 == 0 {
				done := table*layout.SectorsPerFAT + sectorIndex
				span.Message(fmt.Sprintf("writing FAT sectors %d/%d", done, total))
			}
			clear(sector)
			for entryIndex := 0; entryIndex < sectorBytes/4; entryIndex++ {
				fatIndex := int(sectorIndex)*sectorBytes/4 + entryIndex
				if fatIndex >= len(fat) {
					break
				}
				putU32(sector, entryIndex*4, fat[fatIndex])
			}
			offset := partitionOffset(layout, layout.FATStartSector+table*layout.SectorsPerFAT+sectorIndex)
			if _, err := file.WriteAt(sector, offset); err != nil {
				return err
			}
		}
	}
	return nil
}

func readFAT(file *os.File, layout Layout) ([]uint32, error) {
	bytes := make([]byte, int(layout.SectorsPerFAT)*sectorBytes)
	if _, err := file.ReadAt(bytes, partitionOffset(layout, layout.FATStartSector)); err != nil {
		return nil, err
	}
	fatLen := int(layout.TotalClusters + 2)
	fat := make([]uint32, fatLen)
	for i := 0; i < fatLen; i++ {
		fat[i] = binary.LittleEndian.Uint32(bytes[i*4 : i*4+4])
	}
	return fat, nil
}

func readClusterChain(fat []uint32, start uint32, expectedCount uint32) ([]uint32, error) {
	if expectedCount == 0 {
		return nil, nil
	}
	if start < 2 || int(start) >= len(fat) {
		return nil, fmt.Errorf("invalid FAT chain start: %d", start)
	}
	chain := make([]uint32, 0, expectedCount)
	seen := map[uint32]bool{}
	cluster := start
	for {
		if cluster < 2 || int(cluster) >= len(fat) {
			return nil, fmt.Errorf("invalid FAT cluster in chain: %d", cluster)
		}
		if seen[cluster] {
			return nil, fmt.Errorf("cycle in FAT chain at cluster %d", cluster)
		}
		seen[cluster] = true
		chain = append(chain, cluster)
		if expectedCount > 0 && uint32(len(chain)) > expectedCount {
			return nil, fmt.Errorf("FAT chain length mismatch: got more than %d", expectedCount)
		}
		next := fat[cluster] & 0x0FFFFFFF
		if next >= 0x0FFFFFF8 {
			break
		}
		cluster = next
	}
	if uint32(len(chain)) != expectedCount {
		return nil, fmt.Errorf("FAT chain length mismatch: got %d, expected %d", len(chain), expectedCount)
	}
	return chain, nil
}

func resizeClusterChain(fat []uint32, start uint32, oldCount uint32, newCount uint32) ([]uint32, error) {
	chain, err := readClusterChain(fat, start, oldCount)
	if err != nil {
		return nil, err
	}
	if newCount == oldCount {
		return chain, nil
	}
	if newCount < oldCount {
		keep := chain[:newCount]
		free := chain[newCount:]
		if newCount > 0 {
			fat[keep[len(keep)-1]] = fat32EOC
		}
		for _, cluster := range free {
			fat[cluster] = 0
		}
		return keep, nil
	}
	needed := int(newCount - oldCount)
	added := make([]uint32, 0, needed)
	for cluster := uint32(2); cluster < uint32(len(fat)) && len(added) < needed; cluster++ {
		if fat[cluster] == 0 {
			added = append(added, cluster)
			fat[cluster] = fat32EOC
		}
	}
	if len(added) != needed {
		for _, cluster := range added {
			fat[cluster] = 0
		}
		return nil, errors.New("rootfs partition has no free clusters")
	}
	final := append(chain, added...)
	for i := 0; i < len(final); i++ {
		if i+1 == len(final) {
			fat[final[i]] = fat32EOC
		} else {
			fat[final[i]] = final[i+1]
		}
	}
	return final, nil
}

func allocateClusterChain(fat []uint32, count uint32) ([]uint32, error) {
	if count == 0 {
		return nil, nil
	}
	chain := make([]uint32, 0, count)
	for cluster := uint32(2); cluster < uint32(len(fat)) && uint32(len(chain)) < count; cluster++ {
		if fat[cluster] == 0 {
			chain = append(chain, cluster)
		}
	}
	if uint32(len(chain)) != count {
		return nil, errors.New("rootfs partition has no free clusters")
	}
	for i, cluster := range chain {
		if i+1 == len(chain) {
			fat[cluster] = fat32EOC
		} else {
			fat[cluster] = chain[i+1]
		}
	}
	return chain, nil
}

func writeDirentLocation(file *os.File, offset int64, startCluster uint32, size uint64) error {
	if offset <= 0 {
		return errors.New("missing FAT directory entry offset")
	}
	if size > math.MaxUint32 {
		return errors.New("file too large for FAT32 dirent")
	}
	entry := make([]byte, 32)
	if _, err := file.ReadAt(entry, offset); err != nil {
		return err
	}
	putU16(entry, 20, uint16(startCluster>>16))
	putU16(entry, 26, uint16(startCluster&0xFFFF))
	putU32(entry, 28, uint32(size))
	_, err := file.WriteAt(entry, offset)
	return err
}

func writeFileDirent(file *os.File, offset int64, spec FileSpec, startCluster uint32, size uint64) error {
	if offset <= 0 {
		return errors.New("missing FAT directory entry offset")
	}
	if size > math.MaxUint32 {
		return errors.New("file too large for FAT32 dirent")
	}
	attr := byte(archiveAttr)
	if spec.IsSymlink {
		attr = symlinkAttr
	}
	entries := addNamedDirent(nil, spec.Leaf, spec.ShortName, attr, startCluster, uint32(size), -1)
	startOffset := offset - int64((len(entries)-1)*32)
	if startOffset <= 0 {
		return errors.New("invalid FAT directory entry offset")
	}
	for index, entry := range entries {
		if _, err := file.WriteAt(entry.bytes[:], startOffset+int64(index*32)); err != nil {
			return err
		}
	}
	return nil
}

type dirEntryRecord struct {
	bytes     [32]byte
	fileIndex int
}

func writeDirectory(file *os.File, layout Layout, dirIndex int, dirs []Directory, files []FileSpec) error {
	var entries []dirEntryRecord
	dir := dirs[dirIndex]
	if dirIndex != 0 {
		entries = append(entries, dirEntryRecord{bytes: makeDirent(dotShortName("."), dirAttr, dir.Cluster, 0), fileIndex: -1})
		parentCluster := uint32(0)
		if dir.ParentIndex >= 0 {
			parentCluster = dirs[dir.ParentIndex].Cluster
		}
		entries = append(entries, dirEntryRecord{bytes: makeDirent(dotShortName(".."), dirAttr, parentCluster, 0), fileIndex: -1})
	}
	for i, child := range dirs {
		if i != 0 && child.ParentIndex == dirIndex {
			entries = addNamedDirent(entries, child.Leaf, child.ShortName, dirAttr, child.Cluster, 0, -1)
		}
	}
	for fileIndex, spec := range files {
		if spec.ParentDirIndex == dirIndex {
			attr := byte(archiveAttr)
			if spec.IsSymlink {
				attr = symlinkAttr
			}
			if spec.Size > math.MaxUint32 {
				return fmt.Errorf("file too large: %s", spec.ImagePath)
			}
			entries = addNamedDirent(entries, spec.Leaf, spec.ShortName, attr, spec.StartCluster, uint32(spec.Size), fileIndex)
		}
	}
	entriesPerCluster := dirEntriesPerCluster(layout)
	capacity := int(dir.ClusterCount) * entriesPerCluster
	if len(entries) > capacity {
		return fmt.Errorf("directory too large: %s", dir.Path)
	}
	for clusterOffset := uint32(0); clusterOffset < dir.ClusterCount; clusterOffset++ {
		clusterBytes := make([]byte, layout.ClusterBytes())
		firstEntry := int(clusterOffset) * entriesPerCluster
		for i := 0; i < entriesPerCluster && firstEntry+i < len(entries); i++ {
			record := entries[firstEntry+i]
			copy(clusterBytes[i*32:i*32+32], record.bytes[:])
			if record.fileIndex >= 0 {
				entryOffset := partitionOffset(layout, clusterToSector(layout, dir.Cluster+clusterOffset)) + int64(i*32)
				files[record.fileIndex].DirentOffset = entryOffset
			}
		}
		offset := partitionOffset(layout, clusterToSector(layout, dir.Cluster+clusterOffset))
		if _, err := file.WriteAt(clusterBytes, offset); err != nil {
			return err
		}
	}
	return nil
}

func writeFileData(file *os.File, layout Layout, files []FileSpec) error {
	return writeFileDataWithProgress(file, layout, files, nil, 0)
}

func writeFileDataWithProgress(file *os.File, layout Layout, files []FileSpec, span progress.Span, base int64) error {
	clusterBytes := make([]byte, layout.ClusterBytes())
	for index, spec := range files {
		if span != nil {
			span.Set(base+int64(index), fmt.Sprintf("writing files %d/%d %s", index, len(files), spec.ImagePath))
		}
		if spec.ClusterCount == 0 {
			continue
		}
		if spec.IsSymlink {
			if err := writeBytesData(file, layout, spec, spec.SymlinkTarget, clusterBytes); err != nil {
				return err
			}
			continue
		}
		src, err := os.Open(spec.SourcePath)
		if err != nil {
			return err
		}
		if err := writeStreamData(file, layout, spec, src, clusterBytes); err != nil {
			_ = src.Close()
			return err
		}
		if err := src.Close(); err != nil {
			return err
		}
	}
	if span != nil {
		span.Set(base+int64(len(files)), "file data written")
	}
	return nil
}

func writeStreamData(file *os.File, layout Layout, spec FileSpec, src io.Reader, clusterBytes []byte) error {
	for clusterOffset := uint32(0); clusterOffset < spec.ClusterCount; clusterOffset++ {
		clear(clusterBytes)
		n, err := io.ReadFull(src, clusterBytes)
		if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) {
			return err
		}
		offset := partitionOffset(layout, clusterToSector(layout, spec.StartCluster+clusterOffset))
		if _, err := file.WriteAt(clusterBytes, offset); err != nil {
			return err
		}
		if n < len(clusterBytes) {
			break
		}
	}
	return nil
}

func writeStreamDataChain(file *os.File, layout Layout, chain []uint32, src io.Reader, clusterBytes []byte) error {
	for _, cluster := range chain {
		clear(clusterBytes)
		n, err := io.ReadFull(src, clusterBytes)
		if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) {
			return err
		}
		offset := partitionOffset(layout, clusterToSector(layout, cluster))
		if _, err := file.WriteAt(clusterBytes, offset); err != nil {
			return err
		}
		if n < len(clusterBytes) {
			break
		}
	}
	return nil
}

func writeBytesDataChain(file *os.File, layout Layout, chain []uint32, data []byte, clusterBytes []byte) error {
	cursor := 0
	for _, cluster := range chain {
		clear(clusterBytes)
		n := copy(clusterBytes, data[cursor:])
		cursor += n
		offset := partitionOffset(layout, clusterToSector(layout, cluster))
		if _, err := file.WriteAt(clusterBytes, offset); err != nil {
			return err
		}
	}
	return nil
}

func writeBytesData(file *os.File, layout Layout, spec FileSpec, data []byte, clusterBytes []byte) error {
	cursor := 0
	for clusterOffset := uint32(0); clusterOffset < spec.ClusterCount; clusterOffset++ {
		clear(clusterBytes)
		n := copy(clusterBytes, data[cursor:])
		cursor += n
		offset := partitionOffset(layout, clusterToSector(layout, spec.StartCluster+clusterOffset))
		if _, err := file.WriteAt(clusterBytes, offset); err != nil {
			return err
		}
	}
	return nil
}

func makeDirent(shortName [11]byte, attr byte, cluster uint32, fileSize uint32) [32]byte {
	var entry [32]byte
	copy(entry[0:11], shortName[:])
	entry[11] = attr
	putU16(entry[:], 14, fixedTime)
	putU16(entry[:], 16, fixedDate)
	putU16(entry[:], 18, fixedDate)
	putU16(entry[:], 20, uint16(cluster>>16))
	putU16(entry[:], 22, fixedTime)
	putU16(entry[:], 24, fixedDate)
	putU16(entry[:], 26, uint16(cluster&0xFFFF))
	putU32(entry[:], 28, fileSize)
	return entry
}

func addNamedDirent(entries []dirEntryRecord, name string, shortName [11]byte, attr byte, cluster uint32, size uint32, fileIndex int) []dirEntryRecord {
	for _, lfn := range lfnEntries(name, shortName) {
		entries = append(entries, dirEntryRecord{bytes: lfn, fileIndex: -1})
	}
	return append(entries, dirEntryRecord{bytes: makeDirent(shortName, attr, cluster, size), fileIndex: fileIndex})
}

func lfnEntries(name string, shortName [11]byte) [][32]byte {
	units := utf16.Encode([]rune(name))
	count := (len(units) + 12) / 13
	checksum := lfnChecksum(shortName)
	out := make([][32]byte, 0, count)
	for remaining := count; remaining > 0; {
		remaining--
		var entry [32]byte
		for i := range entry {
			entry[i] = 0xFF
		}
		seq := byte(remaining + 1)
		if remaining+1 == count {
			seq |= 0x40
		}
		entry[0] = seq
		entry[11] = lfnAttr
		entry[12] = 0
		entry[13] = checksum
		entry[26], entry[27] = 0, 0
		start := remaining * 13
		for i := 0; i < 13; i++ {
			index := start + i
			ch := uint16(0xFFFF)
			if index < len(units) {
				ch = units[index]
			} else if index == len(units) {
				ch = 0
			}
			putLFNChar(entry[:], i, ch)
		}
		out = append(out, entry)
	}
	return out
}

func lfnEntryCount(name string) int {
	units := utf16.Encode([]rune(name))
	return (len(units) + 12) / 13
}

func lfnChecksum(shortName [11]byte) byte {
	var sum byte
	for _, b := range shortName {
		sum = ((sum & 1) << 7) + (sum >> 1) + b
	}
	return sum
}

func putLFNChar(entry []byte, slot int, ch uint16) {
	offsets := []int{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}
	putU16(entry, offsets[slot], ch)
}

func makeShortName(serial int, isDir bool, extHint string) [11]byte {
	var out [11]byte
	for i := range out {
		out[i] = ' '
	}
	prefix := byte('F')
	if isDir {
		prefix = 'D'
	}
	text := fmt.Sprintf("%c%07d", prefix, serial)
	copy(out[:8], text)
	ext := strings.ToUpper(extHint)
	if len(ext) > 3 {
		ext = ext[:3]
	}
	copy(out[8:], ext)
	return out
}

func dotShortName(dot string) [11]byte {
	var out [11]byte
	for i := range out {
		out[i] = ' '
	}
	copy(out[:], dot)
	return out
}

func extensionOf(name string) string {
	index := strings.LastIndex(name, ".")
	if index < 0 || index+1 >= len(name) {
		return ""
	}
	return name[index+1:]
}

func chainFAT(fat []uint32, start uint32, count uint32) {
	for i := uint32(0); i < count; i++ {
		cluster := start + i
		if i+1 == count {
			fat[cluster] = fat32EOC
		} else {
			fat[cluster] = cluster + 1
		}
	}
}

func clusterToSector(layout Layout, cluster uint32) uint32 {
	return layout.DataStartSector + (cluster-2)*layout.SectorsPerCluster
}

func (layout Layout) ClusterBytes() uint64 {
	return uint64(sectorBytes) * uint64(layout.SectorsPerCluster)
}

func dirEntriesPerCluster(layout Layout) int {
	return int(layout.ClusterBytes() / 32)
}

func partitionOffset(layout Layout, sectorIndex uint32) int64 {
	return int64((layout.PartitionFirstLBA + uint64(sectorIndex)) * sectorBytes)
}

func putU16(bytes []byte, offset int, value uint16) {
	binary.LittleEndian.PutUint16(bytes[offset:offset+2], value)
}

func putU32(bytes []byte, offset int, value uint32) {
	binary.LittleEndian.PutUint32(bytes[offset:offset+4], value)
}

func divCeil32(value uint32, divisor uint32) uint32 {
	if value == 0 {
		return 0
	}
	return (value + divisor - 1) / divisor
}

func divCeil64(value uint64, divisor uint64) uint64 {
	if value == 0 {
		return 0
	}
	return (value + divisor - 1) / divisor
}

func writeStringHash(h hash.Hash, value string) {
	_, _ = h.Write([]byte(value))
}

func SortManifest(manifest *Manifest) {
	sort.SliceStable(manifest.Files, func(i, j int) bool {
		return manifest.Files[i].ImagePath < manifest.Files[j].ImagePath
	})
}
