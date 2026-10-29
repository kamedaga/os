package buildsys

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"

	"capabilityos/pack/internal/config"
)

type jobserver struct {
	path string
	file *os.File
	jobs int
}

func newJobserver(workspace *config.Workspace) (*jobserver, error) {
	jobs := runtime.NumCPU()
	if value := os.Getenv("PACGO_BUILD_JOBS"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 1 {
			return nil, fmt.Errorf("PACGO_BUILD_JOBS must be a positive integer, got %q", value)
		}
		jobs = parsed
	}
	directory := workspace.Path(workspace.State)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return nil, err
	}
	tmp, err := os.CreateTemp(directory, ".jobserver-*.fifo")
	if err != nil {
		return nil, err
	}
	path := tmp.Name()
	if err := tmp.Close(); err != nil {
		return nil, err
	}
	if err := os.Remove(path); err != nil {
		return nil, err
	}
	if err := syscall.Mkfifo(path, 0o600); err != nil {
		return nil, err
	}
	file, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		_ = os.Remove(path)
		return nil, err
	}
	server := &jobserver{path: filepath.Clean(path), file: file, jobs: jobs}
	if jobs > 1 {
		tokens := []byte(strings.Repeat("+", jobs-1))
		if _, err := file.Write(tokens); err != nil {
			_ = server.Close()
			return nil, err
		}
	}
	return server, nil
}

func (j *jobserver) acquire() error {
	var token [1]byte
	_, err := io.ReadFull(j.file, token[:])
	return err
}

func (j *jobserver) release() error {
	_, err := j.file.Write([]byte{'+'})
	return err
}

// configureCommand hands a recipe the FIFO jobserver. Recipes run in the
// devShell, whose GNU make (4.4) and Ninja (1.13) both join a FIFO jobserver.
func (j *jobserver) configureCommand(cmdEnv map[string]string) {
	base := cleanMakeflags(cmdEnv["MAKEFLAGS"])
	auth := "--jobserver-auth=fifo:" + j.path
	cmdEnv["MAKEFLAGS"] = strings.TrimSpace(strings.Join([]string{base, "-j", auth}, " "))
}

func cleanMakeflags(flags string) string {
	fields := strings.Fields(flags)
	kept := make([]string, 0, len(fields))
	for index := 0; index < len(fields); index++ {
		field := fields[index]
		if field == "-j" || strings.HasPrefix(field, "-j") || field == "--jobs" || strings.HasPrefix(field, "--jobs=") || field == "--jobserver-auth" || strings.HasPrefix(field, "--jobserver-auth=") || field == "--jobserver-fds" || strings.HasPrefix(field, "--jobserver-fds=") {
			if (field == "--jobs" || field == "--jobserver-auth" || field == "--jobserver-fds") && index+1 < len(fields) {
				index++
			}
			continue
		}
		kept = append(kept, field)
	}
	return strings.Join(kept, " ")
}

func (j *jobserver) Close() error {
	var first error
	if j.file != nil {
		if err := j.file.Close(); err != nil {
			first = err
		}
		j.file = nil
	}
	if err := os.Remove(j.path); err != nil && !os.IsNotExist(err) && first == nil {
		first = err
	}
	return first
}
