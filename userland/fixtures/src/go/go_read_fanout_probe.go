package main

import (
	"os"
	"runtime"
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

func worker(path string, sem chan struct{}, done chan error) {
	defer func() {
		<-sem
		close(done)
	}()
	data, err := os.ReadFile(path)
	if err != nil {
		done <- err
		return
	}
	if len(data) == 0 {
		done <- os.ErrInvalid
	}
}

func main() {
	sem := make(chan struct{}, runtime.GOMAXPROCS(0)+10)
	done := make([]chan error, len(files))
	for i := range done {
		done[i] = make(chan error)
	}
	go func() {
		for i, path := range files {
			sem <- struct{}{}
			go worker(path, sem, done[i])
		}
	}()
	for _, ch := range done {
		for err := range ch {
			if err != nil {
				println("ERR", err.Error())
				os.Exit(1)
			}
		}
	}
	println("GOREAD_OK")
}
