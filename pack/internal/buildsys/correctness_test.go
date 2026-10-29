package buildsys

import (
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"capabilityos/pack/internal/config"
)

func TestDirectoryArtifactTracksPublishedTree(t *testing.T) {
	root := t.TempDir()
	fixture := filepath.Join(root, "fixture")
	if err := os.MkdirAll(fixture, 0o755); err != nil {
		t.Fatal(err)
	}
	inner := filepath.Join(fixture, "payload")
	if err := os.WriteFile(inner, []byte("first"), 0o644); err != nil {
		t.Fatal(err)
	}
	beforeDir, err := os.Stat(fixture)
	if err != nil {
		t.Fatal(err)
	}
	writeCacheFixture(t, &config.Workspace{Root: root}, "rebuild.sh", "#!/bin/sh\nprintf second > fixture/payload\n")
	workspace := &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		State:     ".state",
		RecipesMap: map[string]config.Recipe{
			"tree": {ID: "tree", Run: []string{"sh", "rebuild.sh"}, Tools: []string{"sh"}, Inputs: []config.RecipeInput{{Path: "rebuild.sh"}}, Outputs: []string{"fixture"}},
		},
		AppsMap: map[string]config.App{
			"tree": {
				ID: "tree",
				File: map[string]any{
					"path":   "fixture",
					"recipe": "tree",
				},
			},
		},
	}

	first, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if first.DirectoryArtifactsChanged != 1 || len(first.ChangedArtifacts) != 1 || first.RebuiltSources != 1 {
		t.Fatalf("first build result = %+v", first)
	}
	afterDir, err := os.Stat(fixture)
	if err != nil {
		t.Fatal(err)
	}
	if !afterDir.ModTime().Equal(beforeDir.ModTime()) {
		t.Fatalf("fake rebuild changed top directory mtime: before=%v after=%v", beforeDir.ModTime(), afterDir.ModTime())
	}

	second, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if second.DirectoryArtifactsChanged != 0 || len(second.ChangedArtifacts) != 0 || second.RebuiltSources != 0 {
		t.Fatalf("unchanged second build result = %+v", second)
	}

	if err := os.WriteFile(inner, []byte("third"), 0o644); err != nil {
		t.Fatal(err)
	}
	changedTime := time.Unix(1_600_000_000, 123)
	if err := os.Chtimes(inner, changedTime, changedTime); err != nil {
		t.Fatal(err)
	}
	third, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if third.DirectoryArtifactsChanged != 1 || len(third.ChangedArtifacts) != 1 || third.RebuiltSources != 1 {
		t.Fatalf("out-of-band build result = %+v", third)
	}
}

func TestPythonCacheDoesNotChangeFingerprint(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}
	recipe := config.Recipe{Inputs: []config.RecipeInput{{Path: "src"}}}
	writeCacheFixture(t, workspace, "src/main.py", "print('source')")
	writeCacheFixture(t, workspace, "src/module.pyc", "old bytecode")
	writeCacheFixture(t, workspace, "src/__pycache__/module.cpython.pyc", "old cache")
	before := cacheFixtureInputs(t, workspace, recipe)
	writeCacheFixture(t, workspace, "src/module.pyc", "new bytecode")
	writeCacheFixture(t, workspace, "src/__pycache__/module.cpython.pyc", "new cache")
	if after := cacheFixtureInputs(t, workspace, recipe); !reflect.DeepEqual(before, after) {
		t.Fatal("Python bytecode cache changed the source fingerprint")
	}
}

func TestRecipeInputMutationWarnsAndStoresPreBuildFingerprint(t *testing.T) {
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		State:     ".state",
		RecipesMap: map[string]config.Recipe{
			"mutator": {ID: "mutator", Run: []string{"sh", "rebuild.sh"}, Tools: []string{"sh"}, Inputs: []config.RecipeInput{{Path: "input.txt"}, {Path: "rebuild.sh"}}, Outputs: []string{"output.bin"}},
		},
		AppsMap: map[string]config.App{
			"mutator": {
				ID: "mutator",
				File: map[string]any{
					"path":   "output.bin",
					"recipe": "mutator",
				},
			},
		},
	}
	writeCacheFixture(t, workspace, "input.txt", "before")
	writeCacheFixture(t, workspace, "rebuild.sh", "#!/bin/sh\nprintf after > input.txt\nprintf output > output.bin\n")
	preBuild, err := snapshotRecipeInputs(workspace, workspace.RecipesMap["mutator"], newHasher(workspace.Path(".pre-hashcache")))
	if err != nil {
		t.Fatal(err)
	}

	readPipe, writePipe, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	originalStderr := os.Stderr
	os.Stderr = writePipe
	result, buildErr := BuildUserland(workspace, UserlandOptions{})
	_ = writePipe.Close()
	os.Stderr = originalStderr
	warning, readErr := io.ReadAll(readPipe)
	_ = readPipe.Close()
	if buildErr != nil {
		t.Fatal(buildErr)
	}
	if readErr != nil {
		t.Fatal(readErr)
	}
	if result.RebuiltSources != 1 {
		t.Fatalf("build result = %+v", result)
	}
	if !strings.Contains(string(warning), "warning: inputs changed while rebuilding recipe mutator") || !strings.Contains(string(warning), "input.txt") {
		t.Fatalf("warning = %q", warning)
	}
	stored, exists, err := readRecipeState(workspace, "mutator")
	if err != nil {
		t.Fatal(err)
	}
	if !exists || !reflect.DeepEqual(stored.DeclaredInputs, preBuild) {
		t.Fatalf("stored inputs = %#v, want pre-build %#v", stored.DeclaredInputs, preBuild)
	}
}

func TestFileRecipeOnlyCountsRebuiltWhenOutputContentChanges(t *testing.T) {
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		State:     ".state",
		RecipesMap: map[string]config.Recipe{
			"noop": {ID: "noop", Run: []string{"sh", "rebuild.sh"}, Tools: []string{"sh"}, Inputs: []config.RecipeInput{{Path: "rebuild.sh"}}, Outputs: []string{"output.bin"}},
		},
		AppsMap: map[string]config.App{
			"noop": {
				ID: "noop",
				File: map[string]any{
					"path":   "output.bin",
					"recipe": "noop",
				},
			},
		},
	}
	writeCacheFixture(t, workspace, "output.bin", "unchanged")
	writeCacheFixture(t, workspace, "rebuild.sh", "#!/bin/sh\n:\n")
	result, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if result.RebuiltSources != 0 || len(result.RebuiltApps) != 0 {
		t.Fatalf("no-op recipe counted as rebuilt: %+v", result)
	}
	if result.CopiedArtifacts != 1 || len(result.ChangedArtifacts) != 1 {
		t.Fatalf("initial artifact publication was not recorded: %+v", result)
	}
}

func TestSharedRecipePublishesAllChangedOutputs(t *testing.T) {
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		State:     ".state",
		RecipesMap: map[string]config.Recipe{
			"shared": {ID: "shared", Run: []string{"sh", "rebuild.sh"}, Tools: []string{"sh"}, Inputs: []config.RecipeInput{{Path: "rebuild.sh"}}, Outputs: []string{"a.out", "b.out"}},
		},
		AppsMap: map[string]config.App{
			"a": {ID: "a", File: map[string]any{"path": "a.out", "recipe": "shared"}},
			"b": {ID: "b", File: map[string]any{"path": "b.out", "recipe": "shared"}},
		},
	}
	writeCacheFixture(t, workspace, "a.out", "old a")
	writeCacheFixture(t, workspace, "b.out", "old b")
	writeCacheFixture(t, workspace, "rebuild.sh", "#!/bin/sh\nprintf 'new a' > a.out\nprintf 'new b' > b.out\n")
	for _, id := range []string{"a", "b"} {
		app, _ := workspace.App(id)
		artifact := workspace.ArtifactPath(app)
		if err := os.MkdirAll(filepath.Dir(artifact), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(artifact, []byte("old "+id), 0o755); err != nil {
			t.Fatal(err)
		}
	}

	result, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if result.RebuiltSources != 2 || result.CopiedArtifacts != 2 || len(result.ChangedArtifacts) != 2 {
		t.Fatalf("shared recipe result = %+v", result)
	}
	if got := strings.Join(result.RebuiltApps, ","); got != "a,b" {
		t.Fatalf("rebuilt apps = %q, want a,b", got)
	}
}

func TestSuccessfulRecipeFingerprintSurvivesLaterRecipeFailure(t *testing.T) {
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		State:     ".state",
		RecipesMap: map[string]config.Recipe{
			"first":  {ID: "first", Run: []string{"sh", "first.sh"}, Tools: []string{"sh"}, Inputs: []config.RecipeInput{{Path: "first.sh"}}, Outputs: []string{"first.out"}},
			"second": {ID: "second", Run: []string{"sh", "second.sh"}, Tools: []string{"sh"}, Inputs: []config.RecipeInput{{Path: "second.sh"}}, Outputs: []string{"second.out"}},
		},
		AppsMap: map[string]config.App{
			"first": {
				ID: "first",
				File: map[string]any{
					"path":   "first.out",
					"recipe": "first",
				},
			},
			"second": {
				ID: "second",
				File: map[string]any{
					"path":   "second.out",
					"recipe": "second",
				},
			},
		},
	}
	writeCacheFixture(t, workspace, "first.sh", "printf run >> first.count\nprintf first > first.out\n")
	writeCacheFixture(t, workspace, "second.sh", "exit 1\n")

	if _, err := BuildUserland(workspace, UserlandOptions{}); err == nil {
		t.Fatal("build unexpectedly succeeded with failing second recipe")
	}
	if _, err := os.Stat(recipeStatePath(workspace, "first")); err != nil {
		t.Fatalf("first recipe fingerprint was not persisted: %v", err)
	}
	writeCacheFixture(t, workspace, "second.sh", "printf second > second.out\n")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	runs, err := os.ReadFile(workspace.Path("first.count"))
	if err != nil {
		t.Fatal(err)
	}
	if string(runs) != "run" {
		t.Fatalf("first recipe executions = %q, want one run", runs)
	}
}
