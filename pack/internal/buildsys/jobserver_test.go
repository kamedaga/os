package buildsys

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestFIFOJobserverBoundsRealMake(t *testing.T) {
	makePath, err := exec.LookPath("make")
	if err != nil {
		t.Skip("make is absent")
	}
	if !makeSupportsFIFO(makePath) {
		matches, _ := filepath.Glob("/nix/store/*-gnumake-4.4.1/bin/make")
		if len(matches) == 0 {
			t.Fatalf("GNU make 4.4.1 required for FIFO jobserver; found %s", makePath)
		}
		makePath = matches[0]
	}
	testRealMakeJobserver(t, makePath)
}

func TestReservationHelperReturnsTokensAfterError(t *testing.T) {
	helper, err := filepath.Abs(filepath.Join("..", "..", "scripts", "lib", "jobs.sh"))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("PACGO_BUILD_JOBS", "4")
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, State: ".artifacts/pack"}
	server, err := newJobserver(workspace)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	// A running recipe owns one implicit slot. The helper may borrow the
	// other two immediately available tokens from this four-slot server.
	if err := server.acquire(); err != nil {
		t.Fatal(err)
	}
	defer server.release()

	marker := filepath.Join(root, "traps-ran")
	failing := `set -Eeuo pipefail
trap 'printf E >> "$2"' ERR
trap 'printf X >> "$2"' EXIT
source "$1"
pacgo_reserve_jobs 4
test "$PACGO_RESERVED_JOBS" = 3
false
`
	output, runErr := runReservationScript(server, helper, marker, failing)
	if runErr == nil {
		t.Fatal("reservation script unexpectedly succeeded")
	}
	if string(output) != "3\n" {
		t.Fatalf("reservation output = %q, want 3", output)
	}
	traps, err := os.ReadFile(marker)
	if err != nil {
		t.Fatal(err)
	}
	if string(traps) != "EX" {
		t.Fatalf("chained traps = %q, want EX", traps)
	}

	recovery := `set -Eeuo pipefail
source "$1"
pacgo_reserve_jobs 4
test "$PACGO_RESERVED_JOBS" = 3
`
	output, err = runReservationScript(server, helper, marker, recovery)
	if err != nil {
		t.Fatalf("tokens were not reusable: %v\n%s", err, output)
	}
	if string(output) != "3\n" {
		t.Fatalf("restored reservation output = %q, want 3", output)
	}
}

func runReservationScript(server *jobserver, helper, marker, script string) ([]byte, error) {
	env := environmentMap(os.Environ())
	cmd := exec.Command("bash", "-c", script, "bash", helper, marker)
	server.configureCommand(env)
	cmd.Env = environmentList(env)
	return cmd.CombinedOutput()
}

// TestNinjaHelperBoundsConcurrency checks that Ninja, given pacgo's FIFO
// jobserver, stays within it.
func TestNinjaHelperBoundsConcurrency(t *testing.T) {
	if _, err := exec.LookPath("ninja"); err != nil {
		t.Skip("ninja is absent")
	}
	helper, err := filepath.Abs(filepath.Join("..", "..", "scripts", "lib", "jobs.sh"))
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("PACGO_BUILD_JOBS", "2")
	root := t.TempDir()
	writeConcurrencyProbe(t, root)
	var ninja strings.Builder
	ninja.WriteString("rule job\n  command = ./job.sh && touch $out\n")
	for index := 1; index <= 8; index++ {
		fmt.Fprintf(&ninja, "build j%d: job\n", index)
	}
	if err := os.WriteFile(filepath.Join(root, "build.ninja"), []byte(ninja.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	server, err := newJobserver(&config.Workspace{Root: root, State: ".artifacts/pack"})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	env := environmentMap(os.Environ())
	cmd := exec.Command("bash", "-c", `set -euo pipefail; source "$1"; pacgo_ninja 8`, "bash", helper)
	cmd.Dir = root
	server.configureCommand(env)
	cmd.Env = environmentList(env)
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("ninja failed: %v\n%s", err, output)
	}
	if strings.Contains(string(output), "Ignoring jobserver") {
		t.Fatalf("ninja ran outside the jobserver:\n%s", output)
	}
	if maximum := readConcurrencyMaximum(t, root); maximum != 2 {
		t.Fatalf("maximum concurrency = %d, want 2; ninja output:\n%s", maximum, output)
	}
}

func writeConcurrencyProbe(t *testing.T, root string) {
	t.Helper()
	script := `#!/bin/sh
set -eu
exec 9>>lock
flock 9
count=$(cat count)
count=$((count + 1))
printf '%s\n' "$count" > count
maximum=$(cat maximum)
if [ "$count" -gt "$maximum" ]; then printf '%s\n' "$count" > maximum; fi
flock -u 9
sleep .05
flock 9
count=$(cat count)
printf '%s\n' $((count - 1)) > count
`
	if err := os.WriteFile(filepath.Join(root, "job.sh"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "count"), []byte("0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "maximum"), []byte("0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readConcurrencyMaximum(t *testing.T, root string) int {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(root, "maximum"))
	if err != nil {
		t.Fatal(err)
	}
	maximum, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil {
		t.Fatal(err)
	}
	return maximum
}

func testRealMakeJobserver(t *testing.T, makePath string) {
	t.Helper()
	t.Setenv("PACGO_BUILD_JOBS", "2")
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, State: ".artifacts/pack"}
	writeConcurrencyProbe(t, root)
	var makefile strings.Builder
	makefile.WriteString(".PHONY: all ")
	for index := 1; index <= 8; index++ {
		fmt.Fprintf(&makefile, "j%d ", index)
	}
	makefile.WriteString("\nall:")
	for index := 1; index <= 8; index++ {
		fmt.Fprintf(&makefile, " j%d", index)
	}
	makefile.WriteByte('\n')
	for index := 1; index <= 8; index++ {
		fmt.Fprintf(&makefile, "j%d:\n\t./job.sh\n", index)
	}
	if err := os.WriteFile(filepath.Join(root, "Makefile"), []byte(makefile.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	server, err := newJobserver(workspace)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	env := environmentMap(os.Environ())
	cmd := exec.Command(makePath, "all")
	cmd.Dir = root
	server.configureCommand(env)
	cmd.Env = environmentList(env)
	var output bytes.Buffer
	cmd.Stdout = &output
	cmd.Stderr = &output
	if err := cmd.Run(); err != nil {
		t.Fatalf("make failed: %v\n%s", err, output.String())
	}
	if maximum := readConcurrencyMaximum(t, root); maximum != 2 {
		t.Fatalf("maximum concurrency = %d, want 2; make output:\n%s", maximum, output.String())
	}
}

func makeSupportsFIFO(path string) bool {
	output, err := exec.Command(path, "--version").Output()
	if err != nil {
		return false
	}
	first := strings.SplitN(string(output), "\n", 2)[0]
	return strings.Contains(first, " 4.4") || strings.Contains(first, " 4.5") || strings.Contains(first, " 5.")
}
