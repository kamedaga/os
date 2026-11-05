//go:build !linux

package rootsync

import (
	"capabilityos/pack/internal/progress"
	"fmt"
	"os"
)

func CheckFilesystem(disk *os.File, region PartitionRegion, filesystem string) error {
	return fmt.Errorf("in-place filesystem checks require Linux with FUSE")
}
func WriteRootlessWithProgress(disk *os.File, region PartitionRegion, filesystem string, manifest Manifest, span progress.Span) (WriteResult, error) {
	return WriteResult{}, fmt.Errorf("rootless %s creation requires Linux with FUSE", filesystem)
}
