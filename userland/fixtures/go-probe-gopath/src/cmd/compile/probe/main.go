package main

import (
	"cmd/compile/internal/syntax"
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

type parsedFile struct {
	file *syntax.File
	err  chan syntax.Error
}

func (p *parsedFile) error(err error) {
	p.err <- err.(syntax.Error)
}

func main() {
	sem := make(chan struct{}, runtime.GOMAXPROCS(0)+10)
	parsed := make([]*parsedFile, len(files))
	for i := range parsed {
		parsed[i] = &parsedFile{err: make(chan syntax.Error)}
	}
	go func() {
		for i, filename := range files {
			filename := filename
			p := parsed[i]
			sem <- struct{}{}
			go func() {
				defer func() { <-sem }()
				defer close(p.err)
				fbase := syntax.NewFileBase(filename)
				f, err := os.Open(filename)
				if err != nil {
					p.error(syntax.Error{Msg: err.Error()})
					return
				}
				defer f.Close()
				p.file, _ = syntax.Parse(fbase, f, p.error, func(pos syntax.Pos, blank bool, text string, current syntax.Pragma) syntax.Pragma {
					return current
				}, syntax.CheckBranches)
			}()
		}
	}()
	var lines uint
	for _, p := range parsed {
		for e := range p.err {
			println("ERR", e.Error())
			os.Exit(1)
		}
		if p.file == nil {
			println("ERR nil file")
			os.Exit(1)
		}
		lines += p.file.EOF.Line()
	}
	println("GOSYNTAX_OK", lines)
}
