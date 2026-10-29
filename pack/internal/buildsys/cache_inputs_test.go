package buildsys

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"capabilityos/pack/internal/config"
)

func writeCacheFixture(t *testing.T, workspace *config.Workspace, name, contents string) {
	t.Helper()
	path := workspace.Path(name)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(contents), 0o644); err != nil {
		t.Fatal(err)
	}
}

func cacheFixtureInputs(t *testing.T, workspace *config.Workspace, recipe config.Recipe) []recipeInputState {
	t.Helper()
	value, err := snapshotRecipeInputs(workspace, recipe, newHasher(workspace.Path(".state", "test-hashcache")))
	if err != nil {
		t.Fatal(err)
	}
	return value
}

func TestRecipeOutputsDoNotInvalidateCache(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}
	recipe := config.Recipe{
		Run: []string{"bash", "tools/build.sh", "src/app", ".artifacts/build", ".artifacts/build/app.elf"},
	}
	writeCacheFixture(t, workspace, "tools/build.sh", "build")
	writeCacheFixture(t, workspace, "src/app/main.c", "first source")
	before := cacheFixtureInputs(t, workspace, recipe)
	writeCacheFixture(t, workspace, ".artifacts/build/app.elf", "rebuilt output")
	writeCacheFixture(t, workspace, ".artifacts/build/CMakeCache.txt", "changed configure state")
	if !reflect.DeepEqual(before, cacheFixtureInputs(t, workspace, recipe)) {
		t.Fatal("generated recipe arguments invalidated their own cache")
	}
	writeCacheFixture(t, workspace, "src/app/main.c", "second source")
	if reflect.DeepEqual(before, cacheFixtureInputs(t, workspace, recipe)) {
		t.Fatal("source change did not invalidate cache")
	}
	// Explicit inputs still carry content dependencies, including generated ones.
	recipe.Inputs = []config.RecipeInput{{Path: ".artifacts/build/app.elf"}}
	before = cacheFixtureInputs(t, workspace, recipe)
	writeCacheFixture(t, workspace, ".artifacts/build/app.elf", "different explicit input")
	if reflect.DeepEqual(before, cacheFixtureInputs(t, workspace, recipe)) {
		t.Fatal("explicit generated input was ignored")
	}
}

func TestNativeCMakeDependencyIsNotRegexInferred(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}
	recipe := config.Recipe{Run: []string{"bash", "tools/build.sh"}, Inputs: []config.RecipeInput{{Path: "userland/cmake"}}}
	writeCacheFixture(t, workspace, "tools/build.sh", "build")
	writeCacheFixture(t, workspace, "userland/cmake/pachaos_native_dependencies.cmake", `set(PACHAOS_NATIVE_DEPENDENCIES "${_pachaos_native_root}/userland/shared/codec.c")`)
	writeCacheFixture(t, workspace, "userland/shared/codec.c", "old codec")
	before := cacheFixtureInputs(t, workspace, recipe)
	writeCacheFixture(t, workspace, "userland/shared/codec.c", "new codec")
	if !reflect.DeepEqual(before, cacheFixtureInputs(t, workspace, recipe)) {
		t.Fatal("native dependency was still inferred outside build-system discovery")
	}
}
