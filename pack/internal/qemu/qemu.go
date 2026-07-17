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
	Memory          string
	CPUs            int
	Display         string
	Console         string
	Firmware        string
	DiskImage       string
	DiskFormat      string
	NoKVM           bool
	NoNet           bool
	Fast            bool
	DryRun          bool
	InputProfile    string
	ExtraArgs       []string
	NewTerminal     bool
	Prepare         bool
	NoBuild         bool
	LimineImage     string
	QMP             string
	CaptureHeadless bool
	Progress        progress.Reporter
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
	QMPSocket     string
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
	Timeout          time.Duration
	NoKVM            bool
	CPUs             int
	Display          string
	InputProfile     string
	ExtraArgs        []string
	BootMarker       string
	Send             []string
	Expect           []string
	ScreendumpCheck  []string
	ScreendumpDevice string
	InputSendEvent   []string
	Python           string
	Progress         progress.Reporter
}

type TTYTestResult struct {
	Command       []string
	ConsoleSocket string
	Serial        string
	Console       string
	Log           string
	PythonLog     string
	HostTimeLog   string
	BootMarker    string
	Timeout       time.Duration
	Sent          int
	Expected      []string
	Matched       []string
	Python        string
	Screendumps   []string
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
		if plan.ConsoleSocket == "" {
			span.Fail("console terminal failed")
			return Result{}, fmt.Errorf("--new-terminal requires virtio-console")
		}
		if opts.DryRun {
			span.Set(2, "preparing console preview")
			consoleArgs, err = consoleTerminalPreview(workspace, plan.ConsoleSocket, plan.HostTimeLog)
			if err != nil {
				span.Fail("console terminal failed")
				return Result{}, err
			}
		} else {
			span.Set(2, "preparing console terminal")
			consoleArgs, err = consoleTerminalCommand(workspace, plan.ConsoleSocket, plan.HostTimeLog)
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
		if opts.NewTerminal {
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
		opts.Timeout = 30 * time.Second
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
		opts.Timeout = 30 * time.Second
	}
	if opts.BootMarker == "" {
		opts.BootMarker = "[termd] linux tty hvc open ready index=0 handle=2"
	}
	if len(opts.Send) == 0 && opts.Python == "" {
		opts.Send = []string{"hello from pacgo"}
	}
	if len(opts.Expect) == 0 && opts.Python == "" {
		opts.Expect = []string{"PachaOS hvc0 ready", "pacha-hvc received: hello from pacgo"}
	}

	span.Set(1, "building qemu command")
	artifacts := workspace.Path(workspace.Artifacts)
	checks := make([]screendumpCheck, 0, len(opts.ScreendumpCheck))
	for i, value := range opts.ScreendumpCheck {
		check, parseErr := parseScreendumpCheck(value, i, artifacts)
		if parseErr != nil {
			span.Fail("invalid screendump check")
			return TTYTestResult{}, parseErr
		}
		checks = append(checks, check)
	}
	inputChecks := make([]inputSendCheck, 0, len(opts.InputSendEvent))
	for _, value := range opts.InputSendEvent {
		check, parseErr := parseInputSendCheck(value)
		if parseErr != nil {
			span.Fail("invalid input send event")
			return TTYTestResult{}, parseErr
		}
		inputChecks = append(inputChecks, check)
	}
	qmpPath := ""
	if len(checks) != 0 || len(inputChecks) != 0 {
		qmpPath = filepath.Join(artifacts, "qemu-tty-test-qmp.sock")
		_ = os.Remove(qmpPath)
	}
	plan, err := commandArgs(workspace, Options{
		Display:         opts.Display,
		Console:         "pty",
		NewTerminal:     true,
		NoKVM:           opts.NoKVM,
		CPUs:            opts.CPUs,
		Fast:            true,
		InputProfile:    opts.InputProfile,
		ExtraArgs:       opts.ExtraArgs,
		QMP:             qmpPath,
		CaptureHeadless: len(checks) != 0,
	})
	if err != nil {
		span.Fail("qemu command failed")
		return TTYTestResult{}, err
	}

	serialPath := filepath.Join(artifacts, "serial-tty-test.log")
	consolePath := filepath.Join(artifacts, "console-tty-test.log")
	pythonLogPath := filepath.Join(artifacts, "qemu-tty-python.log")
	hostTimeLogPath := filepath.Join(artifacts, "qemu-tty-host-time.log")
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
	hostTimeLogFile, err := os.Create(hostTimeLogPath)
	if err != nil {
		span.Fail("host timestamp log create failed")
		return TTYTestResult{}, err
	}
	defer hostTimeLogFile.Close()
	hostLog := &hostTimeLog{file: hostTimeLogFile, started: time.Now()}
	_, _ = fmt.Fprintf(hostTimeLogFile, "%s +0s host | qemu start\n",
		hostLog.started.Format("2006-01-02T15:04:05.000000000-07:00"))
	stdoutHostLog := newHostTimeLineWriter(hostLog, "serial-stdout")
	stderrHostLog := newHostTimeLineWriter(hostLog, "serial-stderr")
	consoleHostLog := newHostTimeLineWriter(hostLog, "console")
	defer stdoutHostLog.Close()
	defer stderrHostLog.Close()
	defer consoleHostLog.Close()

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
		HostTimeLog:   hostTimeLogPath,
		BootMarker:    opts.BootMarker,
		Timeout:       opts.Timeout,
		Expected:      append([]string(nil), opts.Expect...),
		Python:        opts.Python,
	}
	for _, check := range checks {
		result.Screendumps = append(result.Screendumps, check.Path)
	}

	booted := make(chan struct{})
	var bootedOnce sync.Once
	var writeMu sync.Mutex
	scanSerial := func(reader io.Reader, hostWriter *hostTimeLineWriter) {
		scanner := bufio.NewScanner(reader)
		scanner.Buffer(make([]byte, 64*1024), 1024*1024)
		for scanner.Scan() {
			line := scanner.Text()
			hostWriter.writeLine([]byte(line))
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
		scanSerial(stdout, stdoutHostLog)
	}()
	go func() {
		defer scanners.Done()
		scanSerial(stderr, stderrHostLog)
	}()

	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer os.Remove(plan.ConsoleSocket)
	if plan.QMPSocket != "" {
		defer os.Remove(plan.QMPSocket)
	}

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

	var ttyClient *ttyConsoleClient
	var qmp *qmpClient
	if opts.Python == "" {
		ttyClient, err = startTTYConsoleClient(plan.ConsoleSocket, consoleFile, consoleHostLog)
		if err != nil {
			terminateQEMU(cmd, done)
			scanners.Wait()
			span.Fail("virtio-console connect failed")
			return result, err
		}
		defer ttyClient.Close()
		if len(checks) != 0 || len(inputChecks) != 0 {
			if exited, waitErr := waitForSocketOrExit(plan.QMPSocket, done, 5*time.Second); waitErr != nil || exited {
				terminateQEMU(cmd, done)
				scanners.Wait()
				span.Fail("QMP socket failed")
				if waitErr != nil {
					return result, waitErr
				}
				return result, fmt.Errorf("qemu exited before QMP socket was ready")
			}
			qmp, err = connectQMP(plan.QMPSocket, 5*time.Second)
			if err != nil {
				terminateQEMU(cmd, done)
				scanners.Wait()
				span.Fail("QMP connect failed")
				return result, err
			}
			defer qmp.Close()
			if cpuThreads, queryErr := qmp.queryCPUThreads(); queryErr == nil {
				var inventory strings.Builder
				for _, cpu := range cpuThreads {
					_, _ = fmt.Fprintf(&inventory, "%d\t%d\n", cpu.CPUIndex, cpu.ThreadID)
				}
				_ = os.WriteFile(
					workspace.Path(workspace.Artifacts, "qemu-tty-vcpus.tsv"),
					[]byte(inventory.String()),
					0o644,
				)
			}
		}
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
		var checkpointDone chan error
		if len(checks) != 0 {
			checkpointDone = make(chan error, 1)
			device := opts.ScreendumpDevice
			if device == "" {
				device = "pachagpu"
			}
			go func() { checkpointDone <- ttyClient.RunScreendumpChecks(qmp, device, checks, opts.Timeout) }()
		}
		var inputDone chan error
		if len(inputChecks) != 0 {
			inputDone = make(chan error, 1)
			go func() { inputDone <- ttyClient.RunInputSendChecks(qmp, inputChecks, opts.Timeout) }()
		}
		result.Sent, result.Matched, testErr = ttyClient.SendAndExpect(opts.Send, opts.Expect, opts.Timeout)
		if checkpointDone != nil {
			if checkpointErr := <-checkpointDone; testErr == nil {
				testErr = checkpointErr
			}
		}
		if inputDone != nil {
			if inputErr := <-inputDone; testErr == nil {
				testErr = inputErr
			}
		}
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

func (client *ttyConsoleClient) RunInputSendChecks(qmp *qmpClient, checks []inputSendCheck, timeout time.Duration) error {
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for _, check := range checks {
		for {
			client.outputMu.Lock()
			seen := strings.Contains(client.output.String(), check.Marker)
			client.outputMu.Unlock()
			if seen {
				for _, event := range check.Events {
					if err := qmp.inputSendEvent(event); err != nil {
						return fmt.Errorf("input-send-event at %s failed: %w", check.Marker, err)
					}
				}
				break
			}
			select {
			case <-ticker.C:
			case <-deadline.C:
				return fmt.Errorf("input-send-event marker %q not found within %s", check.Marker, timeout)
			}
		}
	}
	return nil
}

func (client *ttyConsoleClient) RunScreendumpChecks(qmp *qmpClient, device string, checks []screendumpCheck, timeout time.Duration) error {
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for _, check := range checks {
		for {
			client.outputMu.Lock()
			seen := strings.Contains(client.output.String(), check.Marker)
			client.outputMu.Unlock()
			if seen {
				if err := qmp.screendump(device, check); err != nil {
					return fmt.Errorf("screendump check %s failed: %w", check.Marker, err)
				}
				break
			}
			select {
			case <-ticker.C:
			case <-deadline.C:
				return fmt.Errorf("screendump marker %q not found within %s", check.Marker, timeout)
			}
		}
	}
	return nil
}

type ttyConsoleClient struct {
	conn        net.Conn
	consoleFile *os.File
	output      strings.Builder
	outputMu    sync.Mutex
	readDone    chan error
	hostWriter  io.Writer
	closeOnce   sync.Once
}

func startTTYConsoleClient(socketPath string, consoleFile *os.File, hostWriters ...io.Writer) (*ttyConsoleClient, error) {
	conn, err := net.DialTimeout("unix", socketPath, 5*time.Second)
	if err != nil {
		return nil, err
	}

	client := &ttyConsoleClient{
		conn:        conn,
		consoleFile: consoleFile,
		readDone:    make(chan error, 1),
	}
	if len(hostWriters) != 0 {
		client.hostWriter = hostWriters[0]
	}
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := conn.Read(buf)
			if n > 0 {
				chunk := buf[:n]
				_, _ = consoleFile.Write(chunk)
				if client.hostWriter != nil {
					_, _ = client.hostWriter.Write(chunk)
				}
				client.outputMu.Lock()
				_, _ = client.output.Write(chunk)
				client.outputMu.Unlock()
			}
			if err != nil {
				if err == io.EOF || strings.Contains(err.Error(), "use of closed network connection") {
					client.readDone <- nil
				} else {
					client.readDone <- err
				}
				return
			}
		}
	}()
	return client, nil
}

func (client *ttyConsoleClient) Close() {
	if client == nil {
		return
	}
	client.closeOnce.Do(func() {
		_ = client.conn.Close()
	})
}

func (client *ttyConsoleClient) SendAndExpect(sends []string, expects []string, timeout time.Duration) (int, []string, error) {
	sent := 0
	for _, value := range sends {
		if value == "" {
			continue
		}
		if !strings.HasSuffix(value, "\n") {
			value += "\n"
		}
		_ = client.conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
		if _, err := io.WriteString(client.conn, value); err != nil {
			client.Close()
			<-client.readDone
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
		client.outputMu.Lock()
		text := client.output.String()
		client.outputMu.Unlock()
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
			client.Close()
			<-client.readDone
			return sent, matched, nil
		}
		select {
		case err := <-client.readDone:
			client.outputMu.Lock()
			text = client.output.String()
			client.outputMu.Unlock()
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
			client.Close()
			<-client.readDone
			return sent, matched, fmt.Errorf("expected console output not found within %s: %s", timeout, strings.Join(missingExpectations(expects, seen), ", "))
		}
	}
}

func runSendExpectTTY(socketPath string, sends []string, expects []string, timeout time.Duration, consoleFile *os.File) (int, []string, error) {
	client, err := startTTYConsoleClient(socketPath, consoleFile)
	if err != nil {
		return 0, nil, err
	}
	defer client.Close()
	return client.SendAndExpect(sends, expects, timeout)
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
	cpus, err := normalizeCPUCount(opts.CPUs)
	if err != nil {
		return commandPlan{}, err
	}
	opts.CPUs = cpus
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

func normalizeCPUCount(cpus int) (int, error) {
	if cpus == 0 {
		return 4, nil
	}
	if cpus < 1 || cpus > 256 {
		return 0, fmt.Errorf("invalid CPU count %d; expected 1..256", cpus)
	}
	return cpus, nil
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

func appendConsoleArgs(workspace *config.Workspace, args []string, opts Options) ([]string, string, error) {
	console := opts.Console
	if console == "" && opts.NewTerminal {
		console = "pty"
	}
	switch console {
	case "", "none", "off":
		return args, "", nil
	case "pty":
		socketPath, err := consoleSocketPath(workspace)
		if err != nil {
			return nil, "", err
		}
		args = append(args,
			"-chardev", "socket,id=virtcon0,path="+socketPath+",server=on,wait=off",
			"-device", "virtio-serial-pci,disable-legacy=on,id=virtserial0",
			"-device", "virtconsole,chardev=virtcon0,name=org.pachaos.console.0",
		)
		return args, socketPath, nil
	default:
		return nil, "", fmt.Errorf("invalid console %q; expected off, none, or pty", opts.Console)
	}
}

func appendInputDeviceArgs(args []string, profile string) ([]string, error) {
	keyboard := []string{"-device", "virtio-keyboard-pci,disable-legacy=on,id=pachakbd"}
	mouse := []string{"-device", "virtio-mouse-pci,disable-legacy=on,id=pachamouse"}
	tablet := []string{"-device", "virtio-tablet-pci,disable-legacy=on,id=pachatablet"}
	switch profile {
	case "", "keyboard-mouse":
		args = append(args, keyboard...)
		args = append(args, mouse...)
	case "keyboard-tablet":
		args = append(args, keyboard...)
		args = append(args, tablet...)
	case "mouse-keyboard":
		args = append(args, mouse...)
		args = append(args, keyboard...)
	default:
		return nil, fmt.Errorf("invalid input profile %q; expected keyboard-mouse, keyboard-tablet, or mouse-keyboard", profile)
	}
	return args, nil
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
		"-smp", fmt.Sprint(opts.CPUs),
		"-monitor", "none",
	}
	args = append(args,
		"-drive", "file="+imagePath+",format=raw,if=ide",
		"-drive", "if=none,file="+diskPath+",format="+diskFormat+",id=rootdisk",
		"-device", "nvme,drive=rootdisk,serial=capos-root",
		"-device", "virtio-gpu-pci,disable-legacy=on,id=pachagpu",
	)
	args, err = appendInputDeviceArgs(args, opts.InputProfile)
	if err != nil {
		return commandPlan{}, err
	}
	args = append(args,
		"-boot", "order=c",
		"-no-reboot",
	)
	if opts.Display == "none" {
		if opts.CaptureHeadless {
			args = append(args, "-display", "none", "-serial", "stdio")
		} else {
			args = append(args, "-nographic")
		}
	} else {
		args = append(args, "-display", opts.Display, "-serial", "stdio")
	}
	if !opts.NoKVM {
		args = append(args, "-enable-kvm")
	}
	args = appendNetworkArgs(args, opts.NoNet)
	if opts.QMP != "" {
		args = append(args, "-qmp", "unix:"+opts.QMP+",server=on,wait=off")
	}
	var consoleSocket string
	args, consoleSocket, err = appendConsoleArgs(workspace, args, opts)
	if err != nil {
		return commandPlan{}, err
	}
	args = append(args, opts.ExtraArgs...)
	return commandPlan{
		Args:          args,
		LogPath:       logPath,
		HostTimeLog:   hostTimeLogPath,
		ConsoleSocket: consoleSocket,
		QMPSocket:     opts.QMP,
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
		"-smp", fmt.Sprint(opts.CPUs),
		"-monitor", "none",
		"-drive", "if=pflash,format=raw,readonly=on,file=" + codePath,
		"-drive", "if=pflash,format=raw,file=" + varsPath,
		"-drive", "if=none,file=" + imagePath + ",format=raw,id=limineboot",
		"-device", "virtio-blk-pci,drive=limineboot,bootindex=1",
		"-drive", "if=none,file=" + diskPath + ",format=" + diskFormat + ",id=rootdisk",
		"-device", "nvme,drive=rootdisk,serial=capos-root,bootindex=2",
		"-device", "virtio-gpu-pci,disable-legacy=on,id=pachagpu",
	}
	args, err = appendInputDeviceArgs(args, opts.InputProfile)
	if err != nil {
		return commandPlan{}, err
	}
	args = append(args,
		"-boot", "order=c",
		"-no-reboot",
	)
	if opts.Display == "none" {
		if opts.CaptureHeadless {
			args = append(args, "-display", "none", "-serial", "stdio")
		} else {
			args = append(args, "-nographic")
		}
	} else {
		args = append(args, "-display", opts.Display, "-serial", "stdio")
	}
	if !opts.NoKVM {
		args = append(args, "-enable-kvm")
	}
	args = appendNetworkArgs(args, opts.NoNet)
	if opts.QMP != "" {
		args = append(args, "-qmp", "unix:"+opts.QMP+",server=on,wait=off")
	}
	var consoleSocket string
	args, consoleSocket, err = appendConsoleArgs(workspace, args, opts)
	if err != nil {
		return commandPlan{}, err
	}
	args = append(args, opts.ExtraArgs...)
	return commandPlan{
		Args:          args,
		LogPath:       logPath,
		HostTimeLog:   hostTimeLogPath,
		ConsoleSocket: consoleSocket,
		QMPSocket:     opts.QMP,
	}, nil
}

func appendNetworkArgs(args []string, noNet bool) []string {
	if noNet {
		return append(args, "-net", "none")
	}
	return append(args,
		"-net", "none",
		"-netdev", "user,id=net0,hostfwd=udp:127.0.0.1:10015-10.0.2.15:7777,hostfwd=tcp:127.0.0.1:10016-10.0.2.15:7778",
		"-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56,disable-legacy=on,csum=off,gso=off,guest_csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,mrg_rxbuf=off",
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

func consoleTerminalCommand(workspace *config.Workspace, socketPath string, readyLogPath string) ([]string, error) {
	if socketPath == "" {
		return nil, fmt.Errorf("--new-terminal requires virtio-console")
	}
	scriptPath, err := consoleTerminalScriptPath(workspace, socketPath, readyLogPath)
	if err != nil {
		return nil, err
	}
	for _, candidate := range consoleTerminalCandidates(workspace, scriptPath) {
		if path, err := exec.LookPath(candidate[0]); err == nil {
			candidate[0] = path
			return candidate, nil
		}
	}
	return nil, fmt.Errorf("no terminal emulator found; install Windows Terminal, x-terminal-emulator, gnome-terminal, konsole, xfce4-terminal, kitty, alacritty, or wezterm")
}

func consoleTerminalPreview(workspace *config.Workspace, socketPath string, readyLogPath string) ([]string, error) {
	if socketPath == "" {
		return nil, fmt.Errorf("--new-terminal requires virtio-console")
	}
	scriptPath, err := consoleTerminalScriptPath(workspace, socketPath, readyLogPath)
	if err != nil {
		return nil, err
	}
	for _, candidate := range consoleTerminalCandidates(workspace, scriptPath) {
		if path, err := exec.LookPath(candidate[0]); err == nil {
			candidate[0] = path
			return candidate, nil
		}
	}
	return []string{"<terminal>", "bash", scriptPath}, nil
}

func consoleTerminalCandidates(workspace *config.Workspace, scriptPath string) [][]string {
	candidates := [][]string{}
	if distro := os.Getenv("WSL_DISTRO_NAME"); distro != "" {
		candidates = append(candidates, []string{
			"wt.exe",
			"-w", "-1",
			"new-tab",
			"--title", "PachaOS virtio-console",
			"wsl.exe",
			"-d", distro,
			"--cd", workspace.Root,
			"--",
			"bash", scriptPath,
		})
	}
	candidates = append(candidates,
		[]string{"x-terminal-emulator", "-e", "bash", scriptPath},
		[]string{"gnome-terminal", "--", "bash", scriptPath},
		[]string{"konsole", "-e", "bash", scriptPath},
		[]string{"xfce4-terminal", "--command", "bash " + shellQuote(scriptPath)},
		[]string{"kitty", "bash", scriptPath},
		[]string{"alacritty", "-e", "bash", scriptPath},
		[]string{"wezterm", "start", "--cwd", workspace.Root, "bash", scriptPath},
	)
	return candidates
}

func consoleTerminalScriptPath(workspace *config.Workspace, socketPath string, readyLogPath string) (string, error) {
	dir := workspace.Path(workspace.Artifacts, "qemu")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	name := strings.TrimSuffix(filepath.Base(socketPath), ".sock") + ".sh"
	path := filepath.Join(dir, name)
	content := consoleTerminalScriptContent(workspace, socketPath, readyLogPath)
	if err := os.WriteFile(path, []byte(content), 0o700); err != nil {
		return "", err
	}
	return path, nil
}

func consoleTerminalScriptContent(workspace *config.Workspace, socketPath string, readyLogPath string) string {
	return strings.Join([]string{
		"#!/usr/bin/env bash",
		"set +e",
		"cd " + shellQuote(workspace.Root),
		"sock=" + shellQuote(socketPath),
		"ready_log=" + shellQuote(readyLogPath),
		"ready_marker='[termd] linux tty hvc open ready index=0 handle=2'",
		"echo 'CapabilityOS virtio-console'",
		"echo 'waiting for '\"$sock\"",
		"while [ ! -S \"$sock\" ]; do",
		"  sleep 0.05",
		"done",
		"attempt=0",
		"while :; do",
		"attempt=$((attempt + 1))",
		"started=$(date +%s 2>/dev/null || echo 0)",
		"echo 'connected'",
		"if command -v socat >/dev/null 2>&1; then",
		"  socat -,raw,echo=0 UNIX-CONNECT:\"$sock\"",
		"  status=$?",
		"elif command -v python3 >/dev/null 2>&1; then",
		"  python3 - \"$sock\" <<'PY'",
		consoleTerminalPythonBridge(),
		"PY",
		"  status=$?",
		"else",
		"  echo 'missing socat or python3'",
		"  status=127",
		"fi",
		"ended=$(date +%s 2>/dev/null || echo 0)",
		"elapsed=$((ended - started))",
		"if [ \"$status\" -eq 0 ] && [ \"$elapsed\" -lt 2 ] && [ \"$attempt\" -lt 60 ]; then",
		"  echo",
		"  echo 'virtio-console disconnected before it was ready; reconnecting'",
		"  sleep 0.1",
		"  continue",
		"fi",
		"break",
		"done",
		"echo",
		"echo virtio-console exited: $status",
		"exec bash",
		"",
	}, "\n")
}

func consoleTerminalPythonBridge() string {
	return strings.Join([]string{
		"import os, select, socket, sys, termios, tty",
		"sock_path = sys.argv[1]",
		"sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)",
		"sock.connect(sock_path)",
		"tty_fd = os.open('/dev/tty', os.O_RDWR)",
		"old_termios = termios.tcgetattr(tty_fd)",
		"tty.setraw(tty_fd)",
		"try:",
		"    while True:",
		"        readable, _, _ = select.select([tty_fd, sock], [], [])",
		"        if tty_fd in readable:",
		"            data = os.read(tty_fd, 4096)",
		"            if not data:",
		"                break",
		"            sock.sendall(data)",
		"        if sock in readable:",
		"            data = sock.recv(4096)",
		"            if not data:",
		"                break",
		"            os.write(tty_fd, data)",
		"finally:",
		"    termios.tcsetattr(tty_fd, termios.TCSADRAIN, old_termios)",
		"    os.close(tty_fd)",
		"    sock.close()",
	}, "\n")
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
