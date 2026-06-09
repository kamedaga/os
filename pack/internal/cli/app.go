package cli

import (
	"fmt"
	"strings"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

func appCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "app",
		Short: "Inspect app definitions",
	}
	cmd.AddCommand(&cobra.Command{
		Use:   "list",
		Short: "List apps",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("app:list")
			rows := [][]string{}
			for _, app := range ctx.workspace.Apps() {
				state := "active"
				if ctx.workspace.Skipped(app) {
					state = "skip"
				}
				rows = append(rows, []string{app.ID, state, app.Kind(), app.Role})
			}
			ui.Table("Apps", []string{"App", "State", "Kind", "Role"}, rows)
			return nil
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "show <app>",
		Short: "Show one app",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			app, ok := ctx.workspace.App(args[0])
			if !ok {
				return fmt.Errorf("app not found: %s", args[0])
			}
			ui.Task("app:show")
			ui.KeyValues("App "+app.ID, appRows(ctx.workspace, app))
			return nil
		},
	})
	return cmd
}

func appRows(workspace *config.Workspace, app config.App) [][2]string {
	state := "active"
	if workspace.Skipped(app) {
		state = "skip"
	}
	return [][2]string{
		{"id", app.ID},
		{"state", state},
		{"kind", app.Kind()},
		{"role", app.Role},
		{"out", app.Out},
		{"target", dash(app.Target)},
		{"optimize", app.Optimize},
		{"publishes", dash(strings.Join(app.PublishSummary(), ", "))},
	}
}
