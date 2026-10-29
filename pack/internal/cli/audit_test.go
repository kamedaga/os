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
		RecipesMap: map[string]config.Recipe{
			"ready": {ID: "ready", Run: []string{"bash", "build.sh"}, Inputs: []config.RecipeInput{{Path: "build.sh"}}, Outputs: []string{".artifacts/ready.elf"}},
		},
		AppsMap: map[string]config.App{
			"ready": {File: map[string]any{
				"path":   ".artifacts/ready.elf",
				"recipe": "ready",
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
		RecipesMap: map[string]config.Recipe{
			"inline": {ID: "inline", Run: []string{"bash", "-lc", "cc input.c -o .artifacts/inline.elf"}, Outputs: []string{".artifacts/inline.elf"}},
		},
		AppsMap: map[string]config.App{
			"inline": {File: map[string]any{
				"path":   ".artifacts/inline.elf",
				"recipe": "inline",
			}},
		},
	}
	problems := sourceAuditProblems(workspace)
	if len(problems) != 1 || !strings.Contains(problems[0], "inline bash") {
		t.Fatalf("problems = %#v", problems)
	}
}
