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
	if helpRequested(os.Args[1:]) {
		return 0
	}
	ui.Success(ctx.started, label(os.Args[1:]))
	return 0
}

func rootCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "pacgo",
		Short: "CapabilityOS build and packaging tool",
		Long: `pacgo is the fast local build runner for CapabilityOS.

Nix owns the reproducible WSL Linux toolchain. pacgo owns the day-to-day build,
packaging, image sync, QEMU, and test tasks.`,
		SilenceUsage:      true,
		SilenceErrors:     true,
		CompletionOptions: cobra.CompletionOptions{DisableDefaultCmd: true},
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
	cmd.SetHelpTemplate(helpTemplate)
	cmd.AddCommand(planCommand(ctx))
	cmd.AddCommand(appCommand(ctx))
	cmd.AddCommand(buildCommand(ctx))
	cmd.AddCommand(genCommand(ctx))
	cmd.AddCommand(syncCommand(ctx))
	cmd.AddCommand(runnerCommand(ctx))
	cmd.AddCommand(stubCommand(ctx, "image", "Create or update disk image"))
	cmd.AddCommand(qemuCommand(ctx))
	cmd.AddCommand(qemuTestCommand(ctx, "qemu-test"))
	cmd.AddCommand(testCommand(ctx))
	cmd.AddCommand(stubCommand(ctx, "all", "Run the full local pipeline"))
	cmd.AddCommand(stubCommand(ctx, "ci", "Run the CI pipeline"))
	return cmd
}

func label(args []string) string {
	if len(args) == 0 {
		return "plan"
	}
	return strings.Join(args, ":")
}

func helpRequested(args []string) bool {
	for _, arg := range args {
		if arg == "-h" || arg == "--help" || arg == "help" {
			return true
		}
	}
	return false
}

func stubCommand(ctx *context, name string, short string) *cobra.Command {
	return &cobra.Command{
		Use:   name,
		Short: short,
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task(name)
			ui.Pending("not implemented yet")
			return nil
		},
	}
}

const helpTemplate = `{{with (or .Long .Short)}}{{. | trimTrailingWhitespaces}}

{{end}}Usage:
  {{.UseLine}}
{{if .HasAvailableSubCommands}}
Tasks:
{{range .Commands}}{{if (or .IsAvailableCommand (eq .Name "help"))}}  {{rpad .Name .NamePadding }} {{.Short}}
{{end}}{{end}}{{end}}{{if .HasAvailableLocalFlags}}
Flags:
{{.LocalFlags.FlagUsages | trimTrailingWhitespaces}}
{{end}}{{if .HasAvailableInheritedFlags}}
Global Flags:
{{.InheritedFlags.FlagUsages | trimTrailingWhitespaces}}
{{end}}`
