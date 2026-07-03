package qemu

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

type Options struct {
	Memory      string
	Display     string
	Console     string
	Firmware    string
	DiskImage   string
	DiskFormat  string
	NoKVM       bool
	NoNet       bool
	Fast        bool
	DryRun      bool
	ExtraArgs   []string
	NewTerminal bool
	Prepare     bool
	NoBuild     bool
	LimineImage string
	Progress    progress.Reporter
}

type Result struct {
	Command        []string
	ConsoleCommand []string
	ConsoleSocket  string
	Log            string
	HostTimeLog    string
	DryRun         bool
	Started        bool
}

type commandPlan struct {
	Args          []string
	LogPath       string
	HostTimeLog   string
	ConsoleSocket string
}

type SmokeOptions struct {
	Timeout     time.Duration
	NoKVM       bool
	NoNet       bool
	LimineImage string
	DiskImage   string
	DiskFormat  string
	ExtraArgs   []string
	Marker      string
	Progress    progress.Reporter
}

type SmokeResult struct {
	Command []string
	Serial  string
	Log     string
	Marker  string
	Timeout time.Duration
	Matched bool
}

type TTYTestOptions struct {
	Timeout    time.Duration
	NoKVM      bool
	ExtraArgs  []string
	BootMarker string
	Send       []string
	Expect     []string
	Python     string
	Progress   progress.Reporter
}

type TTYTestResult struct {
	Command       []string
	ConsoleSocket string
	Serial        string
	Console       string
	Log           string
	PythonLog     string
	BootMarker    string
	Timeout       time.Duration
	Sent          int
	Expected      []string
	Matched       []string
	Python        string
}

type hostTimeLog struct {
	file    *os.File
	started time.Time
	mu      sync.Mutex
}

type hostTimeLineWriter struct {
	log    *hostTimeLog
	stream string
	buf    []byte
}

func newHostTimeLineWriter(log *hostTimeLog, stream string) *hostTimeLineWriter {
	return &hostTimeLineWriter{log: log, stream: stream}
}

func (w *hostTimeLineWriter) Write(p []byte) (int, error) {
	if w == nil || w.log == nil || w.log.file == nil {
		return len(p), nil
	}
	consumed := len(p)
	for len(p) > 0 {
		next := -1
		for i, b := range p {
			if b == '\n' {
				next = i
				break
			}
		}
		if next < 0 {
			w.buf = append(w.buf, p...)
			break
		}
		w.buf = append(w.buf, p[:next]...)
		w.writeLine(w.buf)
		w.buf = w.buf[:0]
		p = p[next+1:]
	}
	return consumed, nil
}

func (w *hostTimeLineWriter) Close() error {
	if w == nil || len(w.buf) == 0 {
		return nil
	}
	w.writeLine(w.buf)
	w.buf = w.buf[:0]
	return nil
}

func (w *hostTimeLineWriter) writeLine(line []byte) {
	now := time.Now()
	text := strings.TrimRight(string(line), "\r")
	w.log.mu.Lock()
	defer w.log.mu.Unlock()
	_, _ = fmt.Fprintf(
		w.log.file,
		"%s +%s %s | %s\n",
		now.Format("2006-01-02T15:04:05.000000000-07:00"),
		now.Sub(w.log.started).Round(time.Microsecond),
		w.stream,
		text,
	)
}

func Run(workspace *config.Workspace, opts Options) (Result, error) {
	span := progress.Use(opts.Progress).Start("qemu", 4)
	defer span.Close()
	span.Set(1, "building qemu command")
	plan, err := commandArgs(workspace, opts)
	if err != nil {
		span.Fail("qemu command failed")
		return Result{}, err
	}
	var consoleArgs []string
	if opts.NewTerminal {
		if opts.DryRun {
			span.Set(2, "preparing console preview")
			consoleArgs = consoleTerminalPreview(workspace, plan.ConsoleSocket)
		} else {
			span.Set(2, "preparing console terminal")
			consoleArgs, err = consoleTerminalCommand(workspace, plan.ConsoleSocket)
			if err != nil {
				span.Fail("console terminal failed")
				return Result{}, err
			}
		}
	}
	if opts.DryRun {
		span.Done("qemu dry-run ready")
		return Result{Command: plan.Args, ConsoleCommand: consoleArgs, ConsoleSocket: plan.ConsoleSocket, Log: plan.LogPath, HostTimeLog: plan.HostTimeLog, DryRun: true}, nil
	}
	span.Set(3, "starting qemu")
	hostLogFile, err := os.Create(plan.HostTimeLog)
	if err != nil {
		span.Fail("host timestamp log create failed")
		return Result{}, err
	}
	defer hostLogFile.Close()
	hostLog := &hostTimeLog{file: hostLogFile, started: time.Now()}
	_, _ = fmt.Fprintf(hostLogFile, "%s +0s host | qemu start\n", hostLog.started.Format("2006-01-02T15:04:05.000000000-07:00"))
	stdoutLog := newHostTimeLineWriter(hostLog, "stdout")
	stderrLog := newHostTimeLineWriter(hostLog, "stderr")
	defer stdoutLog.Close()
	defer stderrLog.Close()
	cmd := exec.Command(plan.Args[0], plan.Args[1:]...)
	cmd.Dir = workspace.Root
	cmd.Stdin = os.Stdin
	cmd.Stdout = io.MultiWriter(os.Stdout, stdoutLog)
	cmd.Stderr = io.MultiWriter(os.Stderr, stderrLog)
	if err := cmd.Start(); err != nil {
		span.Fail("qemu start failed")
		return Result{}, err
	}
	done := make(chan error, 1)
	go func() {
		done <- cmd.Wait()
	}()
	if plan.ConsoleSocket != "" {
		span.Set(4, "waiting for virtio-console socket")
		if exited, err := waitForSocketOrExit(plan.ConsoleSocket, done, 5*time.Second); err != nil {
			if !exited && cmd.Process != nil {
				_ = cmd.Process.Kill()
				<-done
			}
			span.Fail("virtio-console socket failed")
			return Result{}, err
		} else if exited {
			span.Done("qemu exited")
			return Result{Command: plan.Args, ConsoleCommand: consoleArgs, ConsoleSocket: plan.ConsoleSocket, Log: plan.LogPath, HostTimeLog: plan.HostTimeLog}, nil
		}
		span.Message("starting console terminal")
		consoleCmd := exec.Command(consoleArgs[0], consoleArgs[1:]...)
		consoleCmd.Dir = workspace.Root
		if err := consoleCmd.Start(); err != nil {
			_ = cmd.Process.Kill()
			<-done
			span.Fail("console terminal start failed")
			return Result{}, err
		}
	}
	span.Done("qemu started")
	if err := <-done; err != nil {
		return Result{}, err
	}
	return Result{Command: plan.Args, ConsoleCommand: consoleArgs, ConsoleSocket: plan.ConsoleSocket, Log: plan.LogPath, HostTimeLog: plan.HostTimeLog}, nil
}

func Smoke(workspace *config.Workspace, opts SmokeOptions) (SmokeResult, error) {
	span := progress.Use(opts.Progress).Start("qemu smoke", 4)
	defer span.Close()
	if opts.Timeout <= 0 {
		opts.Timeout = 45 * time.Second
	}
	if opts.Marker == "" {
		opts.Marker = "[seed2_root] manifest scheduler done"
	}
	span.Set(1, "building qemu command")
	plan, err := commandArgs(workspace, Options{
		Display:     "none",
		Console:     "pty",
		NoKVM:       opts.NoKVM,
		NoNet:       opts.NoNet,
		Fast:        true,
		LimineImage: opts.LimineImage,
		DiskImage:   opts.DiskImage,
		DiskFormat:  opts.DiskFormat,
		ExtraArgs:   opts.ExtraArgs,
	})
	if err != nil {
		span.Fail("qemu command failed")
		return SmokeResult{}, err
	}
	serialPath := filepath.Join(workspace.Path(workspace.Artifacts), "serial-smoke.log")
	if err := os.MkdirAll(filepath.Dir(serialPath), 0o755); err != nil {
		span.Fail("serial log directory failed")
		return SmokeResult{}, err
	}
	serialFile, err := os.Create(serialPath)
	if err != nil {
		span.Fail("serial log create failed")
		return SmokeResult{}, err
	}
	defer serialFile.Close()

	cmd := exec.Command(plan.Args[0], plan.Args[1:]...)
	cmd.Dir = workspace.Root
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return SmokeResult{}, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		span.Fail("qemu stderr pipe failed")
		return SmokeResult{}, err
	}
	span.Set(2, "starting qemu")
	if err := cmd.Start(); err != nil {
		span.Fail("qemu start failed")
		return SmokeResult{}, err
	}

	matched := make(chan struct{})
	var matchedOnce sync.Once
	var writeMu sync.Mutex
	scan := func(reader io.Reader) {
		scanner := bufio.NewScanner(reader)
		scanner.Buffer(make([]byte, 64*1024), 1024*1024)
		for scanner.Scan() {
			line := scanner.Text()
			writeMu.Lock()
			_, _ = serialFile.WriteString(line + "\n")
			writeMu.Unlock()
			if strings.Contains(line, opts.Marker) {
				matchedOnce.Do(func() { close(matched) })
			}
		}
	}
	var scanners sync.WaitGroup
	scanners.Add(2)
	go func() {
		defer scanners.Done()
		scan(stdout)
	}()
	go func() {
		defer scanners.Done()
		scan(stderr)
	}()

	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	timer := time.NewTimer(opts.Timeout)
	defer timer.Stop()

	result := SmokeResult{
		Command: plan.Args,
		Serial:  serialPath,
		Log:     plan.LogPath,
		Marker:  opts.Marker,
		Timeout: opts.Timeout,
	}
	span.Set(3, "waiting for boot marker")
	select {
	case <-matched:
		result.Matched = true
		span.Set(4, "boot marker matched")
		if cmd.Process != nil {
			_ = cmd.Process.Signal(os.Interrupt)
		}
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			_ = cmd.Process.Kill()
			<-done
		}
		scanners.Wait()
		span.Done("qemu smoke passed")
		return result, nil
	case err := <-done:
		scanners.Wait()
		span.Fail("qemu exited")
		if err != nil {
			return result, fmt.Errorf("qemu exited before smoke marker: %w", err)
		}
		return result, fmt.Errorf("qemu exited before smoke marker")
	case <-timer.C:
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
		}
		<-done
		scanners.Wait()
		span.Fail("qemu smoke timed out")
		return result, fmt.Errorf("smoke marker not reached within %s", opts.Timeout)
	}
}

func TTYTest(workspace *config.Workspace, opts TTYTestOptions) (TTYTestResult, error) {
	span := progress.Use(opts.Progress).Start("qemu tty test", 6)
	defer span.Close()
	if opts.Timeout <= 0 {
		opts.Timeout = 60 * time.Second
	}
	if opts.BootMarker == "" {
		opts.BootMarker = "[seed2_root] manifest scheduler done"
	}
	if len(opts.Send) == 0 && opts.Python == "" {
		opts.Send = []string{"/bin/fastfetch"}
	}
	if len(opts.Expect) == 0 && opts.Python == "" {
		opts.Expect = []string{"PachaOS"}
	}

	span.Set(1, "building qemu command")
	plan, err := commandArgs(workspace, Options{
		Display:     "none",
		Console:     "pty",
		NewTerminal: true,
		NoKVM:       opts.NoKVM,
		Fast:        true,
		ExtraArgs:   opts.ExtraArgs,
	})
	if err != nil {
		span.Fail("qemu command failed")
		return TTYTestResult{}, err
	}

	artifacts := workspace.Path(workspace.Artifacts)
	serialPath := filepath.Join(artifacts, "serial-tty-test.log")
	consolePath := filepath.Join(artifacts, "console-tty-test.log")
	pythonLogPath := filepath.Join(artifacts, "qemu-tty-python.log")
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		span.Fail("artifact directory failed")
		return TTYTestResult{}, err
	}
	serialFile, err := os.Create(serialPath)
	if err != nil {
		span.Fail("serial log create failed")
		return TTYTestResult{}, err
	}
	defer serialFile.Close()
	consoleFile, err := os.Create(consolePath)
	if err != nil {
		span.Fail("console log create failed")
		return TTYTestResult{}, err
	}
	defer consoleFile.Close()

	cmd := exec.Command(plan.Args[0], plan.Args[1:]...)
	cmd.Dir = workspace.Root
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return TTYTestResult{}, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		span.Fail("qemu stderr pipe failed")
		return TTYTestResult{}, err
	}
	span.Set(2, "starting qemu")
	if err := cmd.Start(); err != nil {
		span.Fail("qemu start failed")
		return TTYTestResult{}, err
	}

	result := TTYTestResult{
		Command:       plan.Args,
		ConsoleSocket: plan.ConsoleSocket,
		Serial:        serialPath,
		Console:       consolePath,
		Log:           plan.LogPath,
		PythonLog:     pythonLogPath,
		BootMarker:    opts.BootMarker,
		Timeout:       opts.Timeout,
		Expected:      append([]string(nil), opts.Expect...),
		Python:        opts.Python,
	}

	booted := make(chan struct{})
	var bootedOnce sync.Once
	var writeMu sync.Mutex
	scanSerial := func(reader io.Reader) {
		scanner := bufio.NewScanner(reader)
		scanner.Buffer(make([]byte, 64*1024), 1024*1024)
		for scanner.Scan() {
			line := scanner.Text()
			writeMu.Lock()
			_, _ = serialFile.WriteString(line + "\n")
			writeMu.Unlock()
			if strings.Contains(line, opts.BootMarker) {
				bootedOnce.Do(func() { close(booted) })
			}
		}
	}
	var scanners sync.WaitGroup
	scanners.Add(2)
	go func() {
		defer scanners.Done()
		scanSerial(stdout)
	}()
	go func() {
		defer scanners.Done()
		scanSerial(stderr)
	}()

	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer os.Remove(plan.ConsoleSocket)

	span.Set(3, "waiting for virtio-console socket")
	if exited, err := waitForSocketOrExit(plan.ConsoleSocket, done, 5*time.Second); err != nil {
		if !exited {
			terminateQEMU(cmd, done)
		}
		scanners.Wait()
		span.Fail("virtio-console socket failed")
		return result, err
	} else if exited {
		scanners.Wait()
		span.Fail("qemu exited")
		return result, fmt.Errorf("qemu exited before virtio-console socket was ready")
	}

	span.Set(4, "waiting for boot marker")
	timer := time.NewTimer(opts.Timeout)
	defer timer.Stop()
	select {
	case <-booted:
	case err := <-done:
		scanners.Wait()
		span.Fail("qemu exited")
		if err != nil {
			return result, fmt.Errorf("qemu exited before boot marker: %w", err)
		}
		return result, fmt.Errorf("qemu exited before boot marker")
	case <-timer.C:
		terminateQEMU(cmd, done)
		scanners.Wait()
		span.Fail("boot marker timeout")
		return result, fmt.Errorf("boot marker not reached within %s", opts.Timeout)
	}

	var testErr error
	if opts.Python != "" {
		span.Set(5, "running python tty test")
		testErr = runPythonTTYTest(workspace, plan.ConsoleSocket, result, opts)
	} else {
		span.Set(5, "sending tty input")
		result.Sent, result.Matched, testErr = runSendExpectTTY(plan.ConsoleSocket, opts.Send, opts.Expect, opts.Timeout, consoleFile)
	}

	span.Set(6, "stopping qemu")
	terminateQEMU(cmd, done)
	scanners.Wait()
	if testErr != nil {
		span.Fail("qemu tty test failed")
		return result, testErr
	}
	span.Done("qemu tty test passed")
	return result, nil
}

func runSendExpectTTY(socketPath string, sends []string, expects []string, timeout time.Duration, consoleFile *os.File) (int, []string, error) {
	conn, err := net.DialTimeout("unix", socketPath, 5*time.Second)
	if err != nil {
		return 0, nil, err
	}
	defer conn.Close()

	var output strings.Builder
	var outputMu sync.Mutex
	readDone := make(chan error, 1)
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := conn.Read(buf)
			if n > 0 {
				chunk := buf[:n]
				_, _ = consoleFile.Write(chunk)
				outputMu.Lock()
				_, _ = output.Write(chunk)
				outputMu.Unlock()
			}
			if err != nil {
				if err == io.EOF || strings.Contains(err.Error(), "use of closed network connection") {
					readDone <- nil
				} else {
					readDone <- err
				}
				return
			}
		}
	}()

	sent := 0
	for _, value := range sends {
		if value == "" {
			continue
		}
		if !strings.HasSuffix(value, "\n") {
			value += "\n"
		}
		_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
		if _, err := io.WriteString(conn, value); err != nil {
			_ = conn.Close()
			<-readDone
			return sent, nil, err
		}
		sent++
	}

	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	ticker := time.NewTicker(25 * time.Millisecond)
	defer ticker.Stop()
	matched := make([]string, 0, len(expects))
	seen := make(map[string]bool, len(expects))
	for {
		outputMu.Lock()
		text := output.String()
		outputMu.Unlock()
		for _, expect := range expects {
			if expect == "" || seen[expect] {
				continue
			}
			if strings.Contains(text, expect) {
				seen[expect] = true
				matched = append(matched, expect)
			}
		}
		if len(matched) == len(expects) {
			_ = conn.Close()
			<-readDone
			return sent, matched, nil
		}
		select {
		case err := <-readDone:
			outputMu.Lock()
			text = output.String()
			outputMu.Unlock()
			for _, expect := range expects {
				if expect == "" || seen[expect] {
					continue
				}
				if strings.Contains(text, expect) {
					seen[expect] = true
					matched = append(matched, expect)
				}
			}
			if len(matched) == len(expects) {
				return sent, matched, nil
			}
			if err != nil {
				return sent, matched, err
			}
			return sent, matched, fmt.Errorf("virtio-console closed before expected output")
		case <-ticker.C:
		case <-deadline.C:
			_ = conn.Close()
			<-readDone
			return sent, matched, fmt.Errorf("expected console output not found within %s: %s", timeout, strings.Join(missingExpectations(expects, seen), ", "))
		}
	}
}

func runPythonTTYTest(workspace *config.Workspace, socketPath string, result TTYTestResult, opts TTYTestOptions) error {
	python, err := exec.LookPath("python3")
	if err != nil {
		return err
	}
	cmd := exec.Command(python, opts.Python)
	cmd.Dir = workspace.Root
	cmd.Env = append(os.Environ(),
		"PACGO_QEMU_CONSOLE="+socketPath,
		"PACGO_QEMU_SERIAL_LOG="+result.Serial,
		"PACGO_QEMU_CONSOLE_LOG="+result.Console,
		"PACGO_QEMU_LOG="+result.Log,
		"PACGO_QEMU_BOOT_MARKER="+opts.BootMarker,
		"PACGO_QEMU_TIMEOUT_SECONDS="+fmt.Sprintf("%.3f", opts.Timeout.Seconds()),
	)
	output, err := cmd.CombinedOutput()
	if writeErr := os.WriteFile(result.PythonLog, output, 0o644); writeErr != nil && err == nil {
		err = writeErr
	}
	if err != nil {
		return fmt.Errorf("python qemu test failed: %w", err)
	}
	return nil
}

func missingExpectations(expects []string, seen map[string]bool) []string {
	missing := make([]string, 0)
	for _, expect := range expects {
		if expect != "" && !seen[expect] {
			missing = append(missing, expect)
		}
	}
	return missing
}

func terminateQEMU(cmd *exec.Cmd, done <-chan error) {
	if cmd.Process == nil {
		return
	}
	_ = cmd.Process.Signal(os.Interrupt)
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		_ = cmd.Process.Kill()
		<-done
	}
}

func commandArgs(workspace *config.Workspace, opts Options) (commandPlan, error) {
	qemuPath := firstNonEmpty(os.Getenv("CAPOS_QEMU"), "qemu-system-x86_64")
	if opts.LimineImage == "" {
		opts.LimineImage = workspace.Path(workspace.Artifacts, "limine-boot.img")
	}
	switch firstNonEmpty(opts.Firmware, "bios") {
	case "bios":
		return limineBiosCommandArgs(workspace, qemuPath, opts)
	case "uefi":
		return limineUefiCommandArgs(workspace, qemuPath, opts)
	default:
		return commandPlan{}, fmt.Errorf("invalid firmware %q; expected bios or uefi", opts.Firmware)
	}
}

func limineImagePath(workspace *config.Workspace, image string) (string, error) {
	if image == "" {
		image = workspace.Path(workspace.Artifacts, "limine-boot.img")
	}
	if !filepath.IsAbs(image) {
		image = workspace.Path(image)
	}
	if _, err := os.Stat(image); err != nil {
		return "", err
	}
	return image, nil
}

func qemuDiskPathAndFormat(workspace *config.Workspace, opts Options) (string, string, error) {
	diskPath := workspace.Path(workspace.Disk.Image)
	if opts.DiskImage != "" {
		if filepath.IsAbs(opts.DiskImage) {
			diskPath = opts.DiskImage
		} else {
			diskPath = workspace.Path(opts.DiskImage)
		}
	}
	if _, err := os.Stat(diskPath); err != nil {
		return "", "", err
	}
	diskFormat := opts.DiskFormat
	if diskFormat == "" {
		diskFormat = "raw"
	}
	return diskPath, diskFormat, nil
}

func limineBiosCommandArgs(workspace *config.Workspace, qemuPath string, opts Options) (commandPlan, error) {
	imagePath := opts.LimineImage
	imagePath, err := limineImagePath(workspace, imagePath)
	if err != nil {
		return commandPlan{}, err
	}
	diskPath, diskFormat, err := qemuDiskPathAndFormat(workspace, opts)
	if err != nil {
		return commandPlan{}, err
	}
	if opts.Memory == "" {
		opts.Memory = "2G"
	}
	if opts.Display == "" {
		opts.Display = "none"
	}
	artifacts := workspace.Path(workspace.Artifacts)
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		return commandPlan{}, err
	}
	logPath := filepath.Join(artifacts, "qemu-limine.log")
	hostTimeLogPath := filepath.Join(artifacts, "qemu-limine-host-time.log")
	_ = os.WriteFile(logPath, nil, 0o644)
	args := []string{
		qemuPath,
		"-machine", "q35",
		"-m", opts.Memory,
		"-smp", "4",
		"-monitor", "none",
	}
	args = append(args,
		"-drive", "file="+imagePath+",format=raw,if=ide",
		"-drive", "if=none,file="+diskPath+",format="+diskFormat+",id=rootdisk",
		"-device", "nvme,drive=rootdisk,serial=capos-root",
		"-boot", "order=c",
		"-no-reboot",
	)
	if opts.Display == "none" {
		args = append(args, "-nographic")
	} else {
		args = append(args, "-display", opts.Display, "-serial", "stdio")
	}
	if !opts.NoKVM {
		args = append(args, "-enable-kvm")
	}
	args = appendNetworkArgs(args, opts.NoNet)
	args = append(args, opts.ExtraArgs...)
	return commandPlan{
		Args:        args,
		LogPath:     logPath,
		HostTimeLog: hostTimeLogPath,
	}, nil
}

func limineUefiCommandArgs(workspace *config.Workspace, qemuPath string, opts Options) (commandPlan, error) {
	imagePath, err := limineImagePath(workspace, opts.LimineImage)
	if err != nil {
		return commandPlan{}, err
	}
	diskPath, diskFormat, err := qemuDiskPathAndFormat(workspace, opts)
	if err != nil {
		return commandPlan{}, err
	}
	codePath := firstExisting(os.Getenv("CAPOS_OVMF_CODE"), "/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd")
	varsTemplate := firstExisting(os.Getenv("CAPOS_OVMF_VARS_TEMPLATE"), "/usr/share/OVMF/OVMF_VARS_4M.fd", "/usr/share/OVMF/OVMF_VARS.fd")
	if codePath == "" {
		return commandPlan{}, fmt.Errorf("missing OVMF code firmware for Limine UEFI boot; set CAPOS_OVMF_CODE")
	}
	if varsTemplate == "" {
		return commandPlan{}, fmt.Errorf("missing OVMF vars template for Limine UEFI boot; set CAPOS_OVMF_VARS_TEMPLATE")
	}
	if opts.Memory == "" {
		opts.Memory = "2G"
	}
	if opts.Display == "" {
		opts.Display = "none"
	}
	artifacts := workspace.Path(workspace.Artifacts)
	if err := os.MkdirAll(artifacts, 0o755); err != nil {
		return commandPlan{}, err
	}
	logPath := filepath.Join(artifacts, "qemu-limine-uefi.log")
	hostTimeLogPath := filepath.Join(artifacts, "qemu-limine-uefi-host-time.log")
	varsPath := filepath.Join(artifacts, "OVMF_LIMINE_VARS.fd")
	if err := copyFile(varsTemplate, varsPath); err != nil {
		return commandPlan{}, err
	}
	_ = os.WriteFile(logPath, nil, 0o644)
	args := []string{
		qemuPath,
		"-machine", "q35",
		"-m", opts.Memory,
		"-smp", "4",
		"-monitor", "none",
		"-drive", "if=pflash,format=raw,readonly=on,file=" + codePath,
		"-drive", "if=pflash,format=raw,file=" + varsPath,
		"-drive", "if=none,file=" + imagePath + ",format=raw,id=limineboot",
		"-device", "virtio-blk-pci,drive=limineboot,bootindex=1",
		"-drive", "if=none,file=" + diskPath + ",format=" + diskFormat + ",id=rootdisk",
		"-device", "nvme,drive=rootdisk,serial=capos-root,bootindex=2",
		"-boot", "order=c",
		"-no-reboot",
	}
	if opts.Display == "none" {
		args = append(args, "-nographic")
	} else {
		args = append(args, "-display", opts.Display, "-serial", "stdio")
	}
	if !opts.NoKVM {
		args = append(args, "-enable-kvm")
	}
	args = appendNetworkArgs(args, opts.NoNet)
	args = append(args, opts.ExtraArgs...)
	return commandPlan{
		Args:        args,
		LogPath:     logPath,
		HostTimeLog: hostTimeLogPath,
	}, nil
}

func appendNetworkArgs(args []string, noNet bool) []string {
	if noNet {
		return append(args, "-net", "none")
	}
	return append(args,
		"-net", "none",
		"-netdev", "user,id=net0",
		"-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56,disable-legacy=on",
	)
}

func validateKVMAvailable() error {
	info, err := os.Stat("/dev/kvm")
	if err != nil {
		return fmt.Errorf("KVM requested but /dev/kvm is unavailable: %w%s", err, kvmDiagnostics())
	}
	if info.Mode()&os.ModeCharDevice == 0 {
		return fmt.Errorf("KVM requested but /dev/kvm is not a character device%s", kvmDiagnostics())
	}
	file, err := os.OpenFile("/dev/kvm", os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("KVM requested but /dev/kvm cannot be opened: %w%s", err, kvmDiagnostics())
	}
	_ = file.Close()
	return nil
}

func kvmDiagnostics() string {
	var detail strings.Builder
	if data, err := os.ReadFile("/sys/devices/virtual/misc/kvm/dev"); err == nil {
		detail.WriteString("; kernel registered kvm misc device ")
		detail.WriteString(strings.TrimSpace(string(data)))
	}
	if line := devMountInfo(); line != "" {
		detail.WriteString("; /dev mount: ")
		detail.WriteString(line)
	}
	detail.WriteString("; if this is a Codex sandbox with KVM hidden, run './pacgo runner serve' in a normal WSL terminal and submit fixed KVM tasks with './pacgo runner run smoke --timeout 60s --marker \"[seed0root] ready\"'")
	return detail.String()
}

func devMountInfo() string {
	data, err := os.ReadFile("/proc/self/mountinfo")
	if err != nil {
		return ""
	}
	match := ""
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 5 && fields[4] == "/dev" {
			match = summarizeMountInfo(fields)
		}
	}
	return match
}

func summarizeMountInfo(fields []string) string {
	separator := -1
	for i, field := range fields {
		if field == "-" {
			separator = i
			break
		}
	}
	if separator < 0 || separator+3 >= len(fields) {
		return strings.Join(fields, " ")
	}
	return fmt.Sprintf("%s %s opts=%s", fields[separator+1], fields[separator+2], fields[5])
}

func copyFile(source string, dest string) error {
	data, err := os.ReadFile(source)
	if err != nil {
		return err
	}
	return os.WriteFile(dest, data, 0o644)
}

func firstExisting(paths ...string) string {
	for _, path := range paths {
		if path == "" {
			continue
		}
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}
	return ""
}

func consoleSocketPath(workspace *config.Workspace) (string, error) {
	base := os.Getenv("XDG_RUNTIME_DIR")
	if base == "" {
		base = os.TempDir()
	}
	sum := sha256.Sum256([]byte(workspace.Root))
	name := fmt.Sprintf("%s-%d-%d-virtio-console.sock", hex.EncodeToString(sum[:6]), os.Getpid(), time.Now().UnixNano())
	dir := filepath.Join(base, fmt.Sprintf("pacgo-%d", os.Getuid()))
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", err
	}
	return filepath.Join(dir, name), nil
}

func waitForSocketOrExit(socketPath string, done <-chan error, timeout time.Duration) (bool, error) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-done:
			if err != nil {
				return true, err
			}
			return true, nil
		case <-ticker.C:
			info, err := os.Stat(socketPath)
			if err == nil && info.Mode()&os.ModeSocket != 0 {
				return false, nil
			}
		case <-timer.C:
			return false, fmt.Errorf("virtio-console socket did not appear: %s", socketPath)
		}
	}
}

func consoleTerminalCommand(workspace *config.Workspace, socketPath string) ([]string, error) {
	if socketPath == "" {
		return nil, fmt.Errorf("--new-terminal requires virtio-console")
	}
	script := consoleTerminalScript(workspace, socketPath)
	candidates := [][]string{
		{"x-terminal-emulator", "-e", "bash", "-lc", script},
		{"gnome-terminal", "--", "bash", "-lc", script},
		{"konsole", "-e", "bash", "-lc", script},
		{"xfce4-terminal", "--command", "bash -lc " + shellQuote(script)},
		{"kitty", "bash", "-lc", script},
		{"alacritty", "-e", "bash", "-lc", script},
		{"wezterm", "start", "--cwd", workspace.Root, "bash", "-lc", script},
	}
	for _, candidate := range candidates {
		if path, err := exec.LookPath(candidate[0]); err == nil {
			candidate[0] = path
			return candidate, nil
		}
	}
	return nil, fmt.Errorf("no terminal emulator found; install x-terminal-emulator, gnome-terminal, konsole, xfce4-terminal, kitty, alacritty, or wezterm")
}

func consoleTerminalPreview(workspace *config.Workspace, socketPath string) []string {
	return []string{"<terminal>", "bash", "-lc", consoleTerminalScript(workspace, socketPath)}
}

func consoleTerminalScript(workspace *config.Workspace, socketPath string) string {
	return strings.Join([]string{
		"cd " + shellQuote(workspace.Root),
		"sock=" + shellQuote(socketPath),
		"if ! command -v socat >/dev/null 2>&1; then echo 'missing socat'; exec bash; fi",
		"echo 'CapabilityOS virtio-console'",
		"echo 'waiting for '\"$sock\"",
		"while [ ! -S \"$sock\" ]; do sleep 0.05; done",
		"echo 'connected'",
		"socat -,raw,echo=0 UNIX-CONNECT:\"$sock\"",
		"status=$?",
		"echo",
		"echo virtio-console exited: $status",
		"exec bash",
	}, "; ")
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if value != "" {
			return value
		}
	}
	return ""
}

func shellQuote(value string) string {
	return "'" + strings.ReplaceAll(value, "'", "'\\''") + "'"
}
