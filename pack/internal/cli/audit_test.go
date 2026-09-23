package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestSourceAuditRejectsArtifactWithoutRecipe(t *testing.T) {
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		AppsMap: map[string]config.App{
			"lost": {File: map[string]any{"path": ".artifacts/lost.elf"}},
		},
	}
	problems := sourceAuditProblems(workspace)
	if len(problems) != 1 || !strings.Contains(problems[0], "has no rebuild recipe") {
		t.Fatalf("problems = %#v", problems)
	}
}

func TestSourceAuditAcceptsTrackedRecipe(t *testing.T) {
	root := t.TempDir()
	script := filepath.Join(root, "build.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	workspace := &config.Workspace{
		Root:      root,
		Artifacts: ".artifacts",
		AppsMap: map[string]config.App{
			"ready": {File: map[string]any{
				"path":    ".artifacts/ready.elf",
				"rebuild": []any{"bash", "build.sh"},
			}},
		},
	}
	if problems := sourceAuditProblems(workspace); len(problems) != 0 {
		t.Fatalf("problems = %#v", problems)
	}
}

func TestSourceAuditRejectsInlineShell(t *testing.T) {
	workspace := &config.Workspace{
		Root:      t.TempDir(),
		Artifacts: ".artifacts",
		AppsMap: map[string]config.App{
			"inline": {File: map[string]any{
				"path":    ".artifacts/inline.elf",
				"rebuild": []any{"bash", "-lc", "cc input.c -o .artifacts/inline.elf"},
			}},
		},
	}
	problems := sourceAuditProblems(workspace)
	if len(problems) != 1 || !strings.Contains(problems[0], "inline bash") {
		t.Fatalf("problems = %#v", problems)
	}
}
