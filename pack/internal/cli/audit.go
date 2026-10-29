package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"capabilityos/pack/internal/buildsys"
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
	cmd.AddCommand(&cobra.Command{
		Use:   "deps <recipe>...",
		Short: "Trace recipe reads and audit declared dependencies and tools",
		Args:  cobra.MinimumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			return runDependencyAudit(ctx.workspace, args)
		},
	})
	return cmd
}

func runDependencyAudit(workspace *config.Workspace, recipes []string) error {
	ui.Task("audit:deps")
	results, err := buildsys.AuditRecipeDependencies(workspace, recipes)
	if err != nil {
		return err
	}
	failures := 0
	for _, result := range results {
		fmt.Printf("\n%s (%s)\n", result.Recipe, result.TraceLog)
		if len(result.Missing) == 0 {
			fmt.Println("  missing deps: none")
		} else {
			failures += len(result.Missing)
			fmt.Println("  missing deps:")
			for _, group := range buildsys.MissingDependenciesByDirectory(result.Missing) {
				fmt.Printf("    %s: %s\n", group[0], group[1])
			}
			fmt.Println("  suggested inputs additions:")
			for _, path := range result.Missing {
				fmt.Printf("    - %q\n", filepath.ToSlash(path))
			}
		}
		if len(result.ExcludedButRead) != 0 {
			failures += len(result.ExcludedButRead)
		}
		printAuditPaths("excluded-but-read deps", result.ExcludedButRead)
		printAuditPaths("excluded files only stat'ed (informational)", result.ExcludedStatted)
		printAuditPaths("declared but unread (informational)", result.Overdeclared)
		if len(result.UndeclaredTools) != 0 {
			failures += len(result.UndeclaredTools)
		}
		printAuditPaths("executed binaries absent from tools", result.UndeclaredTools)
		printAuditPaths("failed workspace lookups (negative-dep candidates)", result.FailedLookups)
	}
	if failures != 0 {
		return fmt.Errorf("dependency audit found %d dependency/tool error(s)", failures)
	}
	return nil
}

func printAuditPaths(label string, paths []string) {
	if len(paths) == 0 {
		fmt.Printf("  %s: none\n", label)
		return
	}
	fmt.Printf("  %s:\n", label)
	for _, path := range paths {
		fmt.Printf("    - %s\n", path)
	}
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
	activeRecipes := make(map[string]bool)
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
		if isArtifactRelative(workspace, source.Path) && source.Recipe == "" {
			problems = append(problems, fmt.Sprintf(
				"%s: artifact source %s has no rebuild recipe", app.ID, source.Path))
		}
		if source.Recipe != "" {
			activeRecipes[source.Recipe] = true
		}
	}
	for _, recipe := range workspace.Recipes() {
		if !activeRecipes[recipe.ID] {
			continue
		}
		for _, input := range recipe.Inputs {
			if isArtifactRelative(workspace, input.Path) && !recipeInputHasOwner(workspace, recipe.ID, input.Path) {
				problems = append(problems, fmt.Sprintf(
					"%s: generated artifact input %s has no producing recipe", recipe.ID, input.Path))
			}
		}
		joined := strings.Join(recipe.Run, " ")
		if strings.Contains(joined, "latest-stable") {
			problems = append(problems, fmt.Sprintf(
				"%s: rebuild recipe selects rolling Alpine latest-stable", recipe.ID))
		}
		if len(recipe.Run) >= 2 && recipe.Run[0] == "bash" && recipe.Run[1] == "-lc" {
			problems = append(problems, fmt.Sprintf(
				"%s: inline bash recipe must be a tracked script", recipe.ID))
		}
		if len(recipe.Run) > 1 && recipe.Run[0] == "cmake" && recipe.Run[1] == "--build" {
			problems = append(problems, fmt.Sprintf(
				"%s: cmake --build recipe assumes deleted configure state", recipe.ID))
		}
		seen := map[string]bool{}
		for _, argument := range recipe.Run {
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
						"%s: rebuild program %s does not exist", recipe.ID, candidate))
				}
			}
		}
	}
	sort.Strings(problems)
	return problems
}

func recipeInputHasOwner(workspace *config.Workspace, consumerID string, input string) bool {
	input = filepath.Clean(input)
	for _, recipe := range workspace.Recipes() {
		if recipe.ID == consumerID {
			continue
		}
		for _, output := range recipe.Outputs {
			output = filepath.Clean(output)
			rel, err := filepath.Rel(output, input)
			if err == nil && rel != ".." && !filepath.IsAbs(rel) && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
				return true
			}
		}
	}
	return false
}

func isArtifactRelative(workspace *config.Workspace, path string) bool {
	if filepath.IsAbs(path) {
		return false
	}
	clean := filepath.Clean(path)
	artifacts := filepath.Clean(workspace.Artifacts)
	return clean == artifacts || strings.HasPrefix(clean, artifacts+string(filepath.Separator))
}
