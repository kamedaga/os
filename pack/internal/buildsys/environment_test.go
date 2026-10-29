package buildsys

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestNixEnvironmentCacheKeyInvalidation(t *testing.T) {
	root := t.TempDir()
	workspace := &config.Workspace{Root: root, State: ".artifacts/pack"}
	writeCacheFixture(t, workspace, "flake.nix", "one")
	writeCacheFixture(t, workspace, "flake.lock", "lock")
	writeCacheFixture(t, workspace, "nix/toolchains.nix", "tools")
	original := captureNixEnvironment
	t.Cleanup(func() { captureNixEnvironment = original })
	captures := 0
	captureNixEnvironment = func(root string) (nixEnvironmentDelta, error) {
		captures++
		return nixEnvironmentDelta{
			Variables:  map[string]string{"HOOK_VALUE": "captured", "IN_NIX_SHELL": "1"},
			PathPrefix: []string{filepath.Join(root, "nix-bin")},
		}, nil
	}
	first := newRecipeEnvironment(workspace, newHasher(workspace.Path(".state/hash-one")))
	env, err := first.variables()
	if err != nil {
		t.Fatal(err)
	}
	if env["HOOK_VALUE"] != "captured" || env["IN_NIX_SHELL"] != "1" || captures != 1 {
		t.Fatalf("env=%#v captures=%d", env, captures)
	}
	second := newRecipeEnvironment(workspace, newHasher(workspace.Path(".state/hash-two")))
	if _, err := second.variables(); err != nil {
		t.Fatal(err)
	}
	if captures != 1 {
		t.Fatalf("cached environment recaptured: %d", captures)
	}
	writeCacheFixture(t, workspace, "nix/toolchains.nix", "changed")
	third := newRecipeEnvironment(workspace, newHasher(workspace.Path(".state/hash-three")))
	if _, err := third.variables(); err != nil {
		t.Fatal(err)
	}
	if captures != 2 {
		t.Fatalf("changed nix input did not invalidate environment: %d", captures)
	}
}

func TestNixEnvironmentDeltaOverlayAndExclusions(t *testing.T) {
	outside := []byte("HOME=/home/test\x00PATH=/usr/bin:/bin\x00LANG=C.UTF-8\x00")
	inside := []byte("HOME=/home/test\x00PATH=/nix/a/bin:/nix/b/bin:/usr/bin:/bin\x00" +
		"LANG=C.UTF-8\x00ROCQPATH=/nix/rocq\x00IN_NIX_SHELL=1\x00" +
		"PWD=/workspace\x00OLDPWD=/old\x00SHLVL=2\x00_=/usr/bin/env\x00" +
		"NIX_BUILD_TOP=/tmp/nix-shell.random\x00TMPDIR=/tmp/nix-shell.random\x00")
	delta, err := nixEnvironmentDeltaFromSnapshots(outside, inside)
	if err != nil {
		t.Fatal(err)
	}
	wantPrefix := []string{"/nix/a/bin", "/nix/b/bin"}
	if fmt.Sprint(delta.PathPrefix) != fmt.Sprint(wantPrefix) {
		t.Fatalf("PATH prefix = %#v, want %#v", delta.PathPrefix, wantPrefix)
	}
	for _, name := range []string{"PWD", "OLDPWD", "SHLVL", "_", "NIX_BUILD_TOP", "TMPDIR"} {
		if _, present := delta.Variables[name]; present {
			t.Fatalf("per-session variable %s was captured", name)
		}
	}
	current := map[string]string{
		"PATH": "/caller/bin:/bin", "DISPLAY": ":9", "ROCQPATH": "caller", "CLAUDECODE": "1",
	}
	overlaid := applyNixEnvironment(current, delta)
	if got := overlaid["PATH"]; got != "/nix/a/bin:/nix/b/bin:/caller/bin:/bin" {
		t.Fatalf("overlaid PATH = %q", got)
	}
	if overlaid["ROCQPATH"] != "/nix/rocq" || overlaid["IN_NIX_SHELL"] != "1" {
		t.Fatalf("devShell variables were not overlaid: %#v", overlaid)
	}
	if overlaid["DISPLAY"] != ":9" || overlaid["CLAUDECODE"] != "1" {
		t.Fatalf("caller variables were not preserved: %#v", overlaid)
	}
}

func TestCaptureNixEnvironmentUsesSanitizedParentAndIgnoresStdout(t *testing.T) {
	root := t.TempDir()
	nix := filepath.Join(root, "nix")
	script := `#!/bin/sh
test "$PATH" = /usr/bin:/bin
test -z "${DISPLAY+x}"
printf '%s\n' 'shellHook noise that must not enter env data'
for output do :; done
printf 'PATH=/nix/tool/bin:/usr/bin:/bin\0ROCQPATH=/nix/rocq\0PWD=/session\0' >"$output"
`
	if err := os.WriteFile(nix, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", root+string(filepath.ListSeparator)+os.Getenv("PATH"))
	t.Setenv("DISPLAY", ":caller")
	delta, err := captureNixDevShellEnvironment(root)
	if err != nil {
		t.Fatal(err)
	}
	if fmt.Sprint(delta.PathPrefix) != "[/nix/tool/bin]" {
		t.Fatalf("PATH prefix = %#v", delta.PathPrefix)
	}
	if len(delta.Variables) != 1 || delta.Variables["ROCQPATH"] != "/nix/rocq" {
		t.Fatalf("captured variables = %#v", delta.Variables)
	}
}

func TestNixToolResolutionUsesOverlaidPath(t *testing.T) {
	root := t.TempDir()
	bin := filepath.Join(root, "nix-bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	tool := filepath.Join(bin, "chosen-tool")
	if err := os.WriteFile(tool, []byte("nix tool"), 0o755); err != nil {
		t.Fatal(err)
	}
	recipe := config.Recipe{ID: "tool", Tools: []string{"chosen-tool"}}
	env := applyNixEnvironment(map[string]string{"PATH": "/usr/bin:/bin"}, nixEnvironmentDelta{PathPrefix: []string{bin}})
	states, err := snapshotRecipeToolsInEnvironment(recipe, newHasher(filepath.Join(root, "hashcache")), env)
	if err != nil {
		t.Fatal(err)
	}
	if len(states) != 1 || states[0].Path != tool {
		t.Fatalf("tools = %#v", states)
	}
}

func TestBeginRunSharesHasherByAbsoluteCachePath(t *testing.T) {
	root := t.TempDir()
	one := &config.Workspace{Root: root, State: ".artifacts/pack"}
	two := *one
	BeginRun(one)
	if hasherFor(one) != hasherFor(&two) {
		t.Fatal("workspace copy did not share the hasher for the same cache path")
	}
}
