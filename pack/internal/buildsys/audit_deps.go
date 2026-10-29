package buildsys

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"capabilityos/pack/internal/config"
)

type DependencyAudit struct {
	Recipe          string
	TraceLog        string
	Missing         []string
	ExcludedButRead []string
	// ExcludedStatted names excluded files whose metadata was inspected but
	// whose contents were never opened, such as CMake glob matching. Excluded
	// files are untracked by content, so this is informational.
	ExcludedStatted []string
	Overdeclared    []string
	UndeclaredTools []string
	FailedLookups   []string
}

func AuditRecipeDependencies(workspace *config.Workspace, recipeIDs []string) ([]DependencyAudit, error) {
	if len(recipeIDs) == 0 {
		return nil, errors.New("audit deps requires at least one recipe")
	}
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	environments := newRecipeEnvironment(workspace, hasher)
	env, err := environments.variables()
	if err != nil {
		return nil, fmt.Errorf("load nix environment for strace: %w", err)
	}
	strace, err := lookPathInEnvironment("strace", env)
	if err != nil {
		return nil, fmt.Errorf("find strace in nix environment: %w", err)
	}
	server, err := newJobserver(workspace)
	if err != nil {
		return nil, fmt.Errorf("create jobserver: %w", err)
	}
	defer server.Close()
	seen := make(map[string]bool)
	results := make([]DependencyAudit, 0, len(recipeIDs))
	for _, id := range recipeIDs {
		if seen[id] {
			return nil, fmt.Errorf("duplicate recipe: %s", id)
		}
		seen[id] = true
		recipe, ok := workspace.Recipe(id)
		if !ok {
			return nil, fmt.Errorf("recipe not found: %s", id)
		}
		_, pre, statusErr := recipeStatusInEnvironment(workspace, recipe, hasher, env)
		if statusErr != nil {
			return nil, statusErr
		}
		tracePath := workspace.Path(workspace.Artifacts, "pack", "logs", sanitizeFilename(id)+".strace.log")
		if err := os.MkdirAll(filepath.Dir(tracePath), 0o755); err != nil {
			return nil, err
		}
		prefix := []string{
			strace, "-f", "-qq", "-yy", "-s", "65535",
			"-e", "trace=openat,open,execve,stat,newfstatat,access,readlink,chdir,fchdir,clone,clone3,fork,vfork",
			"-o", tracePath,
		}
		if _, _, runErr := runRecipeWithPrefix(workspace, recipe, pre, hasher, env, server, prefix); runErr != nil {
			return nil, runErr
		}
		result, auditErr := analyzeDependencyTrace(workspace, recipe, tracePath)
		if auditErr != nil {
			return nil, auditErr
		}
		results = append(results, result)
	}
	return results, nil
}

func analyzeDependencyTrace(workspace *config.Workspace, recipe config.Recipe, tracePath string) (DependencyAudit, error) {
	file, err := os.Open(tracePath)
	if err != nil {
		return DependencyAudit{}, err
	}
	events, err := parseStrace(file, workspace.Root)
	closeErr := file.Close()
	if err != nil {
		return DependencyAudit{}, err
	}
	if closeErr != nil {
		return DependencyAudit{}, closeErr
	}
	state, exists, err := readRecipeState(workspace, recipe.ID)
	if err != nil || !exists {
		if err == nil {
			err = fmt.Errorf("recipe %s did not record state", recipe.ID)
		}
		return DependencyAudit{}, err
	}
	covered := make(map[string]bool)
	declared := make(map[string]string)
	for _, input := range state.DeclaredInputs {
		path := absoluteStatePath(workspace, input.Path)
		canonical := canonicalFilePath(path)
		covered[canonical] = true
		declared[canonical] = input.Path
	}
	for _, input := range state.DiscoveredInputs {
		covered[canonicalFilePath(absoluteStatePath(workspace, input.Path))] = true
	}
	read := make(map[string]bool)
	missing := make(map[string]bool)
	excludedButRead := make(map[string]bool)
	excludedStatted := make(map[string]bool)
	opened := make(map[string]bool)
	for _, event := range events {
		if event.Read && (event.Syscall == "open" || event.Syscall == "openat" || event.Exec) {
			opened[canonicalFilePath(filepath.Clean(event.Path))] = true
		}
	}
	executed := make(map[string]bool)
	failed := make(map[string]bool)
	for _, event := range events {
		path := filepath.Clean(event.Path)
		if event.Exec {
			executed[path] = true
		}
		if event.Missing && pathWithinAbs(path, workspace.Root) {
			failed[statePath(workspace, path)] = true
		}
		if !event.Read || !pathWithinAbs(path, workspace.Root) {
			continue
		}
		info, statErr := os.Stat(path)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		canonical := canonicalFilePath(path)
		read[canonical] = true
		if !covered[canonical] && auditInputPath(workspace, recipe, path) {
			if recipeExcludesPath(workspace, recipe, path) {
				if opened[canonical] {
					excludedButRead[statePath(workspace, path)] = true
				} else {
					excludedStatted[statePath(workspace, path)] = true
				}
			} else {
				missing[statePath(workspace, path)] = true
			}
		}
	}
	toolPaths := make(map[string]bool)
	for _, tool := range state.Tools {
		toolPaths[canonicalExecutable(tool.Path)] = true
	}
	undeclaredTools := make(map[string]bool)
	references := storeReferenceScanner{}
	for path := range executed {
		if toolPaths[canonicalExecutable(path)] {
			continue
		}
		if references.referencedByTool(state.Tools, path) {
			continue
		}
		undeclaredTools[path] = true
	}
	overdeclared := make(map[string]bool)
	for path, reported := range declared {
		if !read[path] {
			overdeclared[reported] = true
		}
	}
	return DependencyAudit{
		Recipe: recipe.ID, TraceLog: statePath(workspace, tracePath),
		Missing: mapKeys(missing), ExcludedButRead: mapKeys(excludedButRead), ExcludedStatted: mapKeys(excludedStatted), Overdeclared: mapKeys(overdeclared),
		UndeclaredTools: mapKeys(undeclaredTools), FailedLookups: mapKeys(failed),
	}, nil
}

func recipeExcludesPath(workspace *config.Workspace, recipe config.Recipe, path string) bool {
	for _, input := range recipe.Inputs {
		if len(input.Exclude) == 0 {
			continue
		}
		root := workspace.Path(input.Path)
		if !pathWithinAbs(path, root) {
			continue
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			continue
		}
		excluded, err := excludedInput(input, relative)
		if err == nil && excluded {
			return true
		}
	}
	return false
}

func auditInputPath(workspace *config.Workspace, recipe config.Recipe, path string) bool {
	for _, output := range recipe.Outputs {
		if pathWithinAbs(path, workspace.Path(output)) {
			return false
		}
	}
	if pathWithinAbs(path, workspace.Path(workspace.Artifacts)) {
		for _, owner := range workspace.Recipes() {
			if owner.ID == recipe.ID {
				continue
			}
			for _, output := range owner.Outputs {
				if pathWithinAbs(path, workspace.Path(output)) {
					// The recipe state records the outputs of every need, so
					// reading inside one is already tracked.
					return !containsString(recipe.Needs, owner.ID)
				}
			}
		}
		return false
	}
	return true
}

func absoluteStatePath(workspace *config.Workspace, path string) string {
	if filepath.IsAbs(path) {
		return filepath.Clean(path)
	}
	return filepath.Clean(workspace.Path(filepath.FromSlash(path)))
}

// storeReferenceScanner decides whether an executed Nix store binary is
// reached through a declared tool, such as the unwrapped clang behind
// clang-wrapper. A store path changes whenever its contents could, and the
// declared tool's tracked content names that path, so the tool's digest
// already covers it.
type storeReferenceScanner struct {
	contents map[string][]byte
}

func (s *storeReferenceScanner) referencedByTool(tools []recipeToolState, executed string) bool {
	item := nixStoreItem(canonicalExecutable(executed))
	if item == "" {
		return false
	}
	for _, tool := range tools {
		toolPath := canonicalExecutable(tool.Path)
		if s.contents == nil {
			s.contents = make(map[string][]byte)
		}
		data, ok := s.contents[toolPath]
		if !ok {
			data, _ = os.ReadFile(toolPath)
			s.contents[toolPath] = data
		}
		if strings.Contains(string(data), item) {
			return true
		}
	}
	return false
}

// nixStoreItem returns the /nix/store/<hash>-<name> prefix of path, or "".
func nixStoreItem(path string) string {
	const store = "/nix/store/"
	if !strings.HasPrefix(path, store) {
		return ""
	}
	name, _, _ := strings.Cut(strings.TrimPrefix(path, store), "/")
	if name == "" {
		return ""
	}
	return store + name
}

func canonicalExecutable(path string) string {
	return canonicalFilePath(path)
}

func canonicalFilePath(path string) string {
	path = filepath.Clean(path)
	if resolved, err := filepath.EvalSymlinks(path); err == nil {
		return filepath.Clean(resolved)
	}
	return path
}

func MissingDependenciesByDirectory(paths []string) [][2]string {
	grouped := make(map[string][]string)
	for _, path := range paths {
		directory := filepath.ToSlash(filepath.Dir(path))
		grouped[directory] = append(grouped[directory], filepath.Base(path))
	}
	directories := make([]string, 0, len(grouped))
	for directory := range grouped {
		directories = append(directories, directory)
	}
	sort.Strings(directories)
	groups := make([][2]string, 0, len(directories))
	for _, directory := range directories {
		sort.Strings(grouped[directory])
		groups = append(groups, [2]string{directory, strings.Join(grouped[directory], ", ")})
	}
	return groups
}
