package rootsync

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"capabilityos/pack/internal/progress"
)

func filesystemCheckCommand(filesystem, image string) (string, []string, error) {
	switch normalizeFilesystem(filesystem) {
	case "ext4":
		return "e2fsck", []string{"-fn", image}, nil
	case "xfs":
		return "xfs_repair", []string{"-n", image}, nil
	case "btrfs":
		return "btrfs", []string{"check", "--readonly", image}, nil
	default:
		return "", nil, fmt.Errorf("unsupported filesystem format %q", filesystem)
	}
}

// CheckFilesystem reads the configured partition in place. A read-only FUSE
// alias replaces the old partition-sized fsck scratch copy; no repair is run.
func CheckFilesystem(disk *os.File, region PartitionRegion, filesystem string) (err error) {
	tool, _, err := filesystemCheckCommand(filesystem, "")
	if err != nil {
		return err
	}
	if _, err := exec.LookPath(tool); err != nil {
		return err
	}
	view, err := newImageView(disk, region, nil, false)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, view.close()) }()
	_, args, _ := filesystemCheckCommand(filesystem, filepath.Join(view.dir, "partition"))
	return runFilesystemTool(tool, args...)
}

func WriteRootlessWithProgress(disk *os.File, region PartitionRegion, filesystem string, manifest Manifest, span progress.Span) (result WriteResult, err error) {
	filesystem = normalizeFilesystem(filesystem)
	if filesystem != "xfs" && filesystem != "btrfs" {
		return result, fmt.Errorf("unsupported rootless format %q", filesystem)
	}
	checker, _, _ := filesystemCheckCommand(filesystem, "")
	for _, tool := range []string{"unshare", "mkfs." + filesystem, checker} {
		if _, err := exec.LookPath(tool); err != nil {
			return result, err
		}
	}
	if span != nil {
		span.Message("opening rootless partition and manifest views")
	}
	view, err := newImageView(disk, region, &manifest, true)
	if err != nil {
		return result, err
	}
	defer func() { err = errors.Join(err, view.close()) }()
	image, input := filepath.Join(view.dir, "partition"), filepath.Join(view.dir, "input")
	args := []string{"-q", "-f", "-K", "-L", "pacha-rootfs"}
	if filesystem == "xfs" {
		args = append(args, "-p", input)
	} else {
		args = append(args, "-r", input)
	}
	if span != nil {
		span.Message("formatting " + filesystem + " directly from manifest")
	}
	command := append([]string{"--user", "--map-root-user", "--", "mkfs." + filesystem}, append(args, image)...)
	if err := runFilesystemTool("unshare", command...); err != nil {
		return result, err
	}
	if err := disk.Sync(); err != nil {
		return result, err
	}
	if span != nil {
		span.Message("checking " + filesystem + " in place")
	}
	_, checkArgs, _ := filesystemCheckCommand(filesystem, image)
	if err := runFilesystemTool(checker, checkArgs...); err != nil {
		return result, err
	}
	var bytes uint64
	for _, spec := range manifest.Files {
		if spec.IsSymlink {
			bytes += uint64(len(spec.SymlinkTarget))
		} else if !spec.IsDeviceNode {
			bytes += spec.Size
		}
	}
	return WriteResult{Files: len(manifest.Files), Dirs: len(manifest.Dirs), Bytes: bytes,
		Filesystem: filesystem, FirstLBA: region.FirstLBA, LastLBA: region.LastLBA}, nil
}
