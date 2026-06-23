package rootsync

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"capabilityos/pack/internal/progress"
)

func WriteExt4WithProgress(disk *os.File, region PartitionRegion, manifest Manifest, span progress.Span) (WriteResult, error) {
	partitionBytes := (region.LastLBA - region.FirstLBA + 1) * sectorBytes
	if partitionBytes == 0 || partitionBytes > int64Max() {
		return WriteResult{}, fmt.Errorf("invalid ext4 partition size: %d", partitionBytes)
	}
	if span != nil {
		span.SetTotal(int64(len(manifest.Dirs) + len(manifest.Files) + 5))
		span.Set(0, "creating ext4 staging image")
	}

	tempDir, err := os.MkdirTemp(filepath.Dir(disk.Name()), "rootfs-ext4-*")
	if err != nil {
		return WriteResult{}, err
	}
	defer os.RemoveAll(tempDir)

	imagePath := filepath.Join(tempDir, "rootfs.ext4")
	image, err := os.Create(imagePath)
	if err != nil {
		return WriteResult{}, err
	}
	if err := image.Truncate(int64(partitionBytes)); err != nil {
		_ = image.Close()
		return WriteResult{}, err
	}
	if err := image.Close(); err != nil {
		return WriteResult{}, err
	}

	if err := runExt4Tool(
		"mkfs.ext4",
		"-q",
		"-F",
		"-m", "0",
		"-b", "4096",
		"-O", "^64bit,^metadata_csum,^metadata_csum_seed,^orphan_file",
		"-E", "lazy_itable_init=0,lazy_journal_init=0",
		"-L", "pacha-rootfs",
		imagePath,
	); err != nil {
		return WriteResult{}, err
	}

	cmdPath := filepath.Join(tempDir, "debugfs.cmds")
	bytesWritten, err := writeDebugFSCommands(cmdPath, manifest, span)
	if err != nil {
		return WriteResult{}, err
	}
	if err := runExt4Tool("debugfs", "-w", "-f", cmdPath, imagePath); err != nil {
		return WriteResult{}, err
	}

	if span != nil {
		span.Set(int64(len(manifest.Dirs)+len(manifest.Files)+3), "copying ext4 image into partition")
	}
	if err := copyImageToPartition(disk, imagePath, region); err != nil {
		return WriteResult{}, err
	}
	if span != nil {
		span.Set(int64(len(manifest.Dirs)+len(manifest.Files)+4), "syncing disk")
	}
	if err := disk.Sync(); err != nil {
		return WriteResult{}, err
	}
	return WriteResult{
		Files:      len(manifest.Files),
		Dirs:       len(manifest.Dirs),
		Bytes:      bytesWritten,
		Filesystem: "ext4",
		FirstLBA:   region.FirstLBA,
		LastLBA:    region.LastLBA,
	}, nil
}

func writeDebugFSCommands(path string, manifest Manifest, span progress.Span) (uint64, error) {
	file, err := os.Create(path)
	if err != nil {
		return 0, err
	}
	defer file.Close()

	step := int64(1)
	for i, dir := range manifest.Dirs {
		if dir.Path == "/" {
			continue
		}
		if span != nil && i%64 == 0 {
			span.Set(step, dir.Path)
		}
		if _, err := fmt.Fprintf(file, "mkdir %s\n", debugFSQuote(dir.Path)); err != nil {
			return 0, err
		}
		step++
	}

	var bytesWritten uint64
	for i, spec := range manifest.Files {
		if span != nil && i%64 == 0 {
			span.Set(step, spec.ImagePath)
		}
		if spec.IsSymlink {
			target := string(spec.SymlinkTarget)
			if err := validateDebugFSArg(target); err != nil {
				return 0, fmt.Errorf("%s: invalid symlink target: %w", spec.ImagePath, err)
			}
			if _, err := fmt.Fprintf(file, "symlink %s %s\n", debugFSQuote(spec.ImagePath), debugFSQuote(target)); err != nil {
				return 0, err
			}
			bytesWritten += uint64(len(spec.SymlinkTarget))
		} else {
			if _, err := fmt.Fprintf(file, "write %s %s\n", debugFSQuote(spec.SourcePath), debugFSQuote(spec.ImagePath)); err != nil {
				return 0, err
			}
			bytesWritten += spec.Size
		}
		step++
	}
	return bytesWritten, nil
}

func copyImageToPartition(disk *os.File, imagePath string, region PartitionRegion) error {
	image, err := os.Open(imagePath)
	if err != nil {
		return err
	}
	defer image.Close()

	offset := int64(region.FirstLBA * sectorBytes)
	buffer := make([]byte, 1024*1024)
	for {
		n, readErr := image.Read(buffer)
		if n > 0 {
			if _, err := disk.WriteAt(buffer[:n], offset); err != nil {
				return err
			}
			offset += int64(n)
		}
		if readErr == io.EOF {
			return nil
		}
		if readErr != nil {
			return readErr
		}
	}
}

func runExt4Tool(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s failed: %w\n%s", name, err, string(output))
	}
	return nil
}

func debugFSQuote(value string) string {
	return strconv.Quote(value)
}

func validateDebugFSArg(value string) error {
	if strings.ContainsRune(value, 0) {
		return fmt.Errorf("contains NUL")
	}
	if strings.ContainsAny(value, "\r\n") {
		return fmt.Errorf("contains newline")
	}
	return nil
}

func int64Max() uint64 {
	return uint64(^uint64(0) >> 1)
}
