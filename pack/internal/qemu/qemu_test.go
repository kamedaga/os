package qemu

import (
	"io"
	"net"
	"os"
	"os/exec"
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

func TestNormalizeCPUCount(t *testing.T) {
	for _, test := range []struct {
		name    string
		input   int
		want    int
		wantErr bool
	}{
		{name: "default", input: 0, want: 4},
		{name: "minimum", input: 1, want: 1},
		{name: "maximum", input: 256, want: 256},
		{name: "negative", input: -1, wantErr: true},
		{name: "too large", input: 257, wantErr: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			got, err := normalizeCPUCount(test.input)
			if test.wantErr {
				if err == nil {
					t.Fatalf("normalizeCPUCount(%d) unexpectedly succeeded with %d", test.input, got)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if got != test.want {
				t.Fatalf("normalizeCPUCount(%d) = %d, want %d", test.input, got, test.want)
			}
		})
	}
}

func TestDefaultCPUModelFitsKernelXStatePolicy(t *testing.T) {
	for _, unsupported := range []string{"avx512"} {
		if strings.Contains(defaultCPUModel, unsupported) {
			t.Fatalf("default CPU model %q exceeds the kernel xstate policy", defaultCPUModel)
		}
	}
	for _, required := range []string{"ssse3", "sse4.1", "sse4.2", "popcnt", "xsave", "avx", "avx2"} {
		if !strings.Contains(defaultCPUModel, required) {
			t.Fatalf("default CPU model %q is missing %q", defaultCPUModel, required)
		}
	}
}

func TestAppendInputDeviceArgs(t *testing.T) {
	tests := []struct {
		name    string
		profile string
		want    string
		wantErr bool
	}{
		{
			name: "default",
			want: "qemu -device virtio-keyboard-pci,disable-legacy=on,id=pachakbd -device virtio-mouse-pci,disable-legacy=on,id=pachamouse",
		},
		{
			name:    "keyboard mouse",
			profile: "keyboard-mouse",
			want:    "qemu -device virtio-keyboard-pci,disable-legacy=on,id=pachakbd -device virtio-mouse-pci,disable-legacy=on,id=pachamouse",
		},
		{
			name:    "keyboard tablet",
			profile: "keyboard-tablet",
			want:    "qemu -device virtio-keyboard-pci,disable-legacy=on,id=pachakbd -device virtio-tablet-pci,disable-legacy=on,id=pachatablet",
		},
		{
			name:    "mouse keyboard order",
			profile: "mouse-keyboard",
			want:    "qemu -device virtio-mouse-pci,disable-legacy=on,id=pachamouse -device virtio-keyboard-pci,disable-legacy=on,id=pachakbd",
		},
		{name: "unknown", profile: "keyboard-trackball", wantErr: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			args, err := appendInputDeviceArgs([]string{"qemu"}, test.profile)
			if test.wantErr {
				if err == nil {
					t.Fatalf("appendInputDeviceArgs(%q) unexpectedly succeeded with %q", test.profile, strings.Join(args, " "))
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if got := strings.Join(args, " "); got != test.want {
				t.Fatalf("args = %q, want %q", got, test.want)
			}
		})
	}
}

func TestAppendGraphicsDeviceArgs(t *testing.T) {
	tests := []struct {
		name        string
		profile     string
		display     string
		wantArgs    string
		wantDisplay string
		wantErr     bool
	}{
		{
			name:        "default 2d",
			wantArgs:    "qemu -device virtio-gpu-pci,disable-legacy=on,iommu_platform=on,id=pachagpu",
			wantDisplay: "none",
			display:     "none",
		},
		{
			name:        "virgl gtk enables gl",
			profile:     "virgl",
			display:     "gtk,show-tabs=on",
			wantArgs:    "qemu -device virtio-gpu-gl-pci,disable-legacy=on,iommu_platform=on,id=pachagpu",
			wantDisplay: "gtk,show-tabs=on,gl=on",
		},
		{
			name:        "virgl headless",
			profile:     "virgl",
			display:     "none",
			wantArgs:    "qemu -device virtio-gpu-gl-pci,disable-legacy=on,iommu_platform=on,id=pachagpu",
			wantDisplay: "egl-headless,gl=on",
		},
		{name: "virgl explicit gl off", profile: "virgl", display: "gtk,gl=off", wantErr: true},
		{name: "unknown", profile: "vulkan", display: "none", wantErr: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			args, display, err := appendGraphicsDeviceArgs([]string{"qemu"}, test.profile, test.display)
			if test.wantErr {
				if err == nil {
					t.Fatalf("appendGraphicsDeviceArgs(%q, %q) unexpectedly succeeded", test.profile, test.display)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if got := strings.Join(args, " "); got != test.wantArgs {
				t.Fatalf("args = %q, want %q", got, test.wantArgs)
			}
			if display != test.wantDisplay {
				t.Fatalf("display = %q, want %q", display, test.wantDisplay)
			}
		})
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
	for _, want := range []string{"#!/usr/bin/env bash", "sock='/tmp/virtio-console.sock'", "ready_log='/tmp/qemu-host-time.log'", "ready_marker='[termd] linux tty hvc open ready index=0 handle='", "python3 - \"$sock\" <<'PY'", "os.open('/dev/tty', os.O_RDWR)", "reconnecting", "exec bash"} {
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

func TestWaitForSocketOrExitBroadcastsImmediateExit(t *testing.T) {
	cmd := exec.Command("/bin/sh", "-c", "exit 17")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	wait := watchProcess(cmd)
	start := time.Now()
	for _, socket := range []string{
		filepath.Join(t.TempDir(), "console.sock"),
		filepath.Join(t.TempDir(), "qmp.sock"),
	} {
		exited, err := waitForSocketOrExit(socket, wait, 5*time.Second)
		if !exited || err == nil {
			t.Fatalf("waitForSocketOrExit(%q) = exited=%v err=%v, want immediate process exit", socket, exited, err)
		}
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Fatalf("repeated process-exit observation took %s", elapsed)
	}
}

func TestSmokeImmediateExitDoesNotWaitForBootTimeout(t *testing.T) {
	root := t.TempDir()
	artifacts := filepath.Join(root, ".artifacts")
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"limine-boot.img", "disk.img"} {
		if err := os.WriteFile(filepath.Join(artifacts, name), []byte("image"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	fakeQEMU := filepath.Join(root, "fake-qemu.sh")
	if err := os.WriteFile(fakeQEMU, []byte("#!/bin/sh\nexit 17\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CAPOS_QEMU", fakeQEMU)
	workspace := &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		Disk: config.Disk{
			Image: ".artifacts/disk.img",
		},
	}
	start := time.Now()
	_, err := Smoke(workspace, SmokeOptions{Timeout: 5 * time.Second, NoKVM: true, NoNet: true})
	if err == nil || !strings.Contains(err.Error(), "qemu exited before smoke marker") {
		t.Fatalf("Smoke immediate exit error = %v", err)
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Fatalf("Smoke waited %s after immediate QEMU exit", elapsed)
	}
}

func TestRunCleanExitBeforeConsoleSocketFailsImmediately(t *testing.T) {
	root := t.TempDir()
	artifacts := filepath.Join(root, ".artifacts")
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"limine-boot.img", "disk.img"} {
		if err := os.WriteFile(filepath.Join(artifacts, name), []byte("image"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	fakeQEMU := filepath.Join(root, "fake-qemu.sh")
	if err := os.WriteFile(fakeQEMU, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CAPOS_QEMU", fakeQEMU)
	t.Setenv("XDG_RUNTIME_DIR", t.TempDir())
	workspace := &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		Disk:      config.Disk{Image: ".artifacts/disk.img"},
	}
	start := time.Now()
	_, err := Run(workspace, Options{Console: "pty", NoKVM: true, NoNet: true})
	if err == nil || !strings.Contains(err.Error(), "qemu exited before socket was ready") {
		t.Fatalf("Run clean immediate exit error = %v", err)
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Fatalf("Run waited %s after clean QEMU exit", elapsed)
	}
}

func TestUEFIDryRunDoesNotModifyVars(t *testing.T) {
	workspace, template := testUEFIWorkspace(t)
	varsPath := filepath.Join(workspace.Root, workspace.Artifacts, "OVMF_LIMINE_VARS.fd")
	if err := os.WriteFile(varsPath, []byte("existing vars"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Run(workspace, Options{
		Firmware: "uefi",
		Console:  "off",
		NoKVM:    true,
		NoNet:    true,
		DryRun:   true,
	}); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(varsPath)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(data), "existing vars"; got != want {
		t.Fatalf("dry run modified vars = %q, want %q (template %q)", got, want, template)
	}
}

func TestUEFIPlanPreparesVarsOnlyAfterImageLock(t *testing.T) {
	workspace, template := testUEFIWorkspace(t)
	varsPath := filepath.Join(workspace.Root, workspace.Artifacts, "OVMF_LIMINE_VARS.fd")
	plan, err := commandArgs(workspace, Options{Firmware: "uefi", Console: "off", NoKVM: true, NoNet: true})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(varsPath); !os.IsNotExist(err) {
		t.Fatalf("command planning created vars file: %v", err)
	}
	if !containsPath(plan.ImagePaths, varsPath) {
		t.Fatalf("UEFI vars path %q is not locked with image paths %#v", varsPath, plan.ImagePaths)
	}
	locks, err := lockPlanImages(plan)
	if err != nil {
		t.Fatal(err)
	}
	defer locks.Close()
	if err := preparePlan(plan); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(varsPath)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(data), template; got != want {
		t.Fatalf("prepared vars = %q, want template %q", got, want)
	}
}

func testUEFIWorkspace(t *testing.T) (*config.Workspace, string) {
	t.Helper()
	root := t.TempDir()
	artifacts := filepath.Join(root, ".artifacts")
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"limine-boot.img", "disk.img"} {
		if err := os.WriteFile(filepath.Join(artifacts, name), []byte(name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	codePath := filepath.Join(root, "OVMF_CODE.fd")
	varsTemplate := filepath.Join(root, "OVMF_VARS.fd")
	if err := os.WriteFile(codePath, []byte("code"), 0o644); err != nil {
		t.Fatal(err)
	}
	const template = "fresh vars"
	if err := os.WriteFile(varsTemplate, []byte(template), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CAPOS_OVMF_CODE", codePath)
	t.Setenv("CAPOS_OVMF_VARS_TEMPLATE", varsTemplate)
	return &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		Disk:      config.Disk{Image: ".artifacts/disk.img"},
	}, template
}

func containsPath(paths []string, wanted string) bool {
	for _, path := range paths {
		if path == wanted {
			return true
		}
	}
	return false
}

func TestParseScreendumpCheck(t *testing.T) {
	artifacts := t.TempDir()
	check, err := parseScreendumpCheck("FRAME_READY@8,9,10,11=#12abef:3", 1, artifacts)
	if err != nil {
		t.Fatal(err)
	}
	if check.Marker != "FRAME_READY" || check.X != 8 || check.Y != 9 || check.Width != 10 || check.Height != 11 {
		t.Fatalf("unexpected region: %#v", check)
	}
	if check.Red != 0x12 || check.Green != 0xab || check.Blue != 0xef || check.Tolerance != 3 {
		t.Fatalf("unexpected color: %#v", check)
	}
	if check.Path != filepath.Join(artifacts, "screendump-02.ppm") {
		t.Fatalf("path = %q", check.Path)
	}
}

func TestParseInputSendCheck(t *testing.T) {
	check, err := parseInputSendCheck("INPUT_READY@key:a=down,key:a=up,rel:x=-7,btn:left=down")
	if err != nil {
		t.Fatal(err)
	}
	if check.Marker != "INPUT_READY" || len(check.Patterns) != 1 || len(check.Patterns[0]) != 4 {
		t.Fatalf("unexpected check: %#v", check)
	}
	events := check.Patterns[0]
	if events[0].Kind != "key" || !events[0].Down || events[1].Down {
		t.Fatalf("unexpected key events: %#v", events[:2])
	}
	if events[2].Kind != "rel" || events[2].Code != "x" || events[2].Value != -7 {
		t.Fatalf("unexpected relative event: %#v", events[2])
	}
}

func TestSplitInputSendEventFramesPreservesDeviceFrames(t *testing.T) {
	events := []inputSendEvent{
		{Kind: "key", Code: "a", Down: true},
		{Kind: "key", Code: "a", Down: false},
		{Kind: "rel", Code: "x", Value: 7},
		{Kind: "rel", Code: "y", Value: -4},
		{Kind: "btn", Code: "left", Down: true},
		{Kind: "btn", Code: "left", Down: false},
	}
	frames, err := splitInputSendEventFrames(events)
	if err != nil {
		t.Fatal(err)
	}
	if len(frames) != 2 || len(frames[0]) != 2 || len(frames[1]) != 4 {
		t.Fatalf("unexpected input frames: %#v", frames)
	}
	if frames[0][0].Kind != "key" || frames[1][0].Kind != "rel" ||
		frames[1][3].Kind != "btn" {
		t.Fatalf("input frame order changed: %#v", frames)
	}
}

func TestValidatePPMRegion(t *testing.T) {
	path := filepath.Join(t.TempDir(), "surface.ppm")
	pixels := make([]byte, 4*3*3)
	for i := 0; i < len(pixels); i += 3 {
		pixels[i] = 0x20
		pixels[i+1] = 0x40
		pixels[i+2] = 0x60
	}
	data := append([]byte("P6\n4 3\n255\n"), pixels...)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	check := screendumpCheck{Marker: "FRAME", X: 1, Y: 1, Width: 2, Height: 1, Red: 0x20, Green: 0x40, Blue: 0x60, Path: path}
	if err := validatePPMRegion(check); err != nil {
		t.Fatal(err)
	}
	check.Red = 0xff
	if err := validatePPMRegion(check); err == nil {
		t.Fatal("wrong expected color passed")
	}
}
