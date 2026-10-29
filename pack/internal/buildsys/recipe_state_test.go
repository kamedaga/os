package buildsys

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func recipeWorkspace(t *testing.T, script string) *config.Workspace {
	t.Helper()
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		State:     ".state",
		RecipesMap: map[string]config.Recipe{
			"shared": {
				ID: "shared", Run: []string{"sh", "build.sh"}, Tools: []string{"sh"},
				Inputs: []config.RecipeInput{{Path: "build.sh"}, {Path: "input.txt"}}, Outputs: []string{"a.out", "b.out"},
			},
		},
		AppsMap: map[string]config.App{
			"a": {ID: "a", File: map[string]any{"recipe": "shared", "path": "a.out"}},
			"b": {ID: "b", File: map[string]any{"recipe": "shared", "path": "b.out"}},
		},
	}
	writeCacheFixture(t, workspace, "input.txt", "one")
	writeCacheFixture(t, workspace, "build.sh", script)
	return workspace
}

func TestRecipeStateSecondRunAndGroupInputChange(t *testing.T) {
	workspace := recipeWorkspace(t, `printf x >> runs
cp input.txt a.out
cp input.txt b.out
`)
	first, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if first.RebuiltSources != 2 {
		t.Fatalf("first result = %+v", first)
	}
	second, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if second.RebuiltSources != 0 {
		t.Fatalf("second result = %+v", second)
	}
	writeCacheFixture(t, workspace, "input.txt", "two")
	third, err := BuildUserland(workspace, UserlandOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if third.RebuiltSources != 2 {
		t.Fatalf("third result = %+v", third)
	}
	runs, err := os.ReadFile(workspace.Path("runs"))
	if err != nil {
		t.Fatal(err)
	}
	if string(runs) != "xx" {
		t.Fatalf("recipe ran %d times, want 2", len(runs))
	}
}

func TestRecipeOutputTamperingReruns(t *testing.T) {
	workspace := recipeWorkspace(t, "printf x >> runs\nprintf good > a.out\nprintf good > b.out\n")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	writeCacheFixture(t, workspace, "a.out", "evil")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	runs, _ := os.ReadFile(workspace.Path("runs"))
	if string(runs) != "xx" {
		t.Fatalf("recipe executions = %q", runs)
	}
	contents, _ := os.ReadFile(workspace.Path("a.out"))
	if string(contents) != "good" {
		t.Fatalf("tampered output was not restored: %q", contents)
	}
}

func TestRecipeMissingOutputAfterRunIsError(t *testing.T) {
	workspace := recipeWorkspace(t, "printf only > a.out\n")
	_, err := BuildUserland(workspace, UserlandOptions{})
	if err == nil || !strings.Contains(err.Error(), "b.out") || !strings.Contains(err.Error(), "did not create declared outputs") {
		t.Fatalf("error = %v", err)
	}
}

func TestRecipeToolDigestChangeReruns(t *testing.T) {
	workspace := recipeWorkspace(t, "printf x >> runs\nprintf good > a.out\nprintf good > b.out\n")
	bin := t.TempDir()
	tool := filepath.Join(bin, "fake-tool")
	if err := os.WriteFile(tool, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	recipe := workspace.RecipesMap["shared"]
	recipe.Tools = append(recipe.Tools, "fake-tool")
	workspace.RecipesMap["shared"] = recipe
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(tool, []byte("#!/bin/sh\nexit 1\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	runs, _ := os.ReadFile(workspace.Path("runs"))
	if string(runs) != "xx" {
		t.Fatalf("recipe executions = %q", runs)
	}
}

func TestAdoptAndStatus(t *testing.T) {
	workspace := recipeWorkspace(t, ":\n")
	writeCacheFixture(t, workspace, "a.out", "a")
	writeCacheFixture(t, workspace, "b.out", "b")
	adopted, err := AdoptRecipes(workspace, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(adopted) != 1 || adopted[0] != "shared" {
		t.Fatalf("adopted = %#v", adopted)
	}
	statuses, err := RecipeStatuses(workspace)
	if err != nil {
		t.Fatal(err)
	}
	if len(statuses) != 1 || !statuses[0].UpToDate {
		t.Fatalf("statuses = %#v", statuses)
	}
	writeCacheFixture(t, workspace, "input.txt", "changed")
	statuses, err = RecipeStatuses(workspace)
	if err != nil {
		t.Fatal(err)
	}
	if statuses[0].UpToDate || !strings.Contains(statuses[0].Reason, "input changed") {
		t.Fatalf("statuses after mutation = %#v", statuses)
	}
}

func TestAdoptNamedRecipeRequiresOutputs(t *testing.T) {
	workspace := recipeWorkspace(t, ":\n")
	_, err := AdoptRecipes(workspace, []string{"shared"})
	if err == nil || !strings.Contains(err.Error(), "missing outputs") {
		t.Fatalf("error = %v", err)
	}
}

func TestAdoptRecipeRequiresDiscoveryDirectory(t *testing.T) {
	workspace := recipeWorkspace(t, ":\n")
	writeCacheFixture(t, workspace, "a.out", "a")
	writeCacheFixture(t, workspace, "b.out", "b")
	recipe := workspace.RecipesMap["shared"]
	recipe.Discover = []config.Discovery{{Kind: "ninja", Dir: ".artifacts/missing-build"}}
	workspace.RecipesMap["shared"] = recipe
	_, err := AdoptRecipes(workspace, []string{"shared"})
	if err == nil || !strings.Contains(err.Error(), "missing-build") {
		t.Fatalf("error = %v", err)
	}
}

func TestStatusWithoutStateDoesNotSnapshotOutputs(t *testing.T) {
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, State: ".state"}
	recipe := config.Recipe{ID: "unstated", Run: []string{"true"}, Outputs: []string{"output"}}
	writeCacheFixture(t, workspace, "output/large", strings.Repeat("x", 1024))
	hasher := newHasher(workspace.Path("hashcache"))
	hasher.fileHashReader = func(path string) ([]byte, error) {
		t.Fatalf("status hashed %s before finding that state is absent", path)
		return nil, nil
	}
	status, _, err := recipeStatusOnlyInEnvironment(workspace, recipe, hasher, map[string]string{"PATH": os.Getenv("PATH")})
	if err != nil {
		t.Fatal(err)
	}
	if status.UpToDate || status.Reason != "no state" {
		t.Fatalf("status = %#v", status)
	}
}

func TestAdoptAllSkipsRecipesWithMissingOutputsBeforeTools(t *testing.T) {
	workspace := recipeWorkspace(t, ":\n")
	recipe := workspace.RecipesMap["shared"]
	recipe.Tools = []string{"tool-that-does-not-exist"}
	workspace.RecipesMap["shared"] = recipe
	adopted, err := AdoptRecipes(workspace, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(adopted) != 0 {
		t.Fatalf("adopted = %#v", adopted)
	}
}
