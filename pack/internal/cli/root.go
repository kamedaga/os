package cli

import (
	"fmt"
	"os"
	"strings"
	"time"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

type context struct {
	root      string
	workspace *config.Workspace
	started   time.Time
}

func Execute() int {
	ctx := &context{started: time.Now()}
	cmd := rootCommand(ctx)
	if err := cmd.Execute(); err != nil {
		ui.Failed(ctx.started, label(os.Args[1:]))
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	ui.Success(ctx.started, label(os.Args[1:]))
	return 0
}

func rootCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:           "pacgo",
		Short:         "CapabilityOS build and packaging tool",
		SilenceUsage:  true,
		SilenceErrors: true,
		PersistentPreRunE: func(cmd *cobra.Command, args []string) error {
			if cmd.CommandPath() == "pacgo help" {
				return nil
			}
			root, err := config.FindRoot(".")
			if err != nil {
				return err
			}
			workspace, err := config.Load(root)
			if err != nil {
				return err
			}
			ctx.root = root
			ctx.workspace = workspace
			return nil
		},
		RunE: func(cmd *cobra.Command, args []string) error {
			return runPlan(ctx)
		},
	}
	cmd.AddCommand(planCommand(ctx))
	cmd.AddCommand(appCommand(ctx))
	cmd.AddCommand(buildCommand(ctx))
	cmd.AddCommand(genCommand(ctx))
	cmd.AddCommand(syncCommand(ctx))
	cmd.AddCommand(stubCommand(ctx, "image"))
	cmd.AddCommand(stubCommand(ctx, "qemu"))
	cmd.AddCommand(stubCommand(ctx, "test"))
	cmd.AddCommand(stubCommand(ctx, "all"))
	cmd.AddCommand(stubCommand(ctx, "ci"))
	return cmd
}

func label(args []string) string {
	if len(args) == 0 {
		return "plan"
	}
	return strings.Join(args, ":")
}

func stubCommand(ctx *context, name string) *cobra.Command {
	return &cobra.Command{
		Use:   name,
		Short: name + " task",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task(name)
			fmt.Println("not implemented yet")
			return nil
		},
	}
}
