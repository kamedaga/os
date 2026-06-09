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
	Root       string
	ConfigPath string
	Schema     int               `yaml:"schema"`
	Name       string            `yaml:"name"`
	Artifacts  string            `yaml:"artifacts"`
	State      string            `yaml:"state"`
	Kernel     Kernel            `yaml:"kernel"`
	Disk       Disk              `yaml:"disk"`
	Manifests  map[string]string `yaml:"manifests"`
	RootfsDirs []string          `yaml:"rootfsDirs"`
	Skip       Skip              `yaml:"skip"`
	AppsMap    map[string]App    `yaml:"apps"`
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

type App struct {
	ID       string
	Role     string         `yaml:"role"`
	File     any            `yaml:"file"`
	Zig      map[string]any `yaml:"zig"`
	Out      string         `yaml:"out"`
	Target   string         `yaml:"target"`
	Optimize string         `yaml:"optimize"`
	Strip    bool           `yaml:"strip"`
	Bootfs   any            `yaml:"bootfs"`
	Rootfs   any            `yaml:"rootfs"`
	Startup  map[string]any `yaml:"startup"`
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
	for id, app := range w.AppsMap {
		app.ID = id
		if app.Role == "" {
			app.Role = "asset"
		}
		if app.Optimize == "" {
			app.Optimize = "release"
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

func (a App) Kind() string {
	if a.Zig != nil {
		return "zig"
	}
	if a.File != nil {
		return "file"
	}
	return "none"
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
