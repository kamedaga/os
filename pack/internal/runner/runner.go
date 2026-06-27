package runner

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"capabilityos/pack/internal/config"
)

const (
	defaultDirName = "codex-runner"
	requestDir     = "requests"
	resultDir      = "results"
)

type Action string

const (
	ActionBuildKernel Action = "build-kernel"
	ActionQEMUDryRun  Action = "qemu-dry-run"
	ActionSmoke       Action = "smoke"
)

type Request struct {
	ID        string `json:"id"`
	Action    Action `json:"action"`
	TimeoutMS int64  `json:"timeout_ms,omitempty"`
	Marker    string `json:"marker,omitempty"`
	CreatedAt string `json:"created_at"`
}

type Result struct {
	ID         string `json:"id"`
	Action     Action `json:"action"`
	StartedAt  string `json:"started_at"`
	FinishedAt string `json:"finished_at"`
	ExitCode   int    `json:"exit_code"`
	OK         bool   `json:"ok"`
	Stdout     string `json:"stdout"`
	Stderr     string `json:"stderr"`
	Error      string `json:"error,omitempty"`
}

type ServeOptions struct {
	Dir          string
	PollInterval time.Duration
	Once         bool
	OnResult     func(Result)
}

type RunOptions struct {
	Dir     string
	Action  Action
	Timeout time.Duration
	Marker  string
	Wait    time.Duration
}

type Status struct {
	Dir               string
	Requests          int
	CompletedRequests int
	Results           int
}

type CleanupOptions struct {
	Dir       string
	OlderThan time.Duration
}

type CleanupResult struct {
	RequestsRemoved          int
	CompletedRequestsRemoved int
	ResultsRemoved           int
}

func DefaultDir(workspace *config.Workspace) string {
	return filepath.Join(workspace.Path(workspace.Artifacts), defaultDirName)
}

func Serve(workspace *config.Workspace, opts ServeOptions) error {
	base := firstNonEmpty(opts.Dir, DefaultDir(workspace))
	if opts.PollInterval <= 0 {
		opts.PollInterval = 250 * time.Millisecond
	}
	if err := ensureLayout(base); err != nil {
		return err
	}
	seen := make(map[string]bool)
	for {
		ran, result, err := servePendingOnce(workspace, base, seen)
		if err != nil {
			return err
		}
		if ran && opts.OnResult != nil {
			opts.OnResult(result)
		}
		if opts.Once {
			if ran {
				return nil
			}
			return errors.New("no pending runner requests")
		}
		time.Sleep(opts.PollInterval)
	}
}

func GetStatus(workspace *config.Workspace, dir string) (Status, error) {
	base := firstNonEmpty(dir, DefaultDir(workspace))
	if err := ensureLayout(base); err != nil {
		return Status{}, err
	}
	requests, completedRequests, err := countRequestFiles(base)
	if err != nil {
		return Status{}, err
	}
	results, err := countJSONFiles(filepath.Join(base, resultDir))
	if err != nil {
		return Status{}, err
	}
	return Status{Dir: base, Requests: requests, CompletedRequests: completedRequests, Results: results}, nil
}

func Cleanup(workspace *config.Workspace, opts CleanupOptions) (CleanupResult, error) {
	base := firstNonEmpty(opts.Dir, DefaultDir(workspace))
	if err := ensureLayout(base); err != nil {
		return CleanupResult{}, err
	}
	completedRequests, err := removeCompletedRequests(base)
	if err != nil {
		return CleanupResult{}, err
	}
	if opts.OlderThan <= 0 {
		opts.OlderThan = 24 * time.Hour
	}
	cutoff := time.Now().Add(-opts.OlderThan)
	requests, err := removeJSONOlderThan(filepath.Join(base, requestDir), cutoff)
	if err != nil {
		return CleanupResult{}, err
	}
	results, err := removeJSONOlderThan(filepath.Join(base, resultDir), cutoff)
	if err != nil {
		return CleanupResult{}, err
	}
	return CleanupResult{RequestsRemoved: requests, CompletedRequestsRemoved: completedRequests, ResultsRemoved: results}, nil
}

func SubmitAndWait(workspace *config.Workspace, opts RunOptions) (Result, error) {
	base := firstNonEmpty(opts.Dir, DefaultDir(workspace))
	if opts.Wait <= 0 {
		opts.Wait = 5 * time.Minute
	}
	if err := ensureLayout(base); err != nil {
		return Result{}, err
	}
	req := Request{
		ID:        requestID(),
		Action:    opts.Action,
		TimeoutMS: int64(opts.Timeout / time.Millisecond),
		Marker:    opts.Marker,
		CreatedAt: time.Now().UTC().Format(time.RFC3339Nano),
	}
	if err := validateRequest(req); err != nil {
		return Result{}, err
	}
	reqPath := filepath.Join(base, requestDir, req.ID+".json")
	if err := writeJSONAtomic(reqPath, req); err != nil {
		return Result{}, err
	}
	deadline := time.Now().Add(opts.Wait)
	resultPath := filepath.Join(base, resultDir, req.ID+".json")
	for {
		result, err := readResult(resultPath)
		if err == nil {
			return result, nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return Result{}, err
		}
		if time.Now().After(deadline) {
			return Result{}, fmt.Errorf("runner request timed out waiting for result: %s", req.ID)
		}
		time.Sleep(250 * time.Millisecond)
	}
}

func servePendingOnce(workspace *config.Workspace, base string, seen map[string]bool) (bool, Result, error) {
	entries, err := os.ReadDir(filepath.Join(base, requestDir))
	if err != nil {
		return false, Result{}, err
	}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		id := strings.TrimSuffix(entry.Name(), ".json")
		if seen[id] {
			continue
		}
		reqPath := filepath.Join(base, requestDir, entry.Name())
		resultPath := filepath.Join(base, resultDir, id+".json")
		if _, err := os.Stat(resultPath); err == nil {
			_ = os.Remove(reqPath)
			continue
		} else if !errors.Is(err, os.ErrNotExist) {
			return false, Result{}, err
		}
		req, err := readRequest(reqPath)
		if err != nil {
			return false, Result{}, err
		}
		if req.ID != id {
			return false, Result{}, fmt.Errorf("runner request id mismatch: path=%s body=%s", id, req.ID)
		}
		if err := validateRequest(req); err != nil {
			return false, Result{}, err
		}
		seen[id] = true
		result := execute(workspace, req)
		if err := writeJSONAtomic(resultPath, result); err != nil {
			return false, Result{}, err
		}
		_ = os.Remove(reqPath)
		return true, result, nil
	}
	return false, Result{}, nil
}

func execute(workspace *config.Workspace, req Request) Result {
	started := time.Now()
	args := commandFor(req)
	result := Result{
		ID:        req.ID,
		Action:    req.Action,
		StartedAt: started.UTC().Format(time.RFC3339Nano),
	}
	cmd := exec.Command(args[0], args[1:]...)
	cmd.Dir = workspace.Root
	cmd.Env = runnerEnv(workspace)
	stdout, stderr := &strings.Builder{}, &strings.Builder{}
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	err := cmd.Run()
	result.FinishedAt = time.Now().UTC().Format(time.RFC3339Nano)
	result.Stdout = stdout.String()
	result.Stderr = stderr.String()
	if err == nil {
		result.OK = true
		return result
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		result.ExitCode = exitErr.ExitCode()
	} else {
		result.ExitCode = -1
	}
	result.Error = err.Error()
	return result
}

func commandFor(req Request) []string {
	switch req.Action {
	case ActionBuildKernel:
		return []string{"./pacgo", "build", "kernel"}
	case ActionQEMUDryRun:
		return []string{"./pacgo", "qemu", "--prepare", "--dry-run"}
	case ActionSmoke:
		timeout := req.TimeoutMS
		if timeout <= 0 {
			timeout = int64((60 * time.Second) / time.Millisecond)
		}
		marker := firstNonEmpty(req.Marker, "[seed0root] ready")
		return []string{
			"./pacgo", "test", "smoke",
			"--timeout", fmt.Sprintf("%dms", timeout),
			"--marker", marker,
		}
	default:
		panic("validated runner action became invalid")
	}
}

func validateRequest(req Request) error {
	switch req.Action {
	case ActionBuildKernel, ActionQEMUDryRun:
		if req.Marker != "" || req.TimeoutMS != 0 {
			return fmt.Errorf("%s request does not accept marker or timeout", req.Action)
		}
	case ActionSmoke:
		if req.TimeoutMS < 0 || req.TimeoutMS > int64((10*time.Minute)/time.Millisecond) {
			return fmt.Errorf("smoke timeout out of range: %dms", req.TimeoutMS)
		}
		if strings.ContainsAny(req.Marker, "\x00\r\n") {
			return errors.New("smoke marker must be a single line")
		}
	default:
		return fmt.Errorf("unsupported runner action: %s", req.Action)
	}
	if req.ID == "" {
		return errors.New("runner request id is required")
	}
	if strings.Contains(req.ID, "/") || strings.Contains(req.ID, "\\") || strings.Contains(req.ID, "..") {
		return fmt.Errorf("invalid runner request id: %q", req.ID)
	}
	return nil
}

func runnerEnv(workspace *config.Workspace) []string {
	env := []string{
		"HOME=" + os.Getenv("HOME"),
		"PATH=" + os.Getenv("PATH"),
		"TERM=" + firstNonEmpty(os.Getenv("TERM"), "xterm-256color"),
		"PACGO_NIX_DEVELOP=1",
		"GOCACHE=" + filepath.Join(workspace.Path(workspace.Artifacts), "go-build"),
	}
	for _, name := range []string{"CAPOS_QEMU"} {
		if value := os.Getenv(name); value != "" {
			env = append(env, name+"="+value)
		}
	}
	return env
}

func ensureLayout(base string) error {
	if err := os.MkdirAll(filepath.Join(base, requestDir), 0o755); err != nil {
		return err
	}
	return os.MkdirAll(filepath.Join(base, resultDir), 0o755)
}

func countJSONFiles(dir string) (int, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0, err
	}
	count := 0
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".json") {
			count++
		}
	}
	return count, nil
}

func countRequestFiles(base string) (int, int, error) {
	entries, err := os.ReadDir(filepath.Join(base, requestDir))
	if err != nil {
		return 0, 0, err
	}
	pending := 0
	completed := 0
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		id := strings.TrimSuffix(entry.Name(), ".json")
		resultPath := filepath.Join(base, resultDir, id+".json")
		if _, err := os.Stat(resultPath); err == nil {
			completed++
			continue
		} else if !errors.Is(err, os.ErrNotExist) {
			return pending, completed, err
		}
		pending++
	}
	return pending, completed, nil
}

func removeCompletedRequests(base string) (int, error) {
	entries, err := os.ReadDir(filepath.Join(base, requestDir))
	if err != nil {
		return 0, err
	}
	removed := 0
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		id := strings.TrimSuffix(entry.Name(), ".json")
		reqPath := filepath.Join(base, requestDir, entry.Name())
		resultPath := filepath.Join(base, resultDir, id+".json")
		if _, err := os.Stat(resultPath); err == nil {
			if err := os.Remove(reqPath); err != nil && !errors.Is(err, os.ErrNotExist) {
				return removed, err
			}
			removed++
			continue
		} else if !errors.Is(err, os.ErrNotExist) {
			return removed, err
		}
	}
	return removed, nil
}

func removeJSONOlderThan(dir string, cutoff time.Time) (int, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0, err
	}
	removed := 0
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		path := filepath.Join(dir, entry.Name())
		info, err := entry.Info()
		if err != nil {
			return removed, err
		}
		if info.ModTime().After(cutoff) {
			continue
		}
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			return removed, err
		}
		removed++
	}
	return removed, nil
}

func readRequest(path string) (Request, error) {
	var req Request
	if err := readJSON(path, &req); err != nil {
		return Request{}, err
	}
	return req, nil
}

func readResult(path string) (Result, error) {
	var result Result
	if err := readJSON(path, &result); err != nil {
		return Result{}, err
	}
	return result, nil
}

func readJSON(path string, target any) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	decoder := json.NewDecoder(file)
	decoder.DisallowUnknownFields()
	return decoder.Decode(target)
}

func writeJSONAtomic(path string, value any) error {
	tmp := path + ".tmp"
	file, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(file)
	encoder.SetIndent("", "  ")
	writeErr := encoder.Encode(value)
	closeErr := file.Close()
	if writeErr != nil {
		_ = os.Remove(tmp)
		return writeErr
	}
	if closeErr != nil {
		_ = os.Remove(tmp)
		return closeErr
	}
	return os.Rename(tmp, path)
}

func requestID() string {
	return fmt.Sprintf("%d-%d", time.Now().UnixNano(), os.Getpid())
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if value != "" {
			return value
		}
	}
	return ""
}
