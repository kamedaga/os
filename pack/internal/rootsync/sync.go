package rootsync

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

type Options struct {
	Force          bool
	Fingerprint    []byte
	Full           bool
	ChangedSources []string
	Progress       progress.Reporter
}

type Result struct {
	Disk       string
	Manifest   string
	Partition  int
	Skipped    bool
	Files      int
	Dirs       int
	Bytes      uint64
	Clusters   uint32
	Filesystem string
	FirstLBA   uint64
	LastLBA    uint64
	Updated    int
}

type layoutCache struct {
	ManifestHash string            `json:"manifestHash"`
	Files        []layoutCacheFile `json:"files"`
}

type layoutCacheFile struct {
	ImagePath    string `json:"imagePath"`
	SourcePath   string `json:"sourcePath"`
	Size         uint64 `json:"size"`
	ModTime      int64  `json:"modTime"`
	IsSymlink    bool   `json:"isSymlink"`
	StartCluster uint32 `json:"startCluster"`
	ClusterCount uint32 `json:"clusterCount"`
	DirentOffset int64  `json:"direntOffset"`
}

type fileUpdate struct {
	Spec   FileSpec
	Record layoutCacheFile
}

func SyncRootfs(workspace *config.Workspace, manifestPath string, opts Options) (Result, error) {
	return syncPartition(workspace, "rootfs", manifestPath, opts)
}

func SyncBootfs(workspace *config.Workspace, manifestPath string, opts Options) (Result, error) {
	return syncPartition(workspace, "esp", manifestPath, opts)
}

func syncPartition(workspace *config.Workspace, partitionName string, manifestPath string, opts Options) (Result, error) {
	span := progress.Use(opts.Progress).Start(partitionName+" sync", 8)
	defer span.Close()
	span.Set(1, "checking partition config")
	partition, ok := workspace.Disk.Partitions[partitionName]
	if !ok {
		span.Fail("missing partition")
		return Result{}, fmt.Errorf("missing disk.partitions.%s", partitionName)
	}
	if partition.Index == 0 {
		span.Fail("invalid partition")
		return Result{}, fmt.Errorf("%s partition index is 0", partitionName)
	}
	diskPath := workspace.Path(workspace.Disk.Image)
	span.Set(2, "fingerprinting manifest")
	layoutFingerprint, err := ManifestContentFingerprint(manifestPath)
	if err != nil {
		span.Fail("manifest fingerprint failed")
		return Result{}, err
	}
	contentFingerprint := opts.Fingerprint
	if len(contentFingerprint) == 0 {
		contentFingerprint = layoutFingerprint
		if partitionName != "rootfs" {
			span.Message("loading manifest for content fingerprint")
			manifest, err := LoadManifestWithOptionsProgress(manifestPath, LoadOptions{CheckSymlinks: true}, span)
			if err != nil {
				span.Fail("manifest load failed")
				return Result{}, err
			}
			span.Message("hashing manifest sources")
			contentFingerprint, err = Fingerprint(manifest)
			if err != nil {
				span.Fail("source fingerprint failed")
				return Result{}, err
			}
		}
	}
	cachePath := workspace.Path(workspace.State, partitionName+".sync.sha256")
	span.Set(3, "checking sync cache")
	if !opts.Force && len(opts.ChangedSources) == 0 && fingerprintMatches(cachePath, contentFingerprint) {
		if opts.Full || layoutCacheMatches(workspace, partitionName, layoutFingerprint) {
			span.Message("counting manifest entries")
			counts, err := LoadManifestCounts(manifestPath)
			if err != nil {
				span.Fail("manifest count failed")
				return Result{}, err
			}
			span.Done(partitionName + " up-to-date")
			return Result{
				Disk:      diskPath,
				Manifest:  manifestPath,
				Partition: partition.Index,
				Skipped:   true,
				Files:     counts.Files,
				Dirs:      counts.Dirs,
			}, nil
		}
	}
	if partitionName == "rootfs" && !opts.Force && len(opts.ChangedSources) == 0 && layoutCacheMatches(workspace, partitionName, layoutFingerprint) {
		span.Message("counting cached rootfs entries")
		counts, err := LoadManifestCounts(manifestPath)
		if err != nil {
			span.Fail("manifest count failed")
			return Result{}, err
		}
		if err := writeFingerprint(cachePath, contentFingerprint); err != nil {
			span.Fail("cache write failed")
			return Result{}, err
		}
		span.Done("rootfs up-to-date")
		return Result{
			Disk:      diskPath,
			Manifest:  manifestPath,
			Partition: partition.Index,
			Skipped:   true,
			Files:     counts.Files,
			Dirs:      counts.Dirs,
		}, nil
	}
	if !opts.Full {
		if partitionName == "rootfs" && len(opts.ChangedSources) > 0 {
			span.Set(4, "trying targeted incremental sync")
			if result, ok, err := tryTargetedIncrementalPartition(workspace, partitionName, diskPath, manifestPath, partition.Index, layoutFingerprint, opts.ChangedSources, span); err != nil {
				if !staleIncrementalError(err) {
					span.Fail("targeted incremental failed")
					return Result{}, err
				}
				span.Message("targeted incremental cache stale; falling back to full rewrite")
			} else if ok {
				if err := writeFingerprint(cachePath, contentFingerprint); err != nil {
					span.Fail("cache write failed")
					return Result{}, err
				}
				span.Done("targeted incremental synced")
				return result, nil
			}
		}
		span.Set(5, "trying incremental sync")
		if result, ok, err := tryIncrementalPartition(workspace, partitionName, diskPath, manifestPath, partition.Index, layoutFingerprint, span); err != nil {
			if !staleIncrementalError(err) {
				span.Fail("incremental sync failed")
				return Result{}, err
			}
			span.Message("incremental cache stale; falling back to full rewrite")
		} else if ok {
			if err := writeFingerprint(cachePath, contentFingerprint); err != nil {
				span.Fail("cache write failed")
				return Result{}, err
			}
			span.Done("incremental synced")
			return result, nil
		}
	}
	span.Set(6, "loading manifest for full rewrite")
	manifest, err := LoadManifestWithOptionsProgress(manifestPath, LoadOptions{CheckSymlinks: true}, span)
	if err != nil {
		span.Fail("manifest load failed")
		return Result{}, err
	}
	span.Set(7, "opening disk image")
	disk, err := os.OpenFile(diskPath, os.O_RDWR, 0)
	if err != nil {
		span.Fail("disk open failed")
		return Result{}, err
	}
	defer disk.Close()
	region, err := OpenPartitionRegion(disk, partition.Index)
	if err != nil {
		span.Fail("partition open failed")
		return Result{}, err
	}
	layout, err := ComputeLayout(region)
	if err != nil {
		span.Fail("layout compute failed")
		return Result{}, err
	}
	span.Set(8, "writing FAT32 image")
	writeResult, err := WriteFAT32WithProgress(disk, layout, manifest, span)
	if err != nil {
		span.Fail("FAT32 write failed")
		return Result{}, err
	}
	if err := writeFingerprint(cachePath, contentFingerprint); err != nil {
		span.Fail("cache write failed")
		return Result{}, err
	}
	if err := writeLayoutCache(workspace, partitionName, layoutFingerprint, manifest); err != nil {
		span.Fail("layout cache write failed")
		return Result{}, err
	}
	span.Done(partitionName + " synced")
	return Result{
		Disk:       diskPath,
		Manifest:   manifestPath,
		Partition:  partition.Index,
		Files:      writeResult.Files,
		Dirs:       writeResult.Dirs,
		Bytes:      writeResult.Bytes,
		Clusters:   writeResult.Clusters,
		Filesystem: writeResult.Filesystem,
		FirstLBA:   writeResult.FirstLBA,
		LastLBA:    writeResult.LastLBA,
	}, nil
}

func tryTargetedIncrementalPartition(workspace *config.Workspace, partitionName string, diskPath string, manifestPath string, partitionIndex int, layoutFingerprint []byte, changedSources []string, span progress.Span) (Result, bool, error) {
	cache, err := readLayoutCache(workspace, partitionName)
	if err != nil {
		return Result{}, false, nil
	}
	if cache.ManifestHash != hex.EncodeToString(layoutFingerprint) {
		return Result{}, false, nil
	}
	recordsBySource := map[string]layoutCacheFile{}
	for _, record := range cache.Files {
		recordsBySource[record.SourcePath] = record
	}
	var updates []fileUpdate
	var updatedRecords []layoutCacheFile
	for index, source := range changedSources {
		if span != nil {
			span.Message(fmt.Sprintf("checking changed source %d/%d", index+1, len(changedSources)))
		}
		sourceAbs, err := filepath.Abs(source)
		if err != nil {
			return Result{}, false, err
		}
		record, ok := recordsBySource[sourceAbs]
		if !ok {
			record, ok = recordsBySource[source]
		}
		if !ok {
			return Result{}, false, nil
		}
		changed, err := fileSpec(record.ImagePath, record.SourcePath, "", 0, 0, LoadOptions{CheckSymlinks: true})
		if err != nil {
			return Result{}, false, err
		}
		if record.DirentOffset <= 0 {
			return Result{}, false, nil
		}
		changed.StartCluster = record.StartCluster
		changed.ClusterCount = record.ClusterCount
		changed.DirentOffset = record.DirentOffset
		updates = append(updates, fileUpdate{Spec: changed, Record: record})
		info, err := os.Stat(record.SourcePath)
		if err != nil {
			return Result{}, false, err
		}
		record.ModTime = info.ModTime().UnixNano()
		record.Size = changed.Size
		record.IsSymlink = changed.IsSymlink
		record.ClusterCount = uint32(divCeil64(changed.Size, defaultClusterBytes()))
		updatedRecords = append(updatedRecords, record)
	}
	if len(updates) == 0 {
		return Result{}, false, nil
	}
	if span != nil {
		span.Message(fmt.Sprintf("writing %d targeted updates", len(updates)))
	}
	counts, err := LoadManifestCounts(manifestPath)
	if err != nil {
		return Result{}, false, err
	}
	result, writtenRecords, err := writeIncrementalUpdates(workspace, diskPath, partitionIndex, manifestPath, updates, span)
	if err != nil {
		return Result{}, false, err
	}
	result.Files = counts.Files
	result.Dirs = counts.Dirs
	for _, updated := range mergeUpdatedRecords(updatedRecords, writtenRecords) {
		for index, record := range cache.Files {
			if record.ImagePath == updated.ImagePath {
				cache.Files[index] = updated
				break
			}
		}
	}
	if err := writeLayoutCacheFile(workspace, partitionName, cache); err != nil {
		return Result{}, false, err
	}
	return result, true, nil
}

func tryIncrementalPartition(workspace *config.Workspace, partitionName string, diskPath string, manifestPath string, partitionIndex int, layoutFingerprint []byte, span progress.Span) (Result, bool, error) {
	cache, err := readLayoutCache(workspace, partitionName)
	if err != nil {
		return Result{}, false, nil
	}
	if cache.ManifestHash != hex.EncodeToString(layoutFingerprint) {
		return Result{}, false, nil
	}
	records := map[string]layoutCacheFile{}
	for _, record := range cache.Files {
		records[record.ImagePath] = record
	}
	manifest, err := LoadManifestWithOptionsProgress(manifestPath, LoadOptions{}, span)
	if err != nil {
		return Result{}, false, err
	}
	if len(manifest.Files) != len(cache.Files) {
		return Result{}, false, nil
	}
	var updates []fileUpdate
	var updatedRecords []layoutCacheFile
	for index, spec := range manifest.Files {
		if span != nil && index%128 == 0 {
			span.Message(fmt.Sprintf("checking rootfs files %d/%d", index, len(manifest.Files)))
		}
		record, ok := records[spec.ImagePath]
		if !ok || record.SourcePath != spec.SourcePath {
			return Result{}, false, nil
		}
		info, err := os.Stat(spec.SourcePath)
		if err != nil {
			return Result{}, false, err
		}
		if uint64(info.Size()) == record.Size && info.ModTime().UnixNano() == record.ModTime {
			continue
		}
		changed, err := fileSpec(spec.ImagePath, spec.SourcePath, spec.Leaf, spec.ParentDirIndex, 0, LoadOptions{CheckSymlinks: true})
		if err != nil {
			return Result{}, false, err
		}
		if record.DirentOffset <= 0 {
			return Result{}, false, nil
		}
		changed.StartCluster = record.StartCluster
		changed.ClusterCount = record.ClusterCount
		changed.DirentOffset = record.DirentOffset
		updates = append(updates, fileUpdate{Spec: changed, Record: record})
		record.Size = changed.Size
		record.ModTime = info.ModTime().UnixNano()
		record.IsSymlink = changed.IsSymlink
		record.ClusterCount = uint32(divCeil64(changed.Size, defaultClusterBytes()))
		updatedRecords = append(updatedRecords, record)
	}
	if len(updates) == 0 {
		if span != nil {
			span.Done(partitionName + " up-to-date")
		}
		return Result{
			Disk:      diskPath,
			Manifest:  manifestPath,
			Partition: partitionIndex,
			Skipped:   true,
			Files:     len(manifest.Files),
			Dirs:      len(manifest.Dirs),
		}, true, nil
	}
	if span != nil {
		span.Message(fmt.Sprintf("writing %d incremental updates", len(updates)))
	}
	result, writtenRecords, err := writeIncrementalUpdates(workspace, diskPath, partitionIndex, manifestPath, updates, span)
	if err != nil {
		return Result{}, false, err
	}
	result.Files = len(manifest.Files)
	result.Dirs = len(manifest.Dirs)
	for _, updated := range mergeUpdatedRecords(updatedRecords, writtenRecords) {
		for index, record := range cache.Files {
			if record.ImagePath == updated.ImagePath {
				cache.Files[index] = updated
				break
			}
		}
	}
	if err := writeLayoutCacheFile(workspace, partitionName, cache); err != nil {
		return Result{}, false, err
	}
	return result, true, nil
}

func writeIncrementalUpdates(workspace *config.Workspace, diskPath string, partitionIndex int, manifestPath string, updates []fileUpdate, span progress.Span) (Result, []layoutCacheFile, error) {
	disk, err := os.OpenFile(diskPath, os.O_RDWR, 0)
	if err != nil {
		return Result{}, nil, err
	}
	defer disk.Close()
	region, err := OpenPartitionRegion(disk, partitionIndex)
	if err != nil {
		return Result{}, nil, err
	}
	layout, err := ComputeLayout(region)
	if err != nil {
		return Result{}, nil, err
	}
	fat, err := readFAT(disk, layout)
	if err != nil {
		return Result{}, nil, err
	}
	clusterBytes := make([]byte, layout.ClusterBytes())
	var bytesWritten uint64
	var updatedRecords []layoutCacheFile
	if span != nil {
		span.SetTotal(int64(len(updates)))
	}
	for index, update := range updates {
		spec := update.Spec
		record := update.Record
		if span != nil {
			span.Set(int64(index), spec.ImagePath)
		}
		newClusterCount := uint32(divCeil64(spec.Size, layout.ClusterBytes()))
		chain, err := resizeClusterChain(fat, record.StartCluster, record.ClusterCount, newClusterCount)
		if err != nil {
			return Result{}, nil, err
		}
		newStart := uint32(0)
		if len(chain) > 0 {
			newStart = chain[0]
		}
		if spec.IsSymlink {
			if err := writeBytesDataChain(disk, layout, chain, spec.SymlinkTarget, clusterBytes); err != nil {
				return Result{}, nil, err
			}
			bytesWritten += uint64(len(spec.SymlinkTarget))
		} else {
			src, err := os.Open(spec.SourcePath)
			if err != nil {
				return Result{}, nil, err
			}
			if err := writeStreamDataChain(disk, layout, chain, src, clusterBytes); err != nil {
				_ = src.Close()
				return Result{}, nil, err
			}
			if err := src.Close(); err != nil {
				return Result{}, nil, err
			}
			bytesWritten += spec.Size
		}
		if err := writeDirentLocation(disk, record.DirentOffset, newStart, spec.Size); err != nil {
			return Result{}, nil, err
		}
		record.Size = spec.Size
		record.IsSymlink = spec.IsSymlink
		record.StartCluster = newStart
		record.ClusterCount = newClusterCount
		info, err := os.Stat(spec.SourcePath)
		if err != nil {
			return Result{}, nil, err
		}
		record.ModTime = info.ModTime().UnixNano()
		updatedRecords = append(updatedRecords, record)
		if span != nil {
			span.Set(int64(index+1), spec.ImagePath)
		}
	}
	if span != nil {
		span.Message("writing FAT tables")
	}
	if err := writeFATs(disk, layout, fat); err != nil {
		return Result{}, nil, err
	}
	if span != nil {
		span.Message("syncing disk")
	}
	if err := disk.Sync(); err != nil {
		return Result{}, nil, err
	}
	return Result{
		Disk:       diskPath,
		Manifest:   manifestPath,
		Partition:  partitionIndex,
		Bytes:      bytesWritten,
		Clusters:   0,
		Filesystem: "fat32",
		FirstLBA:   region.FirstLBA,
		LastLBA:    region.LastLBA,
		Updated:    len(updates),
	}, updatedRecords, nil
}

func staleIncrementalError(err error) bool {
	if err == nil {
		return false
	}
	text := err.Error()
	return strings.Contains(text, "invalid FAT cluster") ||
		strings.Contains(text, "FAT chain length mismatch") ||
		strings.Contains(text, "cycle in FAT chain") ||
		strings.Contains(text, "missing FAT directory entry offset")
}

func defaultClusterBytes() uint64 {
	return uint64(sectorBytes) * uint64(defaultSectorsPerCluster)
}

type ManifestCounts struct {
	Files int
	Dirs  int
}

func LoadManifestCounts(manifestPath string) (ManifestCounts, error) {
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return ManifestCounts{}, err
	}
	counts := ManifestCounts{}
	dirs := map[string]bool{"/": true}
	for _, raw := range bytesSplitLines(data) {
		line := bytes.TrimSpace(raw)
		if len(line) == 0 || line[0] == '#' {
			continue
		}
		image, source, ok := bytes.Cut(line, []byte("="))
		if !ok {
			return ManifestCounts{}, fmt.Errorf("invalid manifest line in %s", manifestPath)
		}
		imagePath := string(bytes.TrimSpace(image))
		if string(bytes.TrimSpace(source)) == "@dir" {
			addDirs(dirs, imagePath)
		} else {
			parent, _ := splitParent(imagePath)
			addDirs(dirs, parent)
			counts.Files++
		}
	}
	counts.Dirs = len(dirs)
	return counts, nil
}

func ManifestContentFingerprint(manifestPath string) ([]byte, error) {
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256(append([]byte("rootfs-manifest-v1\n"), data...))
	return sum[:], nil
}

func bytesSplitLines(input []byte) [][]byte {
	if len(input) == 0 {
		return nil
	}
	var out [][]byte
	start := 0
	for i, b := range input {
		if b == '\n' {
			out = append(out, input[start:i])
			start = i + 1
		}
	}
	if start <= len(input) {
		out = append(out, input[start:])
	}
	return out
}

func addDirs(dirs map[string]bool, path string) {
	if path == "" {
		return
	}
	for {
		dirs[path] = true
		if path == "/" {
			return
		}
		parent, _ := splitParent(path)
		if parent == path {
			return
		}
		path = parent
	}
}

func fingerprintMatches(path string, fingerprint []byte) bool {
	existing, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	expected := make([]byte, hex.EncodedLen(len(fingerprint)))
	hex.Encode(expected, fingerprint)
	return bytes.Equal(bytes.TrimSpace(existing), expected)
}

func writeFingerprint(path string, fingerprint []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	out := make([]byte, hex.EncodedLen(len(fingerprint))+1)
	hex.Encode(out[:len(out)-1], fingerprint)
	out[len(out)-1] = '\n'
	return os.WriteFile(path, out, 0o644)
}

func readLayoutCache(workspace *config.Workspace, partitionName string) (layoutCache, error) {
	bytes, err := os.ReadFile(layoutCachePath(workspace, partitionName))
	if err != nil {
		return layoutCache{}, err
	}
	var cache layoutCache
	if err := json.Unmarshal(bytes, &cache); err != nil {
		return layoutCache{}, err
	}
	return cache, nil
}

func layoutCacheMatches(workspace *config.Workspace, partitionName string, fingerprint []byte) bool {
	cache, err := readLayoutCache(workspace, partitionName)
	return err == nil && cache.ManifestHash == hex.EncodeToString(fingerprint)
}

func writeLayoutCache(workspace *config.Workspace, partitionName string, fingerprint []byte, manifest Manifest) error {
	cache := layoutCache{
		ManifestHash: hex.EncodeToString(fingerprint),
		Files:        make([]layoutCacheFile, 0, len(manifest.Files)),
	}
	for _, spec := range manifest.Files {
		info, err := os.Stat(spec.SourcePath)
		if err != nil {
			return err
		}
		cache.Files = append(cache.Files, layoutCacheFile{
			ImagePath:    spec.ImagePath,
			SourcePath:   spec.SourcePath,
			Size:         spec.Size,
			ModTime:      info.ModTime().UnixNano(),
			IsSymlink:    spec.IsSymlink,
			StartCluster: spec.StartCluster,
			ClusterCount: spec.ClusterCount,
			DirentOffset: spec.DirentOffset,
		})
	}
	return writeLayoutCacheFile(workspace, partitionName, cache)
}

func mergeUpdatedRecords(base []layoutCacheFile, replacement []layoutCacheFile) []layoutCacheFile {
	byPath := map[string]layoutCacheFile{}
	for _, record := range base {
		byPath[record.ImagePath] = record
	}
	for _, record := range replacement {
		byPath[record.ImagePath] = record
	}
	out := make([]layoutCacheFile, 0, len(byPath))
	for _, record := range byPath {
		out = append(out, record)
	}
	return out
}

func writeLayoutCacheFile(workspace *config.Workspace, partitionName string, cache layoutCache) error {
	bytes, err := json.Marshal(cache)
	if err != nil {
		return err
	}
	path := layoutCachePath(workspace, partitionName)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	bytes = append(bytes, '\n')
	return os.WriteFile(path, bytes, 0o644)
}

func layoutCachePath(workspace *config.Workspace, partitionName string) string {
	return workspace.Path(workspace.State, partitionName+".layout.json")
}
