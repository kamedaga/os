package main

import (
	limine "capabilityos/pack/internal/bootloader/limine"
	"capabilityos/pack/internal/buildsys"
	"capabilityos/pack/internal/config"
	"fmt"
	"os"
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: pager-image REPO_ROOT OUTPUT_DIR")
		os.Exit(2)
	}
	root := os.Args[1]
	ws, err := config.Load(root)
	if err != nil {
		panic(err)
	}
	out := os.Args[2] + "/"
	result, err := limine.BuildImageWithOptions(ws, limine.Inputs{
		KernelELF:   buildsys.KernelOutputPath(ws, "limine"),
		InitELF:     out + "native_pager_fault.elf",
		BootfsImage: out + "bootfs.img",
	}, limine.Options{ImagePath: out + "boot.img", ConfigPath: out + "limine.conf"})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(result.Image)
}
