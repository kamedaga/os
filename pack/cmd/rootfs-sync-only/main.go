package main

import (
	"fmt"
	"os"
	"path/filepath"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/rootsync"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run() error {
	root, err := config.FindRoot(".")
	if err != nil {
		return err
	}
	workspace, err := config.Load(root)
	if err != nil {
		return err
	}
	var changed []string
	for _, arg := range os.Args[1:] {
		abs, err := filepath.Abs(arg)
		if err != nil {
			return err
		}
		changed = append(changed, filepath.Clean(abs))
	}
	result, err := rootsync.SyncRootfs(workspace, workspace.Path(workspace.Manifests.Rootfs), rootsync.Options{
		ChangedSources: changed,
	})
	if err != nil {
		return err
	}
	state := "synced"
	if result.Skipped {
		state = "up-to-date"
	}
	fmt.Printf("%s files=%d dirs=%d updated=%d bytes=%d clusters=%d\n", state, result.Files, result.Dirs, result.Updated, result.Bytes, result.Clusters)
	return nil
}
