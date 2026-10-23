package buildsys

import (
	"bytes"
	"os"
	"path/filepath"
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

func cacheFixtureFingerprint(t *testing.T, workspace *config.Workspace, source config.FileSource) []byte {
	t.Helper()
	value, err := fileSourceFingerprint(workspace, source)
	if err != nil {
		t.Fatal(err)
	}
	return value
}

func TestRecipeOutputsDoNotInvalidateCache(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}
	source := config.FileSource{
		Path:    ".artifacts/build/app.elf",
		Rebuild: []string{"bash", "tools/build.sh", "src/app", ".artifacts/build", ".artifacts/build/app.elf"},
	}
	writeCacheFixture(t, workspace, "tools/build.sh", "build")
	writeCacheFixture(t, workspace, "src/app/main.c", "first source")
	before := cacheFixtureFingerprint(t, workspace, source)
	writeCacheFixture(t, workspace, ".artifacts/build/app.elf", "rebuilt output")
	writeCacheFixture(t, workspace, ".artifacts/build/CMakeCache.txt", "changed configure state")
	if !bytes.Equal(before, cacheFixtureFingerprint(t, workspace, source)) {
		t.Fatal("generated recipe arguments invalidated their own cache")
	}
	writeCacheFixture(t, workspace, "src/app/main.c", "second source")
	if bytes.Equal(before, cacheFixtureFingerprint(t, workspace, source)) {
		t.Fatal("source change did not invalidate cache")
	}
	// Explicit inputs still carry content dependencies, including generated ones.
	source.Input = []string{".artifacts/build/app.elf"}
	before = cacheFixtureFingerprint(t, workspace, source)
	writeCacheFixture(t, workspace, ".artifacts/build/app.elf", "different explicit input")
	if bytes.Equal(before, cacheFixtureFingerprint(t, workspace, source)) {
		t.Fatal("explicit generated input was ignored")
	}
}

func TestNativeCMakeDependencyInvalidatesOuterCache(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts"}
	source := config.FileSource{Rebuild: []string{"bash", "tools/build.sh"}, Input: []string{"userland/cmake"}}
	writeCacheFixture(t, workspace, "tools/build.sh", "build")
	writeCacheFixture(t, workspace, "userland/cmake/pachaos_native_dependencies.cmake", `set(PACHAOS_NATIVE_DEPENDENCIES "${_pachaos_native_root}/userland/shared/codec.c")`)
	writeCacheFixture(t, workspace, "userland/shared/codec.c", "old codec")
	before := cacheFixtureFingerprint(t, workspace, source)
	writeCacheFixture(t, workspace, "userland/shared/codec.c", "new codec")
	if bytes.Equal(before, cacheFixtureFingerprint(t, workspace, source)) {
		t.Fatal("native dependency change did not reach outer cache")
	}
}
