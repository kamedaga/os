package buildsys

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"

	"capabilityos/pack/internal/config"
)

type recipeInputState struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256,omitempty"`
}

type recipeToolState struct {
	Name   string `json:"name"`
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
}

type recipeOutputState struct {
	Path   string `json:"path"`
	Kind   string `json:"kind"`
	Digest string `json:"digest"`
}

// recipeNeedState is the output snapshot of a needed recipe as this recipe
// last saw it.
type recipeNeedState struct {
	ID      string              `json:"id"`
	Outputs []recipeOutputState `json:"outputs"`
}

type recipeState struct {
	Run              []string            `json:"run"`
	Discover         []config.Discovery  `json:"discover,omitempty"`
	DeclaredInputs   []recipeInputState  `json:"declaredInputs"`
	DiscoveredInputs []recipeInputState  `json:"discoveredInputs,omitempty"`
	NegativeDeps     []string            `json:"negativeDeps,omitempty"`
	Tools            []recipeToolState   `json:"tools"`
	Outputs          []recipeOutputState `json:"outputs"`
	Needs            []recipeNeedState   `json:"needs,omitempty"`
}

type RecipeStatus struct {
	ID       string
	UpToDate bool
	Reason   string
}

func recipeStatePath(workspace *config.Workspace, recipeID string) string {
	return filepath.Join(workspace.Path(workspace.State), "build", "recipes", sanitizeFilename(recipeID)+".json")
}

func recipeInputs(workspace *config.Workspace, recipe config.Recipe) []string {
	seen := make(map[string]bool)
	var dependencies []string
	add := func(path string) {
		if path == "" || strings.HasPrefix(path, "-") {
			return
		}
		if !filepath.IsAbs(path) {
			path = workspace.Path(path)
		}
		path = filepath.Clean(path)
		if !seen[path] {
			seen[path] = true
			dependencies = append(dependencies, path)
		}
	}
	for _, arg := range recipe.Run {
		argPath := arg
		if !filepath.IsAbs(argPath) {
			argPath = workspace.Path(argPath)
		}
		rel, err := filepath.Rel(workspace.Path(workspace.Artifacts), argPath)
		if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
			if _, err := os.Stat(argPath); err == nil {
				add(arg)
			}
		}
	}
	for _, input := range recipe.Inputs {
		add(input.Path)
	}
	return dependencies
}

func snapshotRecipeInputs(workspace *config.Workspace, recipe config.Recipe, hasher *Hasher) ([]recipeInputState, error) {
	seen := make(map[string]bool)
	var inputs []recipeInputState
	addFile := func(path string) error {
		abs, err := filepath.Abs(path)
		if err != nil {
			return err
		}
		abs = filepath.Clean(abs)
		if seen[abs] {
			return nil
		}
		seen[abs] = true
		digest, err := hasher.Hash(abs)
		if err != nil {
			return err
		}
		inputs = append(inputs, recipeInputState{Path: statePath(workspace, abs), SHA256: hex.EncodeToString(digest)})
		return nil
	}
	configured := make(map[string]config.RecipeInput, len(recipe.Inputs))
	for _, input := range recipe.Inputs {
		path := input.Path
		if !filepath.IsAbs(path) {
			path = workspace.Path(path)
		}
		configured[filepath.Clean(path)] = input
	}
	for _, dependency := range recipeInputs(workspace, recipe) {
		info, err := os.Stat(dependency)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, err
		}
		if !info.IsDir() {
			if !strings.HasSuffix(info.Name(), ".pyc") {
				if err := addFile(dependency); err != nil {
					return nil, err
				}
			}
			continue
		}
		input := configured[filepath.Clean(dependency)]
		err = filepath.WalkDir(dependency, func(path string, entry os.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			name := entry.Name()
			if entry.IsDir() {
				if name == ".git" || name == ".zig-cache" || name == "zig-out" || name == "CMakeFiles" || name == "__pycache__" {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(name, ".pyc") || entry.Type()&os.ModeType != 0 {
				return nil
			}
			relative, relErr := filepath.Rel(dependency, path)
			if relErr != nil {
				return relErr
			}
			excluded, excludeErr := excludedInput(input, relative)
			if excludeErr != nil {
				return fmt.Errorf("recipe %s input %s exclude: %w", recipe.ID, input.Path, excludeErr)
			}
			if excluded {
				return nil
			}
			return addFile(path)
		})
		if err != nil {
			return nil, err
		}
	}
	sort.Slice(inputs, func(i, j int) bool { return inputs[i].Path < inputs[j].Path })
	return inputs, nil
}

func snapshotDiscoveredInputs(workspace *config.Workspace, paths []string, hasher *Hasher, requireExists bool) ([]recipeInputState, error) {
	inputs := make([]recipeInputState, 0, len(paths))
	for _, path := range paths {
		if !filepath.IsAbs(path) {
			path = workspace.Path(path)
		}
		path = filepath.Clean(path)
		info, err := os.Stat(path)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				if requireExists {
					return nil, fmt.Errorf("discovered input does not exist: %s", statePath(workspace, path))
				}
				continue
			}
			return nil, err
		}
		if !info.Mode().IsRegular() {
			continue
		}
		state := recipeInputState{Path: statePath(workspace, path)}
		if !pathWithinAbs(path, "/nix/store") {
			digest, err := hasher.Hash(path)
			if err != nil {
				return nil, err
			}
			state.SHA256 = hex.EncodeToString(digest)
		}
		inputs = append(inputs, state)
	}
	sort.Slice(inputs, func(i, j int) bool { return inputs[i].Path < inputs[j].Path })
	return inputs, nil
}

func snapshotNegativeDeps(workspace *config.Workspace, paths []string) (appeared string, err error) {
	for _, stored := range paths {
		path := stored
		if !filepath.IsAbs(path) {
			path = workspace.Path(path)
		}
		_, statErr := os.Lstat(path)
		if statErr == nil {
			return statePath(workspace, path), nil
		}
		if !errors.Is(statErr, os.ErrNotExist) {
			return "", statErr
		}
	}
	return "", nil
}

func statePath(workspace *config.Workspace, path string) string {
	path = filepath.Clean(path)
	if pathWithinAbs(path, workspace.Root) {
		return workspace.Rel(path)
	}
	return filepath.ToSlash(path)
}

func snapshotRecipeToolsInEnvironment(recipe config.Recipe, hasher *Hasher, env map[string]string) ([]recipeToolState, error) {
	tools := make([]recipeToolState, 0, len(recipe.Tools))
	seen := make(map[string]bool, len(recipe.Tools))
	for _, name := range recipe.Tools {
		if seen[name] {
			continue
		}
		seen[name] = true
		resolved, err := lookPathInEnvironment(name, env)
		if err != nil {
			return nil, fmt.Errorf("recipe %s tool %s: %w", recipe.ID, name, err)
		}
		resolved, err = filepath.Abs(resolved)
		if err != nil {
			return nil, err
		}
		resolved = filepath.Clean(resolved)
		digest, err := hasher.Hash(resolved)
		if err != nil {
			return nil, fmt.Errorf("hash recipe %s tool %s: %w", recipe.ID, name, err)
		}
		tools = append(tools, recipeToolState{Name: name, Path: resolved, SHA256: hex.EncodeToString(digest)})
	}
	sort.Slice(tools, func(i, j int) bool { return tools[i].Name < tools[j].Name })
	return tools, nil
}

func snapshotRecipeOutputs(workspace *config.Workspace, recipe config.Recipe, hasher *Hasher) ([]recipeOutputState, []string, error) {
	outputs := make([]recipeOutputState, 0, len(recipe.Outputs))
	var missing []string
	for _, declared := range recipe.Outputs {
		path := workspace.Path(declared)
		info, err := os.Stat(path)
		if errors.Is(err, os.ErrNotExist) {
			missing = append(missing, declared)
			continue
		}
		if err != nil {
			return nil, nil, err
		}
		kind := "file"
		var digest []byte
		if info.IsDir() {
			kind = "dir"
			digest, err = treeOutputDigest(path, hasher)
		} else {
			digest, err = hasher.Hash(path)
		}
		if err != nil {
			return nil, nil, err
		}
		outputs = append(outputs, recipeOutputState{Path: declared, Kind: kind, Digest: hex.EncodeToString(digest)})
	}
	sort.Slice(outputs, func(i, j int) bool { return outputs[i].Path < outputs[j].Path })
	sort.Strings(missing)
	return outputs, missing, nil
}

func readRecipeState(workspace *config.Workspace, recipeID string) (recipeState, bool, error) {
	data, err := os.ReadFile(recipeStatePath(workspace, recipeID))
	if errors.Is(err, os.ErrNotExist) {
		return recipeState{}, false, nil
	}
	if err != nil {
		return recipeState{}, false, err
	}
	var state recipeState
	if err := json.Unmarshal(data, &state); err != nil {
		return recipeState{}, true, err
	}
	return state, true, nil
}

func writeRecipeState(workspace *config.Workspace, recipeID string, state recipeState) error {
	path := recipeStatePath(workspace, recipeID)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	tmp, err := os.CreateTemp(filepath.Dir(path), ".recipe-state-*")
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

func recipeStatusInEnvironment(workspace *config.Workspace, recipe config.Recipe, hasher *Hasher, env map[string]string) (RecipeStatus, recipeState, error) {
	return recipeStatusWithSnapshot(workspace, recipe, hasher, env, true)
}

func recipeStatusOnlyInEnvironment(workspace *config.Workspace, recipe config.Recipe, hasher *Hasher, env map[string]string) (RecipeStatus, recipeState, error) {
	return recipeStatusWithSnapshot(workspace, recipe, hasher, env, false)
}

func recipeStatusWithSnapshot(workspace *config.Workspace, recipe config.Recipe, hasher *Hasher, env map[string]string, requireCurrent bool) (RecipeStatus, recipeState, error) {
	status := RecipeStatus{ID: recipe.ID}
	stored, exists, stateErr := readRecipeState(workspace, recipe.ID)
	if !requireCurrent {
		if !exists {
			status.Reason = "no state"
			return status, recipeState{}, nil
		}
		if stateErr != nil {
			status.Reason = "state unreadable"
			return status, recipeState{}, nil
		}
		if !reflect.DeepEqual(stored.Run, recipe.Run) {
			status.Reason = "run argv changed"
			return status, recipeState{}, nil
		}
		if !reflect.DeepEqual(stored.Discover, recipe.Discover) {
			status.Reason = "discovery configuration changed"
			return status, recipeState{}, nil
		}
	}
	current := recipeState{Run: append([]string(nil), recipe.Run...), Discover: append([]config.Discovery(nil), recipe.Discover...)}
	tools, err := snapshotRecipeToolsInEnvironment(recipe, hasher, env)
	if err != nil {
		return status, recipeState{}, err
	}
	current.Tools = tools
	if !requireCurrent {
		if reason := firstToolDifference(stored.Tools, current.Tools); reason != "" {
			status.Reason = reason
			return status, current, nil
		}
	}
	inputs, err := snapshotRecipeInputs(workspace, recipe, hasher)
	if err != nil {
		return status, recipeState{}, err
	}
	current.DeclaredInputs = inputs
	if !requireCurrent {
		if reason := firstInputDifference("declared input", stored.DeclaredInputs, current.DeclaredInputs); reason != "" {
			status.Reason = reason
			return status, current, nil
		}
		discoveredPaths := make([]string, 0, len(stored.DiscoveredInputs))
		for _, input := range stored.DiscoveredInputs {
			discoveredPaths = append(discoveredPaths, input.Path)
		}
		current.DiscoveredInputs, err = snapshotDiscoveredInputs(workspace, discoveredPaths, hasher, false)
		if err != nil {
			return status, recipeState{}, err
		}
		if reason := firstInputDifference("discovered input", stored.DiscoveredInputs, current.DiscoveredInputs); reason != "" {
			status.Reason = reason
			return status, current, nil
		}
		current.NegativeDeps = append([]string(nil), stored.NegativeDeps...)
		if appeared, negativeErr := snapshotNegativeDeps(workspace, stored.NegativeDeps); negativeErr != nil {
			return status, recipeState{}, negativeErr
		} else if appeared != "" {
			status.Reason = "negative dep appeared: " + appeared
			return status, current, nil
		}
	}
	outputs, missing, err := snapshotRecipeOutputs(workspace, recipe, hasher)
	if err != nil {
		return status, recipeState{}, err
	}
	current.Outputs = outputs
	needs, err := snapshotRecipeNeeds(workspace, recipe, hasher)
	if err != nil {
		return status, recipeState{}, err
	}
	current.Needs = needs
	if !exists {
		status.Reason = "no state"
		return status, current, nil
	}
	if stateErr != nil {
		status.Reason = "state unreadable"
		return status, current, nil
	}
	if !reflect.DeepEqual(stored.Run, current.Run) {
		status.Reason = "run argv changed"
		return status, current, nil
	}
	if !reflect.DeepEqual(stored.Discover, current.Discover) {
		status.Reason = "discovery configuration changed"
		return status, current, nil
	}
	if reason := firstToolDifference(stored.Tools, current.Tools); reason != "" {
		status.Reason = reason
		return status, current, nil
	}
	if reason := firstInputDifference("declared input", stored.DeclaredInputs, current.DeclaredInputs); reason != "" {
		status.Reason = reason
		return status, current, nil
	}
	if requireCurrent {
		discoveredPaths := make([]string, 0, len(stored.DiscoveredInputs))
		for _, input := range stored.DiscoveredInputs {
			discoveredPaths = append(discoveredPaths, input.Path)
		}
		current.DiscoveredInputs, err = snapshotDiscoveredInputs(workspace, discoveredPaths, hasher, false)
		if err != nil {
			return status, recipeState{}, err
		}
		current.NegativeDeps = append([]string(nil), stored.NegativeDeps...)
	}
	if reason := firstInputDifference("discovered input", stored.DiscoveredInputs, current.DiscoveredInputs); reason != "" {
		status.Reason = reason
		return status, current, nil
	}
	if appeared, negativeErr := snapshotNegativeDeps(workspace, stored.NegativeDeps); negativeErr != nil {
		return status, recipeState{}, negativeErr
	} else if appeared != "" {
		status.Reason = "negative dep appeared: " + appeared
		return status, current, nil
	}
	if len(missing) != 0 {
		status.Reason = "output missing: " + missing[0]
		return status, current, nil
	}
	if reason := firstOutputDifference(stored.Outputs, current.Outputs); reason != "" {
		status.Reason = reason
		return status, current, nil
	}
	if reason := firstNeedDifference(stored.Needs, current.Needs); reason != "" {
		status.Reason = reason
		return status, current, nil
	}
	status.UpToDate = true
	status.Reason = "up-to-date"
	return status, current, nil
}

func firstToolDifference(before, after []recipeToolState) string {
	limit := len(before)
	if len(after) < limit {
		limit = len(after)
	}
	for index := 0; index < limit; index++ {
		if before[index].Name != after[index].Name {
			return "tool set changed: " + after[index].Name
		}
		if before[index].Path != after[index].Path {
			return "toolchain path changed: " + after[index].Name
		}
		if before[index].SHA256 != after[index].SHA256 {
			return "tool changed: " + after[index].Name
		}
	}
	if len(before) != len(after) {
		if len(after) > limit {
			return "tool set changed: " + after[limit].Name
		}
		return "tool set changed: " + before[limit].Name
	}
	return ""
}

func firstInputDifference(kind string, before, after []recipeInputState) string {
	limit := len(before)
	if len(after) < limit {
		limit = len(after)
	}
	for index := 0; index < limit; index++ {
		if before[index].Path != after[index].Path {
			return kind + " set changed: " + after[index].Path
		}
		if before[index].SHA256 != after[index].SHA256 {
			return kind + " changed: " + after[index].Path
		}
	}
	if len(before) != len(after) {
		if len(after) > limit {
			return kind + " set changed: " + after[limit].Path
		}
		return kind + " set changed: " + before[limit].Path
	}
	return ""
}

// snapshotRecipeNeeds records the outputs of the recipes this one needs. A
// recipe may read a needed output without declaring or discovering each file
// in it, so a need rebuilt by a run that did not select this recipe must still
// make it stale.
func snapshotRecipeNeeds(workspace *config.Workspace, recipe config.Recipe, hasher *Hasher) ([]recipeNeedState, error) {
	if len(recipe.Needs) == 0 {
		return nil, nil
	}
	needs := make([]recipeNeedState, 0, len(recipe.Needs))
	for _, id := range recipe.Needs {
		need, ok := workspace.Recipe(id)
		if !ok {
			return nil, fmt.Errorf("recipe %s needs unknown recipe %s", recipe.ID, id)
		}
		// A missing output is left out, so the snapshot cannot match one taken
		// while it existed.
		outputs, _, err := snapshotRecipeOutputs(workspace, need, hasher)
		if err != nil {
			return nil, err
		}
		needs = append(needs, recipeNeedState{ID: id, Outputs: outputs})
	}
	sort.Slice(needs, func(i, j int) bool { return needs[i].ID < needs[j].ID })
	return needs, nil
}

func firstNeedDifference(before, after []recipeNeedState) string {
	stored := make(map[string][]recipeOutputState, len(before))
	for _, need := range before {
		stored[need.ID] = need.Outputs
	}
	for _, need := range after {
		outputs, ok := stored[need.ID]
		if !ok {
			return "need added: " + need.ID
		}
		if firstOutputDifference(outputs, need.Outputs) != "" {
			return "needed recipe output changed: " + need.ID
		}
		delete(stored, need.ID)
	}
	for id := range stored {
		return "need removed: " + id
	}
	return ""
}

func firstOutputDifference(before, after []recipeOutputState) string {
	limit := len(before)
	if len(after) < limit {
		limit = len(after)
	}
	for index := 0; index < limit; index++ {
		if before[index].Path != after[index].Path || before[index].Kind != after[index].Kind {
			return "output set changed: " + after[index].Path
		}
		if before[index].Digest != after[index].Digest {
			return "output changed: " + after[index].Path
		}
	}
	if len(before) != len(after) {
		if len(after) > limit {
			return "output set changed: " + after[limit].Path
		}
		return "output missing: " + before[limit].Path
	}
	return ""
}

func RecipeStatuses(workspace *config.Workspace) ([]RecipeStatus, error) {
	return recipeStatuses(workspace, workspace.Recipes())
}

// ActiveRecipeStatuses checks only the recipes that produce artifacts of
// enabled apps, plus everything they need.
func ActiveRecipeStatuses(workspace *config.Workspace) ([]RecipeStatus, error) {
	var roots []string
	for _, app := range workspace.Apps() {
		if workspace.Skipped(app) || app.Kind() != "file" {
			continue
		}
		source, err := app.FileSource()
		if err != nil {
			return nil, err
		}
		if source.Recipe != "" {
			roots = append(roots, source.Recipe)
		}
	}
	selected, err := selectedRecipeClosure(workspace, roots)
	if err != nil {
		return nil, err
	}
	recipes := make([]config.Recipe, 0, len(selected))
	for _, recipe := range workspace.Recipes() {
		if _, ok := selected[recipe.ID]; ok {
			recipes = append(recipes, recipe)
		}
	}
	return recipeStatuses(workspace, recipes)
}

func recipeStatuses(workspace *config.Workspace, recipes []config.Recipe) ([]RecipeStatus, error) {
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	environments := newRecipeEnvironment(workspace, hasher)
	statuses := make([]RecipeStatus, 0, len(recipes))
	for _, recipe := range recipes {
		env, envErr := environments.variables()
		if envErr != nil {
			statuses = append(statuses, RecipeStatus{ID: recipe.ID, Reason: "check failed: " + envErr.Error()})
			continue
		}
		status, _, err := recipeStatusOnlyInEnvironment(workspace, recipe, hasher, env)
		if err != nil {
			statuses = append(statuses, RecipeStatus{ID: recipe.ID, Reason: "check failed: " + err.Error()})
			continue
		}
		statuses = append(statuses, status)
	}
	return statuses, nil
}

func AdoptRecipes(workspace *config.Workspace, recipeIDs []string) ([]string, error) {
	hasher := hasherFor(workspace)
	defer flushHasher(hasher)
	environments := newRecipeEnvironment(workspace, hasher)
	var recipes []config.Recipe
	if len(recipeIDs) == 0 {
		recipes = workspace.Recipes()
	} else {
		seen := make(map[string]bool)
		for _, id := range recipeIDs {
			if seen[id] {
				return nil, fmt.Errorf("duplicate recipe: %s", id)
			}
			recipe, ok := workspace.Recipe(id)
			if !ok {
				return nil, fmt.Errorf("recipe not found: %s", id)
			}
			seen[id] = true
			recipes = append(recipes, recipe)
		}
	}
	var adopted []string
	for _, recipe := range recipes {
		outputs, missing, err := snapshotRecipeOutputs(workspace, recipe, hasher)
		if err != nil {
			return nil, err
		}
		if len(missing) != 0 {
			if len(recipeIDs) == 0 {
				continue
			}
			return nil, fmt.Errorf("cannot adopt recipe %s; missing outputs: %s", recipe.ID, strings.Join(missing, ", "))
		}
		inputs, err := snapshotRecipeInputs(workspace, recipe, hasher)
		if err != nil {
			return nil, err
		}
		env, err := environments.variables()
		if err != nil {
			return nil, err
		}
		tools, err := snapshotRecipeToolsInEnvironment(recipe, hasher, env)
		if err != nil {
			return nil, err
		}
		discovered, err := discoverRecipeInputs(workspace, recipe, env)
		if err != nil {
			return nil, fmt.Errorf("cannot adopt recipe %s: %w", recipe.ID, err)
		}
		discoveredInputs, err := snapshotDiscoveredInputs(workspace, discovered.Files, hasher, true)
		if err != nil {
			return nil, err
		}
		negative := make([]string, 0, len(discovered.Negative))
		for _, path := range discovered.Negative {
			negative = append(negative, statePath(workspace, path))
		}
		needs, err := snapshotRecipeNeeds(workspace, recipe, hasher)
		if err != nil {
			return nil, err
		}
		state := recipeState{
			Run:      append([]string(nil), recipe.Run...),
			Discover: append([]config.Discovery(nil), recipe.Discover...), DeclaredInputs: inputs,
			DiscoveredInputs: discoveredInputs, NegativeDeps: negative, Tools: tools, Outputs: outputs,
			Needs: needs,
		}
		if err := writeRecipeState(workspace, recipe.ID, state); err != nil {
			return nil, err
		}
		adopted = append(adopted, recipe.ID)
	}
	return adopted, nil
}

func warnRecipeInputsChanged(workspace *config.Workspace, recipeID string, before, after []recipeInputState) {
	var changed []string
	beforeMap := make(map[string]string, len(before))
	afterMap := make(map[string]string, len(after))
	for _, input := range before {
		beforeMap[input.Path] = input.SHA256
	}
	for _, input := range after {
		afterMap[input.Path] = input.SHA256
	}
	for path, digest := range beforeMap {
		if afterMap[path] != digest {
			changed = append(changed, path)
		}
	}
	for path, digest := range afterMap {
		if beforeMap[path] != digest {
			if _, present := beforeMap[path]; !present {
				changed = append(changed, path)
			}
		}
	}
	if len(changed) == 0 {
		return
	}
	sort.Strings(changed)
	const limit = 10
	shown := changed
	if len(shown) > limit {
		shown = shown[:limit]
	}
	suffix := ""
	if len(changed) > len(shown) {
		suffix = fmt.Sprintf(" (and %d more)", len(changed)-len(shown))
	}
	fmt.Fprintf(os.Stderr, "warning: inputs changed while rebuilding recipe %s: %s%s; stored the pre-build digests so the next run rebuilds\n", recipeID, strings.Join(shown, ", "), suffix)
}

func sameOutputDigest(left, right recipeOutputState) bool {
	return left.Path == right.Path && left.Kind == right.Kind && left.Digest == right.Digest
}
