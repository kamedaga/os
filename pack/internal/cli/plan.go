package cli

import (
	"fmt"
	"strings"

	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

func planCommand(ctx *context) *cobra.Command {
	return &cobra.Command{
		Use:   "plan",
		Short: "Show workspace plan",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runPlan(ctx)
		},
	}
}

func runPlan(ctx *context) error {
	ui.Task("plan")
	apps := ctx.workspace.Apps()
	active, skipped := 0, 0
	for _, app := range apps {
		if ctx.workspace.Skipped(app) {
			skipped++
		} else {
			active++
		}
	}
	ui.KeyValues("Workspace", [][2]string{
		{"name", ctx.workspace.Name},
		{"root", ctx.workspace.Root},
		{"definition", ctx.workspace.ConfigPath},
		{"kernel", ctx.workspace.Kernel.Dir + " (step: " + ctx.workspace.Kernel.Step + ")"},
		{"apps", fmt.Sprintf("%d (%d active, %d skipped)", len(apps), active, skipped)},
		{"disk", fmt.Sprintf("%s (%d MiB)", ctx.workspace.Disk.Image, ctx.workspace.Disk.SizeMiB)},
		{"artifacts", ctx.workspace.Artifacts},
		{"manifests", ctx.workspace.Manifests.Dir},
		{"skip apps", dash(strings.Join(ctx.workspace.Skip.Apps, ", "))},
		{"skip kinds", dash(strings.Join(ctx.workspace.Skip.Kinds, ", "))},
	})
	return nil
}

func dash(value string) string {
	if value == "" {
		return "-"
	}
	return value
}
