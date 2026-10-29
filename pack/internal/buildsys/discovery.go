package buildsys

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"capabilityos/pack/internal/config"
)

type discoveredPaths struct {
	Files    []string
	Negative []string
}

type kbuildDependencyValue struct {
	Target   string
	Value    string
	IsSource bool
}

type kbuildWorkingDir struct {
	Build  string
	Source string
}

var makeWildcardPattern = regexp.MustCompile(`\$\(\s*wildcard\s+([^)]*)\)`)

func discoverRecipeInputs(workspace *config.Workspace, recipe config.Recipe, env map[string]string) (discoveredPaths, error) {
	files := make(map[string]bool)
	negative := make(map[string]bool)
	for _, source := range recipe.Discover {
		var found discoveredPaths
		var err error
		switch source.Kind {
		case "kbuild":
			found, err = discoverKbuild(workspace.Path(source.Dir))
		case "kbuild-dir-file":
			found, err = discoverKbuildDirFile(workspace, source.Dir)
		case "ninja":
			found, err = discoverNinja(workspace.Path(source.Dir), env)
		case "depfile":
			found, err = discoverDepfiles(workspace, source.Glob)
		default:
			err = fmt.Errorf("unsupported discovery kind %q", source.Kind)
		}
		if err != nil {
			return discoveredPaths{}, fmt.Errorf("recipe %s %s discovery: %w", recipe.ID, source.Kind, err)
		}
		for _, path := range found.Files {
			path, keep, err := classifyDiscoveredPath(workspace, recipe, path)
			if err != nil {
				return discoveredPaths{}, err
			}
			if keep {
				files[path] = true
				delete(negative, path)
			}
		}
		for _, path := range found.Negative {
			path, keep, err := classifyDiscoveredPath(workspace, recipe, path)
			if err != nil {
				return discoveredPaths{}, err
			}
			if keep && !files[path] {
				negative[path] = true
			}
		}
	}
	result := discoveredPaths{Files: mapKeys(files), Negative: mapKeys(negative)}
	return result, nil
}

func classifyDiscoveredPath(workspace *config.Workspace, consumer config.Recipe, path string) (string, bool, error) {
	if !filepath.IsAbs(path) {
		path = workspace.Path(path)
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", false, err
	}
	abs = filepath.Clean(abs)
	owner := ""
	for _, recipe := range workspace.Recipes() {
		for _, output := range recipe.Outputs {
			if pathWithinAbs(abs, workspace.Path(output)) {
				owner = recipe.ID
				break
			}
		}
		if owner != "" {
			break
		}
	}
	if owner == consumer.ID {
		return abs, false, nil
	}
	if owner != "" {
		if !containsString(consumer.Needs, owner) {
			return "", false, fmt.Errorf("recipe %s discovered dependency %s from recipe %s without an edge; add %s to %s.needs", consumer.ID, workspace.Rel(abs), owner, owner, consumer.ID)
		}
		return abs, true, nil
	}
	if pathWithinAbs(abs, workspace.Path(workspace.Artifacts)) {
		return abs, false, nil
	}
	return abs, true, nil
}

func discoverKbuild(buildDir string) (discoveredPaths, error) {
	info, err := os.Stat(buildDir)
	if err != nil {
		return discoveredPaths{}, fmt.Errorf("build directory %s: %w", buildDir, err)
	}
	if !info.IsDir() {
		return discoveredPaths{}, fmt.Errorf("build directory %s is not a directory", buildDir)
	}
	srctree, err := kbuildSourceTree(buildDir)
	if err != nil {
		return discoveredPaths{}, err
	}
	sourceIndex, err := kbuildSourceIndex(srctree)
	if err != nil {
		return discoveredPaths{}, err
	}
	var commandFiles []string
	err = filepath.WalkDir(buildDir, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.Type().IsRegular() && strings.HasPrefix(entry.Name(), ".") && strings.HasSuffix(entry.Name(), ".cmd") {
			commandFiles = append(commandFiles, path)
		}
		return nil
	})
	if err != nil {
		return discoveredPaths{}, err
	}
	autoConf := filepath.Join(buildDir, "include", "config", "auto.conf.cmd")
	if info, statErr := os.Stat(autoConf); statErr == nil && info.Mode().IsRegular() {
		commandFiles = append(commandFiles, autoConf)
	}
	if len(commandFiles) == 0 {
		return discoveredPaths{}, fmt.Errorf("no .cmd files found in %s", buildDir)
	}
	sort.Strings(commandFiles)
	files := make(map[string]bool)
	negative := make(map[string]bool)
	wildcardState := make(map[string]bool)
	for _, commandFile := range commandFiles {
		data, readErr := os.ReadFile(commandFile)
		if readErr != nil {
			return discoveredPaths{}, readErr
		}
		values := kbuildDependencyValues(string(data))
		workingDirs, workingDirErr := kbuildWorkingDirs(values, srctree, buildDir, commandFile, sourceIndex)
		if workingDirErr != nil {
			return discoveredPaths{}, workingDirErr
		}
		for _, dependency := range values {
			workingDir := workingDirs[dependency.Target]
			for _, match := range makeWildcardPattern.FindAllStringSubmatch(dependency.Value, -1) {
				for _, token := range splitMakeWords(match[1]) {
					resolved, exists, resolveErr := resolveKbuildPath(token, srctree, buildDir, workingDir)
					if resolveErr != nil {
						return discoveredPaths{}, resolveErr
					}
					cachedExists, known := wildcardState[resolved]
					if !known {
						wildcardState[resolved] = exists
					} else {
						exists = cachedExists
					}
					if exists {
						files[resolved] = true
					} else {
						negative[resolved] = true
					}
				}
			}
			withoutWildcards := makeWildcardPattern.ReplaceAllString(dependency.Value, " ")
			for _, token := range splitMakeWords(withoutWildcards) {
				if token == "" || strings.Contains(token, "$(") {
					continue
				}
				resolved, exists, resolveErr := resolveKbuildPath(token, srctree, buildDir, workingDir)
				if resolveErr != nil {
					return discoveredPaths{}, resolveErr
				}
				if !exists {
					return discoveredPaths{}, fmt.Errorf("dependency %q from %s does not exist relative to O=%s or srctree=%s", token, commandFile, buildDir, srctree)
				}
				files[resolved] = true
			}
		}
	}
	return discoveredPaths{Files: mapKeys(files), Negative: mapKeys(negative)}, nil
}

func discoverKbuildDirFile(workspace *config.Workspace, path string) (discoveredPaths, error) {
	data, err := os.ReadFile(workspace.Path(path))
	if err != nil {
		return discoveredPaths{}, fmt.Errorf("read kbuild directory file %s: %w", path, err)
	}
	value := strings.TrimSpace(string(data))
	if value == "" || strings.ContainsAny(value, "\r\n") {
		return discoveredPaths{}, fmt.Errorf("kbuild directory file %s must contain exactly one path", path)
	}
	if !filepath.IsAbs(value) {
		value = workspace.Path(filepath.FromSlash(value))
	}
	value = filepath.Clean(value)
	if !pathWithinAbs(value, workspace.Path(workspace.Artifacts)) {
		return discoveredPaths{}, fmt.Errorf("kbuild directory from %s is outside %s: %s", path, workspace.Artifacts, value)
	}
	return discoverKbuild(value)
}

func kbuildSourceTree(buildDir string) (string, error) {
	data, err := os.ReadFile(filepath.Join(buildDir, "Makefile"))
	if err != nil {
		return "", fmt.Errorf("read generated kbuild Makefile: %w", err)
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "include ") || !strings.HasSuffix(line, "/Makefile") {
			continue
		}
		path := strings.TrimSpace(strings.TrimPrefix(line, "include "))
		if filepath.IsAbs(path) {
			return filepath.Dir(filepath.Clean(path)), nil
		}
	}
	return "", fmt.Errorf("generated kbuild Makefile in %s does not identify the source tree", buildDir)
}

func kbuildDependencyValues(text string) []kbuildDependencyValue {
	logical := joinMakeContinuations(text)
	var values []kbuildDependencyValue
	for _, line := range strings.Split(logical, "\n") {
		name, value, ok := strings.Cut(line, ":=")
		if !ok {
			continue
		}
		name = strings.TrimSpace(name)
		if strings.HasPrefix(name, "source_") || strings.HasPrefix(name, "deps_") || name == "deps_config" {
			target := ""
			if name != "deps_config" {
				target = strings.TrimPrefix(strings.TrimPrefix(name, "source_"), "deps_")
			}
			values = append(values, kbuildDependencyValue{
				Target: target, Value: strings.TrimSpace(value), IsSource: strings.HasPrefix(name, "source_"),
			})
		}
	}
	return values
}

func kbuildSourceIndex(srctree string) (map[string][]string, error) {
	result := make(map[string][]string)
	err := filepath.WalkDir(srctree, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if !entry.Type().IsRegular() {
			return nil
		}
		relative, err := filepath.Rel(srctree, path)
		if err != nil {
			return err
		}
		result[entry.Name()] = append(result[entry.Name()], relative)
		return nil
	})
	return result, err
}

func kbuildWorkingDirs(values []kbuildDependencyValue, srctree, buildDir, commandFile string, sourceIndex map[string][]string) (map[string]kbuildWorkingDir, error) {
	result := make(map[string]kbuildWorkingDir)
	commandDir, err := filepath.Rel(buildDir, filepath.Dir(commandFile))
	if err != nil || commandDir == ".." || filepath.IsAbs(commandDir) || strings.HasPrefix(commandDir, ".."+string(filepath.Separator)) {
		return nil, fmt.Errorf("command file %s is outside build directory %s", commandFile, buildDir)
	}
	var candidates []string
	for current := filepath.Clean(commandDir); ; current = filepath.Dir(current) {
		if current == "." {
			candidates = append(candidates, "")
			break
		}
		candidates = append(candidates, current)
	}
	for _, dependency := range values {
		if dependency.Target == "" || !dependency.IsSource {
			continue
		}
		var source string
		for _, token := range splitMakeWords(dependency.Value) {
			if token != "" && !strings.Contains(token, "$(") {
				source = token
				break
			}
		}
		if source == "" || filepath.IsAbs(source) {
			continue
		}
		for _, candidate := range candidates {
			workingDir := kbuildWorkingDir{Build: candidate, Source: candidate}
			_, exists, resolveErr := resolveKbuildPath(source, srctree, buildDir, workingDir)
			if resolveErr != nil {
				return nil, resolveErr
			}
			if exists {
				result[dependency.Target] = workingDir
				break
			}
		}
		if _, exists := result[dependency.Target]; exists {
			continue
		}
		matches := sourceIndex[filepath.Base(source)]
		var matched string
		for _, candidate := range matches {
			candidate = filepath.Clean(candidate)
			suffix := filepath.Clean(filepath.FromSlash(source))
			if candidate == suffix || strings.HasSuffix(candidate, string(filepath.Separator)+suffix) {
				if matched != "" {
					matched = ""
					break
				}
				matched = candidate
			}
		}
		if matched != "" {
			sourceDir := strings.TrimSuffix(filepath.ToSlash(matched), filepath.ToSlash(filepath.Clean(filepath.FromSlash(source))))
			sourceDir = strings.TrimSuffix(sourceDir, "/")
			result[dependency.Target] = kbuildWorkingDir{
				Build: commandDir, Source: filepath.FromSlash(sourceDir),
			}
		}
	}
	return result, nil
}

func resolveKbuildPath(path, srctree, buildDir string, workingDir kbuildWorkingDir) (string, bool, error) {
	if filepath.IsAbs(path) {
		clean := filepath.Clean(path)
		_, err := os.Lstat(clean)
		if err == nil {
			return clean, true, nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return "", false, err
		}
		return clean, false, nil
	}
	buildRelative := filepath.Join(workingDir.Build, filepath.FromSlash(path))
	buildPath := filepath.Clean(filepath.Join(buildDir, buildRelative))
	if _, err := os.Lstat(buildPath); err == nil {
		return buildPath, true, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", false, err
	}
	sourceRelative := filepath.Join(workingDir.Source, filepath.FromSlash(path))
	sourcePath := filepath.Clean(filepath.Join(srctree, sourceRelative))
	if _, err := os.Lstat(sourcePath); err == nil {
		return sourcePath, true, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", false, err
	}
	return buildPath, false, nil
}

func discoverNinja(buildDir string, env map[string]string) (discoveredPaths, error) {
	info, err := os.Stat(buildDir)
	if err != nil {
		return discoveredPaths{}, fmt.Errorf("build directory %s: %w", buildDir, err)
	}
	if !info.IsDir() {
		return discoveredPaths{}, fmt.Errorf("build directory %s is not a directory", buildDir)
	}
	ninja, err := lookPathInEnvironment("ninja", env)
	if err != nil {
		return discoveredPaths{}, err
	}
	deps, err := ninjaToolOutput(ninja, buildDir, env, "deps")
	if err != nil {
		return discoveredPaths{}, err
	}
	targets, err := ninjaDefaultTargets(filepath.Join(buildDir, "build.ninja"))
	if err != nil {
		return discoveredPaths{}, err
	}
	if len(targets) == 0 {
		targetOutput, targetErr := ninjaToolOutput(ninja, buildDir, env, "targets", "depth", "0")
		if targetErr != nil {
			return discoveredPaths{}, targetErr
		}
		for _, line := range strings.Split(string(targetOutput), "\n") {
			if target, _, ok := strings.Cut(line, ":"); ok && strings.TrimSpace(target) != "" {
				targets = append(targets, strings.TrimSpace(target))
			}
		}
	}
	inputArgs := append([]string{"inputs"}, targets...)
	inputs, err := ninjaToolOutput(ninja, buildDir, env, inputArgs...)
	if err != nil {
		return discoveredPaths{}, err
	}
	// Ninja 1.13 lists nothing for "-t inputs build.ninja", so the CMake
	// regeneration inputs come from the manifest edge itself.
	manifest, err := ninjaToolOutput(ninja, buildDir, env, "query", "build.ninja")
	if err != nil {
		return discoveredPaths{}, err
	}
	files := make(map[string]bool)
	for _, path := range ninjaQueryInputs(manifest) {
		files[resolveBuildPath(path, buildDir)] = true
	}
	for _, line := range strings.Split(string(deps), "\n") {
		if len(line) == 0 || line[0] != ' ' {
			continue
		}
		if path := strings.TrimSpace(line); path != "" {
			files[resolveBuildPath(path, buildDir)] = true
		}
	}
	for _, line := range strings.Split(string(inputs), "\n") {
		if path := strings.TrimSpace(line); path != "" {
			files[resolveBuildPath(path, buildDir)] = true
		}
	}
	return discoveredPaths{Files: mapKeys(files)}, nil
}

// ninjaQueryInputs returns the explicit and implicit inputs from "-t query"
// output. Order-only inputs and validations do not make the target stale.
func ninjaQueryInputs(output []byte) []string {
	var inputs []string
	section := ""
	for _, line := range strings.Split(string(output), "\n") {
		trimmed := strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(line, "  input:"):
			section = "input"
			continue
		case strings.HasPrefix(line, "  ") && !strings.HasPrefix(line, "    "):
			section = ""
			continue
		case section != "input" || trimmed == "":
			continue
		case strings.HasPrefix(trimmed, "||"), strings.HasPrefix(trimmed, "|@"):
			continue
		}
		inputs = append(inputs, strings.TrimSpace(strings.TrimPrefix(trimmed, "|")))
	}
	return inputs
}

func ninjaToolOutput(ninja, buildDir string, env map[string]string, args ...string) ([]byte, error) {
	commandArgs := []string{"-C", buildDir, "-t"}
	commandArgs = append(commandArgs, args...)
	cmd := exec.Command(ninja, commandArgs...)
	cmd.Env = environmentList(env)
	cmd.Stdin = nil
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	output, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("%s: %w: %s", strings.Join(cmd.Args, " "), err, strings.TrimSpace(stderr.String()))
	}
	return output, nil
}

func ninjaDefaultTargets(path string) ([]string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", path, err)
	}
	logical := joinNinjaContinuations(string(data))
	var targets []string
	for _, line := range strings.Split(logical, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "default ") {
			targets = append(targets, strings.Fields(strings.TrimPrefix(line, "default "))...)
		}
	}
	return targets, nil
}

func resolveBuildPath(path, buildDir string) string {
	path = strings.ReplaceAll(path, "$$", "$")
	if filepath.IsAbs(path) {
		return filepath.Clean(path)
	}
	return filepath.Clean(filepath.Join(buildDir, filepath.FromSlash(path)))
}

func discoverDepfiles(workspace *config.Workspace, pattern string) (discoveredPaths, error) {
	matches, err := globDoubleStar(workspace.Root, pattern)
	if err != nil {
		return discoveredPaths{}, err
	}
	if len(matches) == 0 {
		return discoveredPaths{}, fmt.Errorf("glob %q matched no depfiles", pattern)
	}
	files := make(map[string]bool)
	for _, depfile := range matches {
		data, readErr := os.ReadFile(depfile)
		if readErr != nil {
			return discoveredPaths{}, readErr
		}
		for _, dependency := range parseDepfile(data) {
			if filepath.IsAbs(dependency) {
				files[filepath.Clean(dependency)] = true
			} else {
				fromBuild := filepath.Clean(filepath.Join(filepath.Dir(depfile), filepath.FromSlash(dependency)))
				fromWorkspace := filepath.Clean(filepath.Join(workspace.Root, filepath.FromSlash(dependency)))
				if _, statErr := os.Lstat(fromBuild); statErr == nil {
					files[fromBuild] = true
				} else if _, statErr := os.Lstat(fromWorkspace); statErr == nil {
					files[fromWorkspace] = true
				} else {
					files[fromBuild] = true
				}
			}
		}
	}
	return discoveredPaths{Files: mapKeys(files)}, nil
}

func parseDepfile(data []byte) []string {
	text := joinMakeContinuations(string(data))
	var dependencies []string
	for _, line := range strings.Split(text, "\n") {
		colon := unescapedColon(line)
		if colon < 0 {
			continue
		}
		dependencies = append(dependencies, splitMakeWords(line[colon+1:])...)
	}
	return dependencies
}

func unescapedColon(value string) int {
	escaped := false
	for index, char := range value {
		if escaped {
			escaped = false
			continue
		}
		if char == '\\' {
			escaped = true
			continue
		}
		if char == ':' {
			return index
		}
	}
	return -1
}

func joinMakeContinuations(text string) string {
	text = strings.ReplaceAll(text, "\\\r\n", " ")
	return strings.ReplaceAll(text, "\\\n", " ")
}

func joinNinjaContinuations(text string) string {
	text = strings.ReplaceAll(text, "$\r\n", "")
	return strings.ReplaceAll(text, "$\n", "")
}

func splitMakeWords(value string) []string {
	var words []string
	var word strings.Builder
	escaped := false
	flush := func() {
		if word.Len() != 0 {
			words = append(words, word.String())
			word.Reset()
		}
	}
	for _, char := range value {
		if escaped {
			word.WriteRune(char)
			escaped = false
			continue
		}
		if char == '\\' {
			escaped = true
			continue
		}
		if char == ' ' || char == '\t' || char == '\r' || char == '\n' {
			flush()
			continue
		}
		word.WriteRune(char)
	}
	if escaped {
		word.WriteByte('\\')
	}
	flush()
	return words
}

func globDoubleStar(root, pattern string) ([]string, error) {
	patternPath := pattern
	if !filepath.IsAbs(patternPath) {
		patternPath = filepath.Join(root, filepath.FromSlash(patternPath))
	}
	patternPath = filepath.Clean(patternPath)
	prefix := patternPath
	for containsGlobMeta(filepath.Base(prefix)) {
		prefix = filepath.Dir(prefix)
	}
	for containsGlobMeta(prefix) {
		prefix = filepath.Dir(prefix)
	}
	var matches []string
	err := filepath.WalkDir(prefix, func(path string, entry os.DirEntry, walkErr error) error {
		if errors.Is(walkErr, os.ErrNotExist) {
			return nil
		}
		if walkErr != nil {
			return walkErr
		}
		matched, matchErr := matchPathPattern(filepath.ToSlash(patternPath), filepath.ToSlash(path))
		if matchErr != nil {
			return matchErr
		}
		if matched && entry.Type().IsRegular() {
			matches = append(matches, filepath.Clean(path))
		}
		return nil
	})
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	sort.Strings(matches)
	return matches, nil
}

func containsGlobMeta(value string) bool {
	return strings.ContainsAny(value, "*?[")
}

func matchPathPattern(pattern, value string) (bool, error) {
	var expression strings.Builder
	expression.WriteByte('^')
	for index := 0; index < len(pattern); {
		switch pattern[index] {
		case '*':
			if index+1 < len(pattern) && pattern[index+1] == '*' {
				index += 2
				if index < len(pattern) && pattern[index] == '/' {
					expression.WriteString(`(?:.*/)?`)
					index++
				} else {
					expression.WriteString(`.*`)
				}
			} else {
				expression.WriteString(`[^/]*`)
				index++
			}
		case '?':
			expression.WriteString(`[^/]`)
			index++
		case '[':
			end := strings.IndexByte(pattern[index+1:], ']')
			if end < 0 {
				return false, fmt.Errorf("invalid glob pattern %q", pattern)
			}
			end += index + 1
			expression.WriteString(pattern[index : end+1])
			index = end + 1
		default:
			expression.WriteString(regexp.QuoteMeta(pattern[index : index+1]))
			index++
		}
	}
	expression.WriteByte('$')
	compiled, err := regexp.Compile(expression.String())
	if err != nil {
		return false, err
	}
	return compiled.MatchString(value), nil
}

func excludedInput(input config.RecipeInput, relative string) (bool, error) {
	relative = filepath.ToSlash(relative)
	for _, pattern := range input.Exclude {
		candidate := relative
		if !strings.Contains(pattern, "/") {
			candidate = filepath.Base(relative)
		}
		matched, err := matchPathPattern(filepath.ToSlash(pattern), candidate)
		if err != nil {
			return false, err
		}
		if matched {
			return true, nil
		}
	}
	return false, nil
}

func mapKeys(values map[string]bool) []string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

func pathWithinAbs(path, parent string) bool {
	path = filepath.Clean(path)
	parent = filepath.Clean(parent)
	rel, err := filepath.Rel(parent, path)
	return err == nil && rel != ".." && !filepath.IsAbs(rel) && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

func containsString(values []string, wanted string) bool {
	for _, value := range values {
		if value == wanted {
			return true
		}
	}
	return false
}
