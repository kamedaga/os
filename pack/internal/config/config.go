package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gopkg.in/yaml.v3"
)

const PackFile = "pack/pack.yaml"

type Workspace struct {
	Root            string
	ConfigPath      string
	Schema          int               `yaml:"schema"`
	Name            string            `yaml:"name"`
	Artifacts       string            `yaml:"artifacts"`
	State           string            `yaml:"state"`
	Kernel          Kernel            `yaml:"kernel"`
	Disk            Disk              `yaml:"disk"`
	Storage         Storage           `yaml:"storage"`
	Manifests       Manifests         `yaml:"manifests"`
	StartupManifest StartupManifest   `yaml:"startupManifest"`
	RootfsDirs      []string          `yaml:"rootfsDirs"`
	Skip            Skip              `yaml:"skip"`
	RecipesMap      map[string]Recipe `yaml:"recipes"`
	AppsMap         map[string]App    `yaml:"apps"`
	Raw             map[string]string `yaml:"-"`
}

type Kernel struct {
	Dir  string `yaml:"dir"`
	Step string `yaml:"step"`
}

type Disk struct {
	Image      string               `yaml:"image"`
	SizeMiB    int                  `yaml:"sizeMiB"`
	Recreate   string               `yaml:"recreate"`
	Partitions map[string]Partition `yaml:"partitions"`
}

// Storage budgets are explicit owner policy, not inode/path/cache limits.
type Storage struct {
	SandboxRAMMiB uint64 `yaml:"sandboxRAMMiB"`
	QueueSize     uint64 `yaml:"queueSize"`
	QueueArenaMiB uint64 `yaml:"queueArenaMiB"`
}

type Partition struct {
	Index   int    `yaml:"index"`
	Format  string `yaml:"format"`
	SizeMiB int    `yaml:"sizeMiB"`
	Grow    bool   `yaml:"grow"`
}

type Skip struct {
	Apps                    []string `yaml:"apps"`
	Kinds                   []string `yaml:"kinds"`
	IncludeSkippedArtifacts bool     `yaml:"includeSkippedArtifacts"`
}

type Manifests struct {
	Dir     string `yaml:"dir"`
	Bootfs  string `yaml:"bootfs"`
	Rootfs  string `yaml:"rootfs"`
	Startup string `yaml:"startup"`
}

type StartupManifest struct {
	Path    string   `yaml:"path"`
	Include []string `yaml:"include"`
}

type App struct {
	ID      string
	Role    string         `yaml:"role"`
	File    any            `yaml:"file"`
	Out     string         `yaml:"out"`
	Bootfs  any            `yaml:"bootfs"`
	Rootfs  any            `yaml:"rootfs"`
	Startup map[string]any `yaml:"startup"`
}

type Recipe struct {
	ID       string
	Run      []string      `yaml:"run"`
	Tools    []string      `yaml:"tools"`
	Needs    []string      `yaml:"needs"`
	Inputs   []RecipeInput `yaml:"inputs"`
	Discover []Discovery   `yaml:"discover"`
	Outputs  []string      `yaml:"outputs"`
}

type RecipeInput struct {
	Path    string   `yaml:"path"`
	Exclude []string `yaml:"exclude,omitempty"`
}

func (input *RecipeInput) UnmarshalYAML(node *yaml.Node) error {
	switch node.Kind {
	case yaml.ScalarNode:
		if node.Tag != "!!str" {
			return fmt.Errorf("recipe input must be a path string or mapping")
		}
		input.Path = node.Value
		return nil
	case yaml.MappingNode:
		for index := 0; index < len(node.Content); index += 2 {
			key := node.Content[index].Value
			if key != "path" && key != "exclude" {
				return fmt.Errorf("unknown recipe input key %q", key)
			}
		}
		type plain RecipeInput
		return node.Decode((*plain)(input))
	default:
		return fmt.Errorf("recipe input must be a path string or mapping")
	}
}

type Discovery struct {
	Kind string `yaml:"kind" json:"kind"`
	Dir  string `yaml:"dir,omitempty" json:"dir,omitempty"`
	Glob string `yaml:"glob,omitempty" json:"glob,omitempty"`
}

func (discovery *Discovery) UnmarshalYAML(node *yaml.Node) error {
	if node.Kind != yaml.MappingNode {
		return fmt.Errorf("recipe discovery must be a mapping")
	}
	for index := 0; index < len(node.Content); index += 2 {
		key := node.Content[index].Value
		if key != "kind" && key != "dir" && key != "glob" {
			return fmt.Errorf("unknown recipe discovery key %q", key)
		}
	}
	type plain Discovery
	return node.Decode((*plain)(discovery))
}

func (r *Recipe) UnmarshalYAML(node *yaml.Node) error {
	if node.Kind != yaml.MappingNode {
		return fmt.Errorf("recipe must be a mapping")
	}
	known := map[string]bool{"run": true, "env": true, "tools": true, "needs": true, "inputs": true, "discover": true, "outputs": true}
	for index := 0; index < len(node.Content); index += 2 {
		key := node.Content[index].Value
		if !known[key] {
			return fmt.Errorf("unknown recipe key %q", key)
		}
	}
	type plain Recipe
	return node.Decode((*plain)(r))
}

func FindRoot(start string) (string, error) {
	current, err := filepath.Abs(start)
	if err != nil {
		return "", err
	}
	for {
		if _, err := os.Stat(filepath.Join(current, PackFile)); err == nil {
			return current, nil
		}
		if filepath.Base(current) == "pack" {
			if _, err := os.Stat(filepath.Join(current, "pack.yaml")); err == nil {
				return filepath.Dir(current), nil
			}
		}
		parent := filepath.Dir(current)
		if parent == current {
			return "", errors.New("could not find " + PackFile)
		}
		current = parent
	}
}

func Load(root string) (*Workspace, error) {
	path := filepath.Join(root, PackFile)
	bytes, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var workspace Workspace
	if err := yaml.Unmarshal(bytes, &workspace); err != nil {
		return nil, err
	}
	workspace.Root = root
	workspace.ConfigPath = path
	if workspace.Schema != 1 {
		return nil, fmt.Errorf("%s: unsupported schema %d", path, workspace.Schema)
	}
	workspace.defaults()
	if err := workspace.validate(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return &workspace, nil
}

func (w *Workspace) defaults() {
	if w.Name == "" {
		w.Name = "CapabilityOS"
	}
	if w.Artifacts == "" {
		w.Artifacts = ".artifacts"
	}
	if w.State == "" {
		w.State = ".artifacts/pack"
	}
	if w.Kernel.Dir == "" {
		w.Kernel.Dir = "kernel"
	}
	if w.Kernel.Step == "" {
		w.Kernel.Step = "limine"
	}
	if w.Disk.SizeMiB == 0 {
		w.Disk.SizeMiB = 512
	}
	if w.Manifests.Dir == "" {
		w.Manifests.Dir = ".artifacts/manifests"
	}
	if w.Manifests.Bootfs == "" {
		w.Manifests.Bootfs = filepath.Join(w.Manifests.Dir, "bootfs.generated.txt")
	}
	if w.Manifests.Rootfs == "" {
		w.Manifests.Rootfs = filepath.Join(w.Manifests.Dir, "rootfs.generated.txt")
	}
	if w.Manifests.Startup == "" {
		w.Manifests.Startup = filepath.Join(w.Manifests.Dir, "startup.generated.txt")
	}
	if w.StartupManifest.Path == "" {
		w.StartupManifest.Path = "/sys/startup_manifest.txt"
	}
	for id, recipe := range w.RecipesMap {
		recipe.ID = id
		w.RecipesMap[id] = recipe
	}
	for id, app := range w.AppsMap {
		app.ID = id
		if app.Role == "" {
			app.Role = "asset"
		}
		w.AppsMap[id] = app
	}
}

func (w *Workspace) validate() error {
	ids := make([]string, 0, len(w.RecipesMap))
	for id := range w.RecipesMap {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		recipe := w.RecipesMap[id]
		if len(recipe.Run) == 0 || recipe.Run[0] == "" {
			return fmt.Errorf("recipe %s is missing run", id)
		}
		if len(recipe.Outputs) == 0 {
			return fmt.Errorf("recipe %s is missing outputs", id)
		}
		for _, input := range recipe.Inputs {
			if input.Path == "" {
				return fmt.Errorf("recipe %s has an empty input path", id)
			}
			if len(input.Exclude) != 0 && len(recipe.Discover) == 0 {
				return fmt.Errorf("recipe %s input %s uses exclude without discover", id, input.Path)
			}
			for _, pattern := range input.Exclude {
				if pattern == "" {
					return fmt.Errorf("recipe %s input %s has an empty exclude pattern", id, input.Path)
				}
			}
		}
		for _, discovery := range recipe.Discover {
			switch discovery.Kind {
			case "kbuild", "kbuild-dir-file", "ninja":
				if discovery.Dir == "" || discovery.Glob != "" {
					return fmt.Errorf("recipe %s %s discovery requires dir and does not accept glob", id, discovery.Kind)
				}
			case "depfile":
				if discovery.Glob == "" || discovery.Dir != "" {
					return fmt.Errorf("recipe %s depfile discovery requires glob and does not accept dir", id)
				}
			default:
				return fmt.Errorf("recipe %s has invalid discovery kind %q", id, discovery.Kind)
			}
		}
		for _, need := range recipe.Needs {
			if _, ok := w.RecipesMap[need]; !ok {
				return fmt.Errorf("recipe %s needs unknown recipe %s", id, need)
			}
		}
	}
	for leftIndex, leftID := range ids {
		left := w.RecipesMap[leftID]
		for _, leftOutput := range left.Outputs {
			if leftOutput == "" {
				return fmt.Errorf("recipe %s has an empty output", leftID)
			}
			for _, rightID := range ids[leftIndex+1:] {
				right := w.RecipesMap[rightID]
				for _, rightOutput := range right.Outputs {
					if pathsOverlap(leftOutput, rightOutput) {
						return fmt.Errorf("recipes %s and %s have overlapping outputs %q and %q", leftID, rightID, leftOutput, rightOutput)
					}
				}
			}
		}
	}
	// An input owned by another recipe is an implicit dependency. Keep the
	// explicit order, then add derived edges in stable recipe/output order.
	for _, id := range ids {
		recipe := w.RecipesMap[id]
		seen := make(map[string]bool, len(recipe.Needs))
		for _, need := range recipe.Needs {
			seen[need] = true
		}
		for _, ownerID := range ids {
			if ownerID == id || seen[ownerID] {
				continue
			}
			owner := w.RecipesMap[ownerID]
			derived := false
			for _, input := range recipe.Inputs {
				for _, output := range owner.Outputs {
					if pathWithin(input.Path, output) {
						derived = true
						break
					}
				}
				if derived {
					break
				}
			}
			if derived {
				recipe.Needs = append(recipe.Needs, ownerID)
				seen[ownerID] = true
			}
		}
		w.RecipesMap[id] = recipe
	}
	for _, app := range w.Apps() {
		source, err := app.FileSource()
		if app.File == nil {
			continue
		}
		if err != nil {
			return err
		}
		if source.Recipe == "" {
			continue
		}
		recipe, ok := w.RecipesMap[source.Recipe]
		if !ok {
			return fmt.Errorf("app %s references unknown recipe %s", app.ID, source.Recipe)
		}
		owned := false
		for _, output := range recipe.Outputs {
			if pathWithin(source.Path, output) {
				owned = true
				break
			}
		}
		if !owned {
			return fmt.Errorf("app %s file path %q is outside recipe %s outputs", app.ID, source.Path, source.Recipe)
		}
	}
	visiting := make(map[string]bool)
	visited := make(map[string]bool)
	var visit func(string, []string) error
	visit = func(id string, stack []string) error {
		if visiting[id] {
			stack = append(stack, id)
			return fmt.Errorf("recipe dependency cycle: %s", strings.Join(stack, " -> "))
		}
		if visited[id] {
			return nil
		}
		visiting[id] = true
		for _, need := range w.RecipesMap[id].Needs {
			if err := visit(need, append(stack, id)); err != nil {
				return err
			}
		}
		visiting[id] = false
		visited[id] = true
		return nil
	}
	for _, id := range ids {
		if err := visit(id, nil); err != nil {
			return err
		}
	}
	return nil
}

func pathWithin(path string, parent string) bool {
	path = filepath.Clean(path)
	parent = filepath.Clean(parent)
	if path == parent {
		return true
	}
	rel, err := filepath.Rel(parent, path)
	return err == nil && rel != ".." && !filepath.IsAbs(rel) && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

func pathsOverlap(left string, right string) bool {
	return pathWithin(left, right) || pathWithin(right, left)
}

func (w *Workspace) Apps() []App {
	apps := make([]App, 0, len(w.AppsMap))
	for _, app := range w.AppsMap {
		apps = append(apps, app)
	}
	sort.Slice(apps, func(i, j int) bool { return apps[i].ID < apps[j].ID })
	return apps
}

func (w *Workspace) App(id string) (App, bool) {
	app, ok := w.AppsMap[id]
	if ok {
		app.ID = id
	}
	return app, ok
}

func (w *Workspace) Skipped(app App) bool {
	for _, id := range w.Skip.Apps {
		if id == app.ID {
			return true
		}
	}
	for _, kind := range w.Skip.Kinds {
		if kind == app.Kind() {
			return true
		}
	}
	return false
}

func (w *Workspace) Path(parts ...string) string {
	items := append([]string{w.Root}, parts...)
	return filepath.Join(items...)
}

func (w *Workspace) Rel(path string) string {
	if filepath.IsAbs(path) {
		if rel, err := filepath.Rel(w.Root, path); err == nil {
			return filepath.ToSlash(rel)
		}
		return filepath.ToSlash(path)
	}
	return filepath.ToSlash(path)
}

func (w *Workspace) ArtifactPath(app App) string {
	return filepath.Join(w.Root, w.Artifacts, "userland", app.ID, app.OutputName())
}

func (a App) OutputName() string {
	if a.Out != "" {
		return a.Out
	}
	return a.ID
}

func (a App) Kind() string {
	if a.File != nil {
		return "file"
	}
	return "none"
}

type FileSource struct {
	Path   string
	Recipe string
}

type Publish struct {
	ID   string
	Path string
}

type Startup struct {
	Publish  string
	Action   string
	Name     string
	Label    string
	Load     string
	After    []string
	Requires []string
	Provides []string
	Ensure   []string
	Block    []string
	Device   []string
	Input    []string
}

func (a App) FileSource() (FileSource, error) {
	switch typed := a.File.(type) {
	case nil:
		return FileSource{}, fmt.Errorf("app %s has no file source", a.ID)
	case string:
		return FileSource{Path: typed}, nil
	case map[string]any:
		src := FileSource{}
		if _, ok := typed["rebuild"]; ok {
			return FileSource{}, fmt.Errorf("app %s uses obsolete file.rebuild; define a top-level recipe and use file: {recipe: ..., path: ...}", a.ID)
		}
		if _, ok := typed["input"]; ok {
			return FileSource{}, fmt.Errorf("app %s uses obsolete file.input; define a top-level recipe and use file: {recipe: ..., path: ...}", a.ID)
		}
		for key := range typed {
			if key != "path" && key != "recipe" {
				return FileSource{}, fmt.Errorf("app %s has unknown file source key %q", a.ID, key)
			}
		}
		if value, ok := typed["path"]; ok {
			src.Path = fmt.Sprint(value)
		}
		if value, ok := typed["recipe"]; ok {
			src.Recipe = fmt.Sprint(value)
		}
		if src.Path == "" {
			return FileSource{}, fmt.Errorf("app %s file source is missing path", a.ID)
		}
		return src, nil
	default:
		return FileSource{}, fmt.Errorf("app %s has unsupported file source", a.ID)
	}
}

func (w *Workspace) Recipes() []Recipe {
	recipes := make([]Recipe, 0, len(w.RecipesMap))
	for _, recipe := range w.RecipesMap {
		recipes = append(recipes, recipe)
	}
	sort.Slice(recipes, func(i, j int) bool { return recipes[i].ID < recipes[j].ID })
	return recipes
}

func (w *Workspace) Recipe(id string) (Recipe, bool) {
	recipe, ok := w.RecipesMap[id]
	if ok {
		recipe.ID = id
	}
	return recipe, ok
}

// RecipeGraph returns each recipe's direct dependencies. The returned slices
// are copies and are sorted to make graph consumers deterministic.
func (w *Workspace) RecipeGraph() map[string][]string {
	graph := make(map[string][]string, len(w.RecipesMap))
	for id, recipe := range w.RecipesMap {
		graph[id] = append([]string(nil), recipe.Needs...)
		sort.Strings(graph[id])
	}
	return graph
}

func (a App) Publishes(fs string) []Publish {
	var out []Publish
	for _, publish := range publishEntries(a.publishValue(fs)) {
		out = append(out, publish)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Path == out[j].Path {
			return out[i].ID < out[j].ID
		}
		return out[i].Path < out[j].Path
	})
	return out
}

func (a App) StartupConfig() (Startup, bool) {
	if a.Startup == nil {
		return Startup{}, false
	}
	startup := Startup{
		Publish:  stringField(a.Startup, "publish"),
		Action:   stringField(a.Startup, "action"),
		Name:     stringField(a.Startup, "name"),
		Label:    stringField(a.Startup, "label"),
		Load:     stringField(a.Startup, "load"),
		After:    stringSlice(a.Startup["after"]),
		Requires: stringSlice(a.Startup["requires"]),
		Provides: stringSlice(a.Startup["provides"]),
		Ensure:   stringSlice(a.Startup["ensure"]),
		Block:    stringSlice(a.Startup["block"]),
		Device:   stringSlice(a.Startup["device"]),
		Input:    stringSlice(a.Startup["input"]),
	}
	return startup, true
}

func (a App) publishValue(fs string) any {
	switch fs {
	case "bootfs":
		return a.Bootfs
	case "rootfs":
		return a.Rootfs
	default:
		return nil
	}
}

func (a App) PublishSummary() []string {
	var out []string
	appendPublish := func(fs string, value any) {
		for _, path := range publishPaths(value) {
			out = append(out, fs+":"+path)
		}
	}
	appendPublish("bootfs", a.Bootfs)
	appendPublish("rootfs", a.Rootfs)
	sort.Strings(out)
	return out
}

func publishPaths(value any) []string {
	switch typed := value.(type) {
	case nil:
		return nil
	case string:
		return []string{typed}
	case []any:
		out := make([]string, 0, len(typed))
		for _, item := range typed {
			out = append(out, fmt.Sprint(item))
		}
		return out
	case map[string]any:
		out := make([]string, 0, len(typed))
		for _, item := range typed {
			out = append(out, fmt.Sprint(item))
		}
		return out
	default:
		return []string{fmt.Sprint(typed)}
	}
}

func publishEntries(value any) []Publish {
	switch typed := value.(type) {
	case nil:
		return nil
	case string:
		return []Publish{{ID: "default", Path: typed}}
	case []any:
		out := make([]Publish, 0, len(typed))
		for i, item := range typed {
			out = append(out, Publish{ID: fmt.Sprintf("%d", i), Path: fmt.Sprint(item)})
		}
		return out
	case map[string]any:
		out := make([]Publish, 0, len(typed))
		for key, item := range typed {
			out = append(out, Publish{ID: key, Path: fmt.Sprint(item)})
		}
		sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
		return out
	default:
		return []Publish{{ID: "default", Path: fmt.Sprint(typed)}}
	}
}

func stringField(values map[string]any, key string) string {
	if value, ok := values[key]; ok {
		return fmt.Sprint(value)
	}
	return ""
}

func stringSlice(value any) []string {
	switch typed := value.(type) {
	case nil:
		return nil
	case string:
		return []string{typed}
	case []string:
		return append([]string{}, typed...)
	case []any:
		out := make([]string, 0, len(typed))
		for _, item := range typed {
			out = append(out, fmt.Sprint(item))
		}
		return out
	default:
		return []string{fmt.Sprint(typed)}
	}
}
