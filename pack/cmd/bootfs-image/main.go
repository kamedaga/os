// SPDX-License-Identifier: MIT
// Build a boot archive from an explicit manifest without syncing any disk.
package main

import (
	"flag"
	"fmt"
	"os"

	"capabilityos/pack/internal/bootfs"
)

func main() {
	manifest := flag.String("manifest", "", "bootfs manifest")
	output := flag.String("output", "", "output archive")
	flag.Parse()
	if *manifest == "" || *output == "" {
		flag.Usage()
		os.Exit(2)
	}
	_, err := bootfs.BuildImageWithOptions(*manifest, *output, bootfs.Options{})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
