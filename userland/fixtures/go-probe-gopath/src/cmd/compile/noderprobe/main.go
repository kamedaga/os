package main

import (
	"cmd/compile/internal/base"
	"cmd/compile/internal/noder"
)

var files = []string{
	"/usr/lib/go/src/internal/abi/abi.go",
	"/usr/lib/go/src/internal/abi/abi_amd64.go",
	"/usr/lib/go/src/internal/abi/compiletype.go",
	"/usr/lib/go/src/internal/abi/escape.go",
	"/usr/lib/go/src/internal/abi/funcpc.go",
	"/usr/lib/go/src/internal/abi/iface.go",
	"/usr/lib/go/src/internal/abi/map_noswiss.go",
	"/usr/lib/go/src/internal/abi/map_select_swiss.go",
	"/usr/lib/go/src/internal/abi/map_swiss.go",
	"/usr/lib/go/src/internal/abi/rangefuncconsts.go",
	"/usr/lib/go/src/internal/abi/runtime.go",
	"/usr/lib/go/src/internal/abi/stack.go",
	"/usr/lib/go/src/internal/abi/switch.go",
	"/usr/lib/go/src/internal/abi/symtab.go",
	"/usr/lib/go/src/internal/abi/type.go",
}

func main() {
	base.Flag.Std = true
	noder.LoadPackage(files)
	println("GONODER_OK")
}
