package qemu

import (
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"capabilityos/pack/internal/config"
)

func TestAppendConsoleArgsNewTerminalDefaultEnablesPty(t *testing.T) {
	t.Setenv("XDG_RUNTIME_DIR", t.TempDir())
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}

	args, socketPath, err := appendConsoleArgs(workspace, []string{"qemu-system-x86_64"}, Options{NewTerminal: true})
	if err != nil {
		t.Fatal(err)
	}
	if socketPath == "" {
		t.Fatal("socketPath is empty")
	}
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "virtio-serial-pci") || !strings.Contains(joined, "virtconsole") {
		t.Fatalf("console args missing virtio console devices: %q", joined)
	}
}

func TestAppendConsoleArgsOff(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}

	args, socketPath, err := appendConsoleArgs(workspace, []string{"qemu-system-x86_64"}, Options{Console: "off"})
	if err != nil {
		t.Fatal(err)
	}
	if socketPath != "" {
		t.Fatalf("socketPath = %q, want empty", socketPath)
	}
	if got, want := strings.Join(args, " "), "qemu-system-x86_64"; got != want {
		t.Fatalf("args = %q, want %q", got, want)
	}
}

func TestConsoleTerminalCandidatesPreferWindowsTerminalOnWSL(t *testing.T) {
	t.Setenv("WSL_DISTRO_NAME", "Ubuntu")
	workspace := &config.Workspace{Root: "/home/kamer/os"}

	candidates := consoleTerminalCandidates(workspace, "/tmp/pacgo-console.sh")
	if len(candidates) == 0 {
		t.Fatal("no terminal candidates")
	}
	first := strings.Join(candidates[0], " ")
	for _, want := range []string{"wt.exe", "-w -1", "wsl.exe", "-d Ubuntu", "--cd /home/kamer/os", "bash /tmp/pacgo-console.sh"} {
		if !strings.Contains(first, want) {
			t.Fatalf("first candidate %q does not contain %q", first, want)
		}
	}
	if strings.Contains(first, ";") {
		t.Fatalf("Windows Terminal candidate contains command separators: %q", first)
	}
}

func TestConsoleTerminalScriptPathWritesArtifactScript(t *testing.T) {
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts"}

	scriptPath, err := consoleTerminalScriptPath(workspace, "/tmp/virtio-console.sock", "/tmp/qemu-host-time.log")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(scriptPath, filepath.Join(root, ".artifacts", "qemu")) {
		t.Fatalf("scriptPath = %q, want .artifacts/qemu path", scriptPath)
	}
	info, err := os.Stat(scriptPath)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm()&0o100 == 0 {
		t.Fatalf("script mode = %v, want executable", info.Mode().Perm())
	}
	contentBytes, err := os.ReadFile(scriptPath)
	if err != nil {
		t.Fatal(err)
	}
	content := string(contentBytes)
	for _, want := range []string{"#!/usr/bin/env bash", "sock='/tmp/virtio-console.sock'", "ready_log='/tmp/qemu-host-time.log'", "ready_marker='[seed0boot] hvc console spawn status=0'", "python3 - \"$sock\" <<'PY'", "os.open('/dev/tty', os.O_RDWR)", "reconnecting", "exec bash"} {
		if !strings.Contains(content, want) {
			t.Fatalf("script does not contain %q:\n%s", want, content)
		}
	}
}

func TestRunSendExpectTTY(t *testing.T) {
	socketPath := filepath.Join(t.TempDir(), "console.sock")
	listener, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()

	serverDone := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			serverDone <- err
			return
		}
		defer conn.Close()

		buf := make([]byte, 1024)
		n, err := conn.Read(buf)
		if err != nil {
			serverDone <- err
			return
		}
		if got := string(buf[:n]); !strings.Contains(got, "fastfetch") {
			serverDone <- io.ErrUnexpectedEOF
			return
		}
		_, err = conn.Write([]byte("CapabilityOS\n"))
		serverDone <- err
	}()

	logPath := filepath.Join(t.TempDir(), "console.log")
	logFile, err := os.Create(logPath)
	if err != nil {
		t.Fatal(err)
	}
	defer logFile.Close()

	sent, matched, err := runSendExpectTTY(socketPath, []string{"fastfetch"}, []string{"CapabilityOS"}, time.Second, logFile)
	if err != nil {
		t.Fatal(err)
	}
	if sent != 1 {
		t.Fatalf("sent = %d, want 1", sent)
	}
	if len(matched) != 1 || matched[0] != "CapabilityOS" {
		t.Fatalf("matched = %#v", matched)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
}
