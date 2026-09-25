package rootsync

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// Exercise the actual image writer: debugfs can truncate a trailing hole
// even though every write command and e2fsck succeeds.
func TestExt4SparseTailPreservesEOF(t *testing.T) {
	for _, tool := range []string{"mkfs.ext4", "debugfs"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skip(err)
		}
	}
	dir := t.TempDir()
	image, source, commands, output := filepath.Join(dir, "test.ext4"), filepath.Join(dir, "source"), filepath.Join(dir, "commands"), filepath.Join(dir, "output")
	check := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	check(os.WriteFile(image, nil, 0600))
	check(os.Truncate(image, 32*1024*1024))
	check(runExt4Tool("mkfs.ext4", "-q", "-F", "-b", "4096", "-O", "inline_data", image))
	check(os.WriteFile(source, bytes.Repeat([]byte{0x5a}, 4096), 0600))
	check(os.Truncate(source, 4104))
	_, err := writeDebugFSCommands(commands, Manifest{Files: []FileSpec{{ImagePath: "/probe", SourcePath: source, Size: 4104}}}, nil)
	check(err)
	check(runExt4Tool("debugfs", "-w", "-f", commands, image))
	check(runExt4Tool("debugfs", "-R", "dump /probe "+debugFSQuote(output), image))
	want, err := os.ReadFile(source)
	check(err)
	got, err := os.ReadFile(output)
	check(err)
	if !bytes.Equal(got, want) {
		t.Fatalf("image content differs: got %d bytes, want %d", len(got), len(want))
	}
}
