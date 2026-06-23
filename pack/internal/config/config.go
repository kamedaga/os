package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"

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
	Manifests       Manifests         `yaml:"manifests"`
	StartupManifest StartupManifest   `yaml:"startupManifest"`
	RootfsDirs      []string          `yaml:"rootfsDirs"`
	Skip            Skip              `yaml:"skip"`
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
		w.Kernel.Step = "efi"
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
	for id, app := range w.AppsMap {
		app.ID = id
		if app.Role == "" {
			app.Role = "asset"
		}
		w.AppsMap[id] = app
	}
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
	Path    string
	Rebuild []string
	Input   []string
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
		if value, ok := typed["path"]; ok {
			src.Path = fmt.Sprint(value)
		}
		if value, ok := typed["rebuild"]; ok {
			src.Rebuild = stringSlice(value)
		}
		if value, ok := typed["input"]; ok {
			src.Input = stringSlice(value)
		}
		if src.Path == "" {
			return FileSource{}, fmt.Errorf("app %s file source is missing path", a.ID)
		}
		return src, nil
	default:
		return FileSource{}, fmt.Errorf("app %s has unsupported file source", a.ID)
	}
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
