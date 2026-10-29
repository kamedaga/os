package buildsys

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"capabilityos/pack/internal/config"
)

func schedulerWorkspace(t *testing.T, recipes map[string]config.Recipe, roots ...string) *config.Workspace {
	t.Helper()
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts", State: ".artifacts/pack", RecipesMap: recipes, AppsMap: make(map[string]config.App)}
	for id, recipe := range recipes {
		recipe.ID = id
		if len(recipe.Tools) == 0 {
			recipe.Tools = []string{"sh"}
		}
		recipes[id] = recipe
	}
	for _, id := range roots {
		recipe := recipes[id]
		workspace.AppsMap[id] = config.App{ID: id, File: map[string]any{"recipe": id, "path": recipe.Outputs[0]}}
	}
	return workspace
}

func TestRecipeSchedulerDiamondOrdering(t *testing.T) {
	t.Setenv("PACGO_BUILD_JOBS", "3")
	workspace := schedulerWorkspace(t, map[string]config.Recipe{
		"a": {Run: []string{"sh", "-c", "printf 'a\\n' >> order; printf a > a.out"}, Outputs: []string{"a.out"}},
		"b": {Run: []string{"sh", "-c", "test -f a.out; printf 'b\\n' >> order; printf b > b.out"}, Needs: []string{"a"}, Outputs: []string{"b.out"}},
		"c": {Run: []string{"sh", "-c", "test -f a.out; printf 'c\\n' >> order; printf c > c.out"}, Needs: []string{"a"}, Outputs: []string{"c.out"}},
		"d": {Run: []string{"sh", "-c", "test -f b.out && test -f c.out; printf 'd\\n' >> order; printf d > d.out"}, Needs: []string{"b", "c"}, Outputs: []string{"d.out"}},
	}, "d")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(workspace.Path("order"))
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Fields(string(data))
	if len(lines) != 4 || lines[0] != "a" || lines[3] != "d" {
		t.Fatalf("order = %q", data)
	}
}

func TestRecipeCascadeOnlyWhenUpstreamOutputChanges(t *testing.T) {
	t.Setenv("PACGO_BUILD_JOBS", "2")
	workspace := schedulerWorkspace(t, map[string]config.Recipe{
		"producer": {Run: []string{"sh", "-c", "cat desired > producer.out; printf x >> producer.runs"}, Inputs: []config.RecipeInput{{Path: "desired"}, {Path: "trigger"}}, Outputs: []string{"producer.out"}},
		"consumer": {Run: []string{"sh", "-c", "printf x >> consumer.runs; cp producer.out consumer.out"}, Needs: []string{"producer"}, Outputs: []string{"consumer.out"}},
	}, "consumer")
	writeCacheFixture(t, workspace, "desired", "same")
	writeCacheFixture(t, workspace, "trigger", "one")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	writeCacheFixture(t, workspace, "trigger", "two")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	consumerRuns, _ := os.ReadFile(workspace.Path("consumer.runs"))
	if string(consumerRuns) != "x" {
		t.Fatalf("consumer cascaded after unchanged output: %q", consumerRuns)
	}
	writeCacheFixture(t, workspace, "desired", "changed")
	writeCacheFixture(t, workspace, "trigger", "three")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	consumerRuns, _ = os.ReadFile(workspace.Path("consumer.runs"))
	if string(consumerRuns) != "xx" {
		t.Fatalf("needs-only edge did not cascade: %q", consumerRuns)
	}
}

func TestNeedRebuiltWithoutConsumerMakesConsumerStale(t *testing.T) {
	t.Setenv("PACGO_BUILD_JOBS", "2")
	// The consumer reads the producer's output without declaring it, as the
	// LPR fixtures read the Alpine sysroot.
	workspace := schedulerWorkspace(t, map[string]config.Recipe{
		"producer": {Run: []string{"sh", "-c", "cat desired > producer.out"}, Inputs: []config.RecipeInput{{Path: "desired"}}, Outputs: []string{"producer.out"}},
		"consumer": {Run: []string{"sh", "-c", "printf x >> consumer.runs; cp producer.out consumer.out"}, Needs: []string{"producer"}, Outputs: []string{"consumer.out"}},
	}, "producer", "consumer")
	writeCacheFixture(t, workspace, "desired", "one")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	writeCacheFixture(t, workspace, "desired", "two")
	if _, err := BuildUserland(workspace, UserlandOptions{AppIDs: []string{"producer"}}); err != nil {
		t.Fatal(err)
	}
	status, _, err := recipeStatusInEnvironment(workspace, workspace.RecipesMap["consumer"], hasherFor(workspace), environmentMap(os.Environ()))
	if err != nil {
		t.Fatal(err)
	}
	if status.UpToDate || status.Reason != "needed recipe output changed: producer" {
		t.Fatalf("consumer status after its need was rebuilt alone = %+v", status)
	}
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	consumerRuns, _ := os.ReadFile(workspace.Path("consumer.runs"))
	consumerOut, _ := os.ReadFile(workspace.Path("consumer.out"))
	if string(consumerRuns) != "xx" || string(consumerOut) != "two" {
		t.Fatalf("consumer runs = %q, output = %q", consumerRuns, consumerOut)
	}
}

func TestIndependentRecipesOverlap(t *testing.T) {
	t.Setenv("PACGO_BUILD_JOBS", "2")
	peerWait := "touch %s.start; i=0; while test ! -f %s.start && test $i -lt 100; do sleep .01; i=$((i+1)); done; test -f %s.start; printf ok > %s.out"
	workspace := schedulerWorkspace(t, map[string]config.Recipe{
		"a": {Run: []string{"sh", "-c", fmt.Sprintf(peerWait, "a", "b", "b", "a")}, Outputs: []string{"a.out"}},
		"b": {Run: []string{"sh", "-c", fmt.Sprintf(peerWait, "b", "a", "a", "b")}, Outputs: []string{"b.out"}},
	}, "a", "b")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
}

func TestFailureStopsNewRecipesAndRunningSuccessRecordsState(t *testing.T) {
	t.Setenv("PACGO_BUILD_JOBS", "2")
	workspace := schedulerWorkspace(t, map[string]config.Recipe{
		"a": {Run: []string{"sh", "-c", "sleep .05; exit 1"}, Outputs: []string{"a.out"}},
		"b": {Run: []string{"sh", "-c", "sleep .15; printf done > b.out"}, Outputs: []string{"b.out"}},
		"c": {Run: []string{"sh", "-c", "touch c.started; printf done > c.out"}, Outputs: []string{"c.out"}},
	}, "a", "b", "c")
	started := time.Now()
	if _, err := BuildUserland(workspace, UserlandOptions{}); err == nil || !strings.Contains(err.Error(), "a") {
		t.Fatalf("error = %v", err)
	}
	if time.Since(started) < 120*time.Millisecond {
		t.Fatal("scheduler returned before an already-running recipe finished")
	}
	if _, err := os.Stat(recipeStatePath(workspace, "b")); err != nil {
		t.Fatalf("successful running recipe state: %v", err)
	}
	if _, err := os.Stat(workspace.Path("c.started")); !os.IsNotExist(err) {
		t.Fatalf("recipe c was scheduled after failure: %v", err)
	}
}

func TestRecipeLogAndNullStdin(t *testing.T) {
	t.Setenv("PACGO_BUILD_JOBS", "1")
	workspace := schedulerWorkspace(t, map[string]config.Recipe{
		"read": {Run: []string{"sh", "-c", "if read value; then exit 9; fi; echo stdout; echo stderr >&2; printf ok > read.out"}, Outputs: []string{"read.out"}},
	}, "read")
	if _, err := BuildUserland(workspace, UserlandOptions{}); err != nil {
		t.Fatal(err)
	}
	log, err := os.ReadFile(filepath.Join(workspace.Root, ".artifacts", "pack", "logs", "read.log"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(log), "stdout") || !strings.Contains(string(log), "stderr") {
		t.Fatalf("log = %q", log)
	}
}
