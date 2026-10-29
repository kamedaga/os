package buildsys

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"capabilityos/pack/internal/config"
)

type recipeEnvironment struct {
	workspace *config.Workspace
	hasher    *Hasher
	mu        sync.Mutex
	nix       nixEnvironmentDelta
	nixErr    error
	loaded    bool
}

type nixEnvironmentCache struct {
	Key        string            `json:"key"`
	Variables  map[string]string `json:"variables"`
	PathPrefix []string          `json:"pathPrefix"`
}

type nixEnvironmentDelta struct {
	Variables  map[string]string
	PathPrefix []string
}

var captureNixEnvironment = captureNixDevShellEnvironment

func captureNixDevShellEnvironment(root string) (nixEnvironmentDelta, error) {
	parent := sanitizedNixParentEnvironment()
	envPath, err := lookPathInEnvironment("env", parent)
	if err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("find env for nix environment capture: %w", err)
	}
	outside := exec.Command(envPath, "-0")
	outside.Dir = root
	outside.Env = environmentList(parent)
	outside.Stdin = nil
	outsideData, err := outside.Output()
	if err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("capture sanitized parent environment: %w", err)
	}

	nixPath, err := exec.LookPath("nix")
	if err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("find nix: %w", err)
	}
	temp, err := os.CreateTemp("", "pacgo-nix-environment-*")
	if err != nil {
		return nixEnvironmentDelta{}, err
	}
	tempPath := temp.Name()
	if err := temp.Close(); err != nil {
		_ = os.Remove(tempPath)
		return nixEnvironmentDelta{}, err
	}
	defer os.Remove(tempPath)
	cmd := exec.Command(nixPath, "develop", root, "--command", "/bin/sh", "-c", `env -0 >"$1"`, "pacgo-env", tempPath)
	cmd.Dir = root
	cmd.Env = environmentList(parent)
	cmd.Stdin = nil
	cmd.Stdout = io.Discard
	var stderr strings.Builder
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("capture nix devShell environment: %w: %s", err, strings.TrimSpace(stderr.String()))
	}
	insideData, err := os.ReadFile(tempPath)
	if err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("read nix devShell environment: %w", err)
	}
	return nixEnvironmentDeltaFromSnapshots(outsideData, insideData)
}

func newRecipeEnvironment(workspace *config.Workspace, hasher *Hasher) *recipeEnvironment {
	return &recipeEnvironment{workspace: workspace, hasher: hasher}
}

// variables is the environment every recipe runs and resolves its tools in:
// the caller's environment overlaid with the flake devShell. It is the same
// whether pacgo was started inside `nix develop` or from the host shell (as
// for QEMU with the host GPU), so neither changes a recipe's toolchain.
func (e *recipeEnvironment) variables() (map[string]string, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if !e.loaded {
		e.nix, e.nixErr = loadNixEnvironment(e.workspace, e.hasher)
		e.loaded = true
	}
	if e.nixErr != nil {
		return nil, e.nixErr
	}
	return applyNixEnvironment(environmentMap(os.Environ()), e.nix), nil
}

func loadNixEnvironment(workspace *config.Workspace, hasher *Hasher) (nixEnvironmentDelta, error) {
	key, err := nixEnvironmentKey(workspace, hasher)
	if err != nil {
		return nixEnvironmentDelta{}, err
	}
	cachePath := workspace.Path(workspace.State, "nix-env.json")
	if data, readErr := os.ReadFile(cachePath); readErr == nil {
		var cache nixEnvironmentCache
		if json.Unmarshal(data, &cache) == nil && cache.Key == key && cache.Variables != nil {
			return nixEnvironmentDelta{Variables: cache.Variables, PathPrefix: cache.PathPrefix}, nil
		}
	}
	delta, err := captureNixEnvironment(workspace.Root)
	if err != nil {
		return nixEnvironmentDelta{}, err
	}
	if err := writeNixEnvironmentCache(cachePath, nixEnvironmentCache{
		Key: key, Variables: delta.Variables, PathPrefix: delta.PathPrefix,
	}); err != nil {
		return nixEnvironmentDelta{}, err
	}
	return delta, nil
}

func nixEnvironmentKey(workspace *config.Workspace, hasher *Hasher) (string, error) {
	paths := []string{workspace.Path("flake.nix"), workspace.Path("flake.lock")}
	nixDir := workspace.Path("nix")
	err := filepath.WalkDir(nixDir, func(path string, entry os.DirEntry, walkErr error) error {
		if errors.Is(walkErr, os.ErrNotExist) {
			return nil
		}
		if walkErr != nil {
			return walkErr
		}
		if entry.Type().IsRegular() {
			paths = append(paths, path)
		}
		return nil
	})
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	sort.Strings(paths)
	digest := sha256.New()
	_, _ = io.WriteString(digest, "pacgo-nix-environment-v2\x00")
	for _, value := range environmentList(sanitizedNixParentEnvironment()) {
		_, _ = io.WriteString(digest, value)
		_, _ = digest.Write([]byte{0})
	}
	for _, path := range paths {
		fileDigest, err := hasher.Hash(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return "", err
		}
		rel := workspace.Rel(path)
		_, _ = io.WriteString(digest, rel)
		_, _ = digest.Write([]byte{0})
		_, _ = digest.Write(fileDigest)
		_, _ = digest.Write([]byte{0})
	}
	return hex.EncodeToString(digest.Sum(nil)), nil
}

func sanitizedNixParentEnvironment() map[string]string {
	env := map[string]string{
		"PATH":   "/usr/bin:/bin",
		"LANG":   "C.UTF-8",
		"LC_ALL": "C.UTF-8",
	}
	for _, name := range []string{"HOME", "USER", "LOGNAME"} {
		if value, ok := os.LookupEnv(name); ok {
			env[name] = value
		}
	}
	for _, name := range []string{
		"NIX_CONFIG", "NIX_CONF_DIR", "NIX_DAEMON_SOCKET_PATH", "NIX_DATA_DIR",
		"NIX_IGNORE_SYMLINK_STORE", "NIX_LOG_DIR", "NIX_PATH", "NIX_REMOTE",
		"NIX_SSL_CERT_FILE", "NIX_STATE_DIR", "NIX_STORE_DIR", "NIX_USER_CONF_FILES",
	} {
		if value, ok := os.LookupEnv(name); ok {
			env[name] = value
		}
	}
	return env
}

func nixEnvironmentDeltaFromSnapshots(outsideData, insideData []byte) (nixEnvironmentDelta, error) {
	outside, err := parseNixEnvironment(outsideData)
	if err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("parse sanitized parent environment: %w", err)
	}
	inside, err := parseNixEnvironment(insideData)
	if err != nil {
		return nixEnvironmentDelta{}, fmt.Errorf("parse nix devShell environment: %w", err)
	}
	prefix, err := nixPathPrefix(outside["PATH"], inside["PATH"])
	if err != nil {
		return nixEnvironmentDelta{}, err
	}
	excluded := map[string]bool{
		"PWD": true, "OLDPWD": true, "SHLVL": true, "_": true,
		"NIX_BUILD_TOP": true, "TEMP": true, "TEMPDIR": true, "TMP": true, "TMPDIR": true,
	}
	variables := make(map[string]string)
	for name, value := range inside {
		outsideValue, existed := outside[name]
		if name == "PATH" || excluded[name] || existed && outsideValue == value {
			continue
		}
		variables[name] = value
	}
	return nixEnvironmentDelta{Variables: variables, PathPrefix: prefix}, nil
}

func nixPathPrefix(outside, inside string) ([]string, error) {
	base := filepath.SplitList(outside)
	full := filepath.SplitList(inside)
	if len(full) < len(base) {
		return nil, fmt.Errorf("nix devShell PATH did not preserve the sanitized parent PATH")
	}
	offset := len(full) - len(base)
	for index := range base {
		if full[offset+index] != base[index] {
			return nil, fmt.Errorf("nix devShell PATH did not prepend to the sanitized parent PATH")
		}
	}
	return append([]string(nil), full[:offset]...), nil
}

func applyNixEnvironment(current map[string]string, delta nixEnvironmentDelta) map[string]string {
	env := cloneEnvironment(current)
	for name, value := range delta.Variables {
		env[name] = value
	}
	if len(delta.PathPrefix) != 0 {
		prefix := strings.Join(delta.PathPrefix, string(filepath.ListSeparator))
		if currentPath := current["PATH"]; currentPath != "" {
			env["PATH"] = prefix + string(filepath.ListSeparator) + currentPath
		} else {
			env["PATH"] = prefix
		}
	}
	return env
}

func parseNixEnvironment(data []byte) (map[string]string, error) {
	env := make(map[string]string)
	for _, entry := range strings.Split(string(data), "\x00") {
		if entry == "" {
			continue
		}
		name, value, found := strings.Cut(entry, "=")
		if !found || name == "" {
			return nil, fmt.Errorf("parse nix environment entry %q", entry)
		}
		env[name] = value
	}
	return env, nil
}

func writeNixEnvironmentCache(path string, cache nixEnvironmentCache) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(cache, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	tmp, err := os.CreateTemp(filepath.Dir(path), ".nix-env-*")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	ok := false
	defer func() {
		_ = tmp.Close()
		if !ok {
			_ = os.Remove(tmpPath)
		}
	}()
	if _, err := tmp.Write(data); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpPath, path); err != nil {
		return err
	}
	ok = true
	return nil
}

func environmentMap(values []string) map[string]string {
	env := make(map[string]string, len(values))
	for _, value := range values {
		name, contents, found := strings.Cut(value, "=")
		if found {
			env[name] = contents
		}
	}
	return env
}

func cloneEnvironment(source map[string]string) map[string]string {
	if source == nil {
		return nil
	}
	result := make(map[string]string, len(source))
	for key, value := range source {
		result[key] = value
	}
	return result
}

func environmentList(env map[string]string) []string {
	values := make([]string, 0, len(env))
	for name, value := range env {
		values = append(values, name+"="+value)
	}
	sort.Strings(values)
	return values
}

func lookPathInEnvironment(name string, env map[string]string) (string, error) {
	if strings.ContainsRune(name, filepath.Separator) {
		return name, nil
	}
	path := env["PATH"]
	for _, directory := range filepath.SplitList(path) {
		if directory == "" {
			directory = "."
		}
		candidate := filepath.Join(directory, name)
		info, err := os.Stat(candidate)
		if err == nil && !info.IsDir() && info.Mode()&0o111 != 0 {
			return candidate, nil
		}
	}
	return "", exec.ErrNotFound
}
