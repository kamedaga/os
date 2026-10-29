package buildsys

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

type recipeOutcome struct {
	ID       string
	Run      recipeRun
	Ran      bool
	Changed  bool
	Implicit bool
	Err      error
	LogPath  string
}

type recipeFailures struct {
	items []recipeOutcome
}

func (e *recipeFailures) Error() string {
	ids := make([]string, 0, len(e.items))
	for _, item := range e.items {
		ids = append(ids, item.ID)
	}
	sort.Strings(ids)
	message := "recipes failed: " + strings.Join(ids, ", ")
	if len(e.items) == 1 && e.items[0].Err != nil {
		message += ": " + e.items[0].Err.Error()
	}
	return message
}

func selectedRecipeClosure(workspace *config.Workspace, roots []string) (map[string]config.Recipe, error) {
	selected := make(map[string]config.Recipe)
	var visit func(string) error
	visit = func(id string) error {
		if _, exists := selected[id]; exists {
			return nil
		}
		recipe, ok := workspace.Recipe(id)
		if !ok {
			return fmt.Errorf("recipe not found: %s", id)
		}
		selected[id] = recipe
		for _, need := range recipe.Needs {
			if err := visit(need); err != nil {
				return err
			}
		}
		return nil
	}
	for _, id := range roots {
		if err := visit(id); err != nil {
			return nil, err
		}
	}
	return selected, nil
}

func scheduleRecipes(workspace *config.Workspace, roots []string, force bool, hasher *Hasher, span progress.Span) (map[string]recipeRun, error) {
	selected, err := selectedRecipeClosure(workspace, roots)
	if err != nil {
		return nil, err
	}
	runs := make(map[string]recipeRun)
	if len(selected) == 0 {
		return runs, nil
	}
	server, err := newJobserver(workspace)
	if err != nil {
		return nil, fmt.Errorf("create jobserver: %w", err)
	}
	defer func() {
		if err := server.Close(); err != nil {
			fmt.Fprintf(os.Stderr, "warning: could not remove jobserver: %v\n", err)
		}
	}()
	environments := newRecipeEnvironment(workspace, hasher)
	indegree := make(map[string]int, len(selected))
	dependents := make(map[string][]string, len(selected))
	for id, recipe := range selected {
		for _, need := range recipe.Needs {
			if _, ok := selected[need]; !ok {
				continue
			}
			indegree[id]++
			dependents[need] = append(dependents[need], id)
		}
	}
	for id := range dependents {
		sort.Strings(dependents[id])
	}
	ready := make([]string, 0, len(selected))
	for id := range selected {
		if indegree[id] == 0 {
			ready = append(ready, id)
		}
	}
	sort.Strings(ready)
	outcomes := make(map[string]recipeOutcome, len(selected))
	results := make(chan recipeOutcome, len(selected))
	running := make(map[string]bool)
	implicitInUse := false
	failed := false
	completed := 0
	var failures []recipeOutcome

	complete := func(outcome recipeOutcome) {
		outcomes[outcome.ID] = outcome
		completed++
		for _, dependent := range dependents[outcome.ID] {
			indegree[dependent]--
			if indegree[dependent] == 0 {
				ready = append(ready, dependent)
			}
		}
		sort.Strings(ready)
	}
	refreshProgress := func() {
		ids := make([]string, 0, len(running))
		for id := range running {
			ids = append(ids, id)
		}
		sort.Strings(ids)
		if len(ids) == 0 {
			span.Message("recipe scheduling")
		} else {
			span.Message("running: " + strings.Join(ids, ", "))
		}
	}

	for completed < len(selected) {
		madeProgress := false
		for !failed && len(ready) != 0 && len(running) < server.jobs {
			id := ready[0]
			ready = ready[1:]
			recipe := selected[id]
			env, envErr := environments.variables()
			if envErr != nil {
				outcome := recipeOutcome{ID: id, Err: envErr, LogPath: recipeLogPath(workspace, id)}
				failures = append(failures, outcome)
				failed = true
				complete(outcome)
				madeProgress = true
				break
			}
			status, current, statusErr := recipeStatusInEnvironment(workspace, recipe, hasher, env)
			if statusErr != nil {
				outcome := recipeOutcome{ID: id, Err: statusErr, LogPath: recipeLogPath(workspace, id)}
				failures = append(failures, outcome)
				failed = true
				complete(outcome)
				madeProgress = true
				break
			}
			upstreamChanged := false
			for _, need := range recipe.Needs {
				upstream := outcomes[need]
				if upstream.Ran && upstream.Changed {
					upstreamChanged = true
					break
				}
			}
			if status.UpToDate && !force && !upstreamChanged {
				complete(recipeOutcome{ID: id})
				madeProgress = true
				continue
			}
			implicit := !implicitInUse
			if implicit {
				implicitInUse = true
			} else if err := server.acquire(); err != nil {
				outcome := recipeOutcome{ID: id, Err: fmt.Errorf("acquire jobserver token: %w", err), LogPath: recipeLogPath(workspace, id)}
				failures = append(failures, outcome)
				failed = true
				complete(outcome)
				break
			}
			running[id] = true
			refreshProgress()
			madeProgress = true
			go func() {
				run, logPath, runErr := runRecipe(workspace, recipe, current, hasher, env, server)
				outcome := recipeOutcome{ID: id, Run: run, Ran: true, Changed: run.Changed, Implicit: implicit, Err: runErr, LogPath: logPath}
				results <- outcome
				if !implicit {
					if releaseErr := server.release(); releaseErr != nil && outcome.Err == nil {
						fmt.Fprintf(os.Stderr, "warning: could not return jobserver token for %s: %v\n", id, releaseErr)
					}
				}
			}()
		}

		if len(running) != 0 {
			outcome := <-results
			delete(running, outcome.ID)
			if outcome.Implicit {
				implicitInUse = false
			}
			if outcome.Err != nil {
				failures = append(failures, outcome)
				failed = true
			} else {
				runs[outcome.ID] = outcome.Run
			}
			complete(outcome)
			refreshProgress()
			continue
		}
		if failed {
			break
		}
		if !madeProgress {
			return runs, errors.New("recipe scheduler made no progress")
		}
	}
	if len(failures) != 0 {
		sort.Slice(failures, func(i, j int) bool { return failures[i].ID < failures[j].ID })
		for _, failure := range failures {
			printRecipeFailure(failure)
		}
		return runs, &recipeFailures{items: failures}
	}
	return runs, nil
}

func recipeLogPath(workspace *config.Workspace, id string) string {
	return workspace.Path(workspace.Artifacts, "pack", "logs", sanitizeFilename(id)+".log")
}

func runRecipe(workspace *config.Workspace, definition config.Recipe, pre recipeState, hasher *Hasher, env map[string]string, server *jobserver) (recipeRun, string, error) {
	return runRecipeWithPrefix(workspace, definition, pre, hasher, env, server, nil)
}

func runRecipeWithPrefix(workspace *config.Workspace, definition config.Recipe, pre recipeState, hasher *Hasher, env map[string]string, server *jobserver, prefix []string) (recipeRun, string, error) {
	recipe := recipeRun{Before: make(map[string]recipeOutputState, len(pre.Outputs))}
	for _, output := range pre.Outputs {
		recipe.Before[workspace.Path(output.Path)] = output
	}
	logPath := recipeLogPath(workspace, definition.ID)
	if err := os.MkdirAll(filepath.Dir(logPath), 0o755); err != nil {
		return recipe, logPath, err
	}
	log, err := os.Create(logPath)
	if err != nil {
		return recipe, logPath, err
	}
	defer log.Close()
	executable, err := lookPathInEnvironment(definition.Run[0], env)
	if err != nil {
		return recipe, logPath, fmt.Errorf("recipe %s command %s: %w", definition.ID, definition.Run[0], err)
	}
	commandPath := executable
	commandArgs := append([]string(nil), definition.Run[1:]...)
	if len(prefix) != 0 {
		commandPath = prefix[0]
		commandArgs = append(append([]string(nil), prefix[1:]...), "--", executable)
		commandArgs = append(commandArgs, definition.Run[1:]...)
	}
	cmd := exec.Command(commandPath, commandArgs...)
	if len(prefix) == 0 {
		cmd.Args[0] = definition.Run[0]
	}
	cmd.Dir = workspace.Root
	cmd.Stdout = log
	cmd.Stderr = log
	cmd.Stdin = nil
	server.configureCommand(env)
	cmd.Env = environmentList(env)
	started := time.Now()
	if os.Getenv("PACGO_PROGRESS") == "plain" {
		progressOutputMu.Lock()
		fmt.Printf("start %s %.3fs\n", definition.ID, 0.0)
		progressOutputMu.Unlock()
	}
	runErr := cmd.Run()
	duration := time.Since(started)
	if os.Getenv("PACGO_PROGRESS") == "plain" {
		progressOutputMu.Lock()
		fmt.Printf("finish %s %.3fs\n", definition.ID, duration.Seconds())
		progressOutputMu.Unlock()
	}
	var invalidated []string
	invalidated = append(invalidated, recipeInputs(workspace, definition)...)
	for _, output := range definition.Outputs {
		invalidated = append(invalidated, workspace.Path(output))
	}
	hasher.Invalidate(invalidated...)
	if runErr != nil {
		return recipe, logPath, fmt.Errorf("%s failed after %s: %w", strings.Join(definition.Run, " "), duration.Round(time.Millisecond), runErr)
	}
	postInputs, err := snapshotRecipeInputs(workspace, definition, hasher)
	if err != nil {
		return recipe, logPath, err
	}
	postOutputs, missing, err := snapshotRecipeOutputs(workspace, definition, hasher)
	if err != nil {
		return recipe, logPath, err
	}
	if len(missing) != 0 {
		return recipe, logPath, fmt.Errorf("recipe %s did not create declared outputs: %s", definition.ID, strings.Join(missing, ", "))
	}
	discovered, err := discoverRecipeInputs(workspace, definition, env)
	if err != nil {
		return recipe, logPath, err
	}
	hasher.Invalidate(discovered.Files...)
	discoveredInputs, err := snapshotDiscoveredInputs(workspace, discovered.Files, hasher, true)
	if err != nil {
		return recipe, logPath, err
	}
	negative := make([]string, 0, len(discovered.Negative))
	for _, path := range discovered.Negative {
		negative = append(negative, statePath(workspace, path))
	}
	warnRecipeInputsChanged(workspace, definition.ID, pre.DeclaredInputs, postInputs)
	state := recipeState{
		Run:      append([]string(nil), definition.Run...),
		Discover: append([]config.Discovery(nil), definition.Discover...), DeclaredInputs: pre.DeclaredInputs,
		DiscoveredInputs: discoveredInputs, NegativeDeps: negative, Tools: pre.Tools, Outputs: postOutputs,
		Needs: pre.Needs,
	}
	if err := writeRecipeState(workspace, definition.ID, state); err != nil {
		return recipe, logPath, err
	}
	recipe.Changed = firstOutputDifference(pre.Outputs, postOutputs) != ""
	return recipe, logPath, nil
}

func printRecipeFailure(failure recipeOutcome) {
	fmt.Fprintf(os.Stderr, "recipe %s failed; last 60 log lines (%s):\n", failure.ID, failure.LogPath)
	file, err := os.Open(failure.LogPath)
	if err == nil {
		defer file.Close()
		var lines []string
		scanner := bufio.NewScanner(file)
		for scanner.Scan() {
			lines = append(lines, scanner.Text())
			if len(lines) > 60 {
				lines = lines[1:]
			}
		}
		for _, line := range lines {
			fmt.Fprintln(os.Stderr, line)
		}
	}
	fmt.Fprintf(os.Stderr, "recipe %s error: %v\n", failure.ID, failure.Err)
}

var progressOutputMu sync.Mutex
