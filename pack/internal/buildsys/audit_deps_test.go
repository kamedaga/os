package buildsys

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestAnalyzeDependencyTraceClassifiesAuditFindings(t *testing.T) {
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, Artifacts: ".artifacts", State: ".artifacts/pack"}
	for name := range map[string]bool{"declared.txt": true, "unused.txt": true, "discovered.c": true, "missing.txt": true, "excluded.c": true, "globbed.c": true, "statted.txt": true} {
		mustWriteDiscoveryFile(t, filepath.Join(root, name), name)
	}
	tool := filepath.Join(root, "declared-tool")
	mustWriteDiscoveryFile(t, tool, "tool")
	state := recipeState{
		DeclaredInputs:   []recipeInputState{{Path: "declared.txt", SHA256: "x"}, {Path: "unused.txt", SHA256: "x"}},
		DiscoveredInputs: []recipeInputState{{Path: "discovered.c", SHA256: "x"}},
		Tools:            []recipeToolState{{Name: "tool", Path: tool, SHA256: "x"}},
	}
	if err := writeRecipeState(workspace, "app", state); err != nil {
		t.Fatal(err)
	}
	trace := "1 openat(AT_FDCWD<" + root + ">, \"declared.txt\", O_RDONLY) = 3<" + filepath.Join(root, "declared.txt") + ">\n" +
		"1 openat(AT_FDCWD<" + root + ">, \"discovered.c\", O_RDONLY) = 3<" + filepath.Join(root, "discovered.c") + ">\n" +
		"1 openat(AT_FDCWD<" + root + ">, \"missing.txt\", O_RDONLY) = 3<" + filepath.Join(root, "missing.txt") + ">\n" +
		"1 openat(AT_FDCWD<" + root + ">, \"excluded.c\", O_RDONLY) = 3<" + filepath.Join(root, "excluded.c") + ">\n" +
		"1 newfstatat(AT_FDCWD<" + root + ">, \"globbed.c\", {st_mode=S_IFREG|0644}, 0) = 0\n" +
		"1 newfstatat(AT_FDCWD<" + root + ">, \"statted.txt\", {st_mode=S_IFREG|0644}, 0) = 0\n" +
		"1 newfstatat(AT_FDCWD<" + root + ">, \"excluded.c\", {st_mode=S_IFREG|0644}, 0) = 0\n" +
		"1 execve(\"/usr/bin/undeclared\", [\"undeclared\"], 0x0) = 0\n" +
		"1 access(\"" + filepath.Join(root, "shadow.h") + "\", F_OK) = -1 ENOENT (No such file or directory)\n"
	tracePath := filepath.Join(root, ".artifacts", "pack", "logs", "app.strace.log")
	mustWriteDiscoveryFile(t, tracePath, trace)
	result, err := analyzeDependencyTrace(workspace, config.Recipe{ID: "app", Inputs: []config.RecipeInput{{Path: ".", Exclude: []string{"*.c"}}}}, tracePath)
	if err != nil {
		t.Fatal(err)
	}
	// Only content reads of excluded files are errors; a stat of an undeclared
	// file still is one.
	if !reflect.DeepEqual(result.ExcludedStatted, []string{"globbed.c"}) {
		t.Fatalf("excluded statted = %#v", result.ExcludedStatted)
	}
	if !reflect.DeepEqual(result.Missing, []string{"missing.txt", "statted.txt"}) {
		t.Fatalf("missing = %#v", result.Missing)
	}
	if !reflect.DeepEqual(result.ExcludedButRead, []string{"excluded.c"}) {
		t.Fatalf("excluded but read = %#v", result.ExcludedButRead)
	}
	if !reflect.DeepEqual(result.Overdeclared, []string{"unused.txt"}) {
		t.Fatalf("overdeclared = %#v", result.Overdeclared)
	}
	if !reflect.DeepEqual(result.UndeclaredTools, []string{"/usr/bin/undeclared"}) {
		t.Fatalf("tools = %#v", result.UndeclaredTools)
	}
	if !reflect.DeepEqual(result.FailedLookups, []string{"shadow.h"}) {
		t.Fatalf("failed = %#v", result.FailedLookups)
	}
	if _, err := os.Stat(tracePath); err != nil {
		t.Fatal(err)
	}
}

func TestAuditTreatsNeededOutputsAsTracked(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Artifacts: ".artifacts", State: ".artifacts/pack", RecipesMap: map[string]config.Recipe{
		"sysroot": {ID: "sysroot", Outputs: []string{".artifacts/sysroot"}},
		"other":   {ID: "other", Outputs: []string{".artifacts/other"}},
	}}
	consumer := config.Recipe{ID: "app", Needs: []string{"sysroot"}, Outputs: []string{".artifacts/app.elf"}}
	if auditInputPath(workspace, consumer, workspace.Path(".artifacts/sysroot/usr/lib/libc.a")) {
		t.Fatal("a file inside a needed output was reported as an undeclared input")
	}
	if !auditInputPath(workspace, consumer, workspace.Path(".artifacts/other/file")) {
		t.Fatal("a file inside an output that is not needed was not reported")
	}
	if auditInputPath(workspace, consumer, workspace.Path(".artifacts/app.elf")) {
		t.Fatal("the recipe's own output was reported")
	}
}

func TestStoreReferenceScannerCoversWrappedBinaries(t *testing.T) {
	wrapper := filepath.Join(t.TempDir(), "clang")
	mustWriteDiscoveryFile(t, wrapper, "#!/bin/sh\nexec /nix/store/aaaa-clang-21/bin/clang \"$@\"\n")
	tools := []recipeToolState{{Name: "clang", Path: wrapper, SHA256: "x"}}
	scanner := storeReferenceScanner{}
	if !scanner.referencedByTool(tools, "/nix/store/aaaa-clang-21/bin/clang") {
		t.Fatal("binary named by the declared wrapper was reported")
	}
	if scanner.referencedByTool(tools, "/nix/store/bbbb-gcc-14/bin/gcc") {
		t.Fatal("unrelated store binary was accepted")
	}
	if scanner.referencedByTool(tools, "/usr/bin/clang") {
		t.Fatal("non-store binary was accepted")
	}
}
