package cli

import (
	"fmt"

	"capabilityos/pack/internal/buildsys"
	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

func stateCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "state",
		Short: "Inspect or adopt recipe build state",
	}
	cmd.AddCommand(&cobra.Command{
		Use:   "graph [recipe]",
		Short: "Print the recipe dependency tree",
		Args:  cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			recipe := ""
			if len(args) != 0 {
				recipe = args[0]
			}
			graph, err := buildsys.RecipeGraphText(ctx.workspace, recipe)
			if err != nil {
				return err
			}
			fmt.Print(graph)
			return nil
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "status",
		Short: "Show whether each recipe is up to date and why",
		Args:  cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("state:status")
			statuses, err := buildsys.RecipeStatuses(ctx.workspace)
			if err != nil {
				return err
			}
			rows := make([][]string, 0, len(statuses))
			for _, status := range statuses {
				state := "stale"
				if status.UpToDate {
					state = "up-to-date"
				}
				rows = append(rows, []string{status.ID, state, status.Reason})
			}
			ui.Table("Recipe state", []string{"RECIPE", "STATE", "REASON"}, rows)
			return nil
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "adopt [recipe...]",
		Short: "Record existing outputs as current without running recipes",
		Args:  cobra.ArbitraryArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("state:adopt")
			adopted, err := buildsys.AdoptRecipes(ctx.workspace, args)
			if err != nil {
				return err
			}
			for _, recipe := range adopted {
				fmt.Printf("  adopted %s\n", recipe)
			}
			ui.Done(fmt.Sprintf("adopted %d recipe(s); this asserts their existing outputs are up to date", len(adopted)))
			return nil
		},
	})
	return cmd
}
