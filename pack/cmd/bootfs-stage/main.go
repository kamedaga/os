// SPDX-License-Identifier: MIT
// bootfs-stage builds an immutable boot archive without synchronizing a disk.
package main

import (
	"flag"
	"fmt"
	"os"

	"capabilityos/pack/internal/bootfs"
)

func main() {
	manifest := flag.String("manifest", "", "selected bootfs manifest")
	output := flag.String("output", "", "new archive output (must not exist)")
	disk := flag.String("disk", "", "configured raw disk, opened read-only for root UUID")
	partition := flag.Int("root-partition", 2, "configured root partition index")
	ram := flag.Uint64("ram", 512<<20, "sandbox memory policy in bytes")
	depth := flag.Uint64("queue-size", 64, "split-ring depth")
	arena := flag.Uint64("arena", 16<<20, "normal queue arena bytes")
	flag.Parse()
	if *manifest == "" || *output == "" || *disk == "" || flag.NArg() != 0 {
		flag.Usage()
		os.Exit(2)
	}
	if _, err := os.Stat(*output); !os.IsNotExist(err) {
		fmt.Fprintln(os.Stderr, "refusing to replace output:", *output)
		os.Exit(1)
	}
	result, err := bootfs.BuildImageWithOptions(*manifest, *output, bootfs.Options{
		Storage: &bootfs.StorageOptions{DiskPath: *disk, RootPartition: *partition,
			RAMBytes: *ram, QueueSize: *depth, ArenaBytes: *arena}})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("bootfs-stage: %d files, %d bytes -> %s\n", result.Entries, result.Bytes, result.Path)
}
