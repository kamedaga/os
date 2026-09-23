package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

var rebuildPathPattern = regexp.MustCompile(`[A-Za-z0-9_./-]+\.(?:sh|py)`)

func auditCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "audit",
		Short: "Validate reproducible workspace inputs",
	}
	cmd.AddCommand(&cobra.Command{
		Use:   "sources",
		Short: "Reject artifact-only inputs and missing build recipes",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runSourceAudit(ctx.workspace)
		},
	})
	return cmd
}

func runSourceAudit(workspace *config.Workspace) error {
	ui.Task("audit:sources")
	problems := sourceAuditProblems(workspace)
	if len(problems) != 0 {
		for _, problem := range problems {
			fmt.Fprintln(os.Stderr, "- "+problem)
		}
		return fmt.Errorf("source audit found %d problem(s)", len(problems))
	}
	active := 0
	for _, app := range workspace.Apps() {
		if !workspace.Skipped(app) {
			active++
		}
	}
	ui.KeyValues("Source audit", [][2]string{
		{"active apps", fmt.Sprint(active)},
		{"state", "recipes declared"},
	})
	return nil
}

func sourceAuditProblems(workspace *config.Workspace) []string {
	var problems []string
	for _, app := range workspace.Apps() {
		if workspace.Skipped(app) {
			continue
		}
		if app.Kind() != "file" {
			continue
		}
		source, err := app.FileSource()
		if err != nil {
			problems = append(problems, fmt.Sprintf("%s: %v", app.ID, err))
			continue
		}
		if isArtifactRelative(workspace, source.Path) && len(source.Rebuild) == 0 {
			problems = append(problems, fmt.Sprintf(
				"%s: artifact source %s has no rebuild recipe", app.ID, source.Path))
		}
		for _, input := range source.Input {
			if isArtifactRelative(workspace, input) {
				problems = append(problems, fmt.Sprintf(
					"%s: generated artifact %s is declared as a source input", app.ID, input))
			}
		}
		joined := strings.Join(source.Rebuild, " ")
		if strings.Contains(joined, "latest-stable") {
			problems = append(problems, fmt.Sprintf(
				"%s: rebuild recipe selects rolling Alpine latest-stable", app.ID))
		}
		if len(source.Rebuild) >= 2 && source.Rebuild[0] == "bash" && source.Rebuild[1] == "-lc" {
			problems = append(problems, fmt.Sprintf(
				"%s: inline bash recipe must be a tracked script", app.ID))
		}
		if len(source.Rebuild) > 1 && source.Rebuild[0] == "cmake" && source.Rebuild[1] == "--build" {
			problems = append(problems, fmt.Sprintf(
				"%s: cmake --build recipe assumes deleted configure state", app.ID))
		}
		seen := map[string]bool{}
		for _, argument := range source.Rebuild {
			for _, candidate := range rebuildPathPattern.FindAllString(argument, -1) {
				if seen[candidate] || strings.HasPrefix(candidate, ".artifacts/") {
					continue
				}
				seen[candidate] = true
				path := candidate
				if !filepath.IsAbs(path) {
					path = workspace.Path(path)
				}
				if info, err := os.Stat(path); err != nil || info.IsDir() {
					problems = append(problems, fmt.Sprintf(
						"%s: rebuild program %s does not exist", app.ID, candidate))
				}
			}
		}
	}
	sort.Strings(problems)
	return problems
}

func isArtifactRelative(workspace *config.Workspace, path string) bool {
	if filepath.IsAbs(path) {
		return false
	}
	clean := filepath.Clean(path)
	artifacts := filepath.Clean(workspace.Artifacts)
	return clean == artifacts || strings.HasPrefix(clean, artifacts+string(filepath.Separator))
}
