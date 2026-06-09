package cli

import (
	"fmt"

	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

func buildCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "build",
		Short: "Build artifacts",
	}
	cmd.AddCommand(taskStub("build:kernel", "kernel"))
	cmd.AddCommand(taskStub("build:userland", "userland [app]"))
	return cmd
}

func genCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "gen",
		Short: "Generate derived files",
	}
	cmd.AddCommand(taskStub("gen:manifests", "manifests"))
	return cmd
}

func syncCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "sync",
		Short: "Sync images",
	}
	cmd.AddCommand(taskStub("sync:bootfs", "bootfs"))
	cmd.AddCommand(taskStub("sync:rootfs", "rootfs"))
	return cmd
}

func taskStub(task string, use string) *cobra.Command {
	return &cobra.Command{
		Use:   use,
		Short: task + " task",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task(task)
			fmt.Println("not implemented yet")
			return nil
		},
	}
}
