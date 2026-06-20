package cli

import (
	"fmt"
	"strings"
	"time"

	"capabilityos/pack/internal/runner"
	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

func runnerCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "runner",
		Short: "Run a narrow external command runner",
	}
	cmd.AddCommand(runnerServeCommand(ctx))
	cmd.AddCommand(runnerRunCommand(ctx))
	cmd.AddCommand(runnerStatusCommand(ctx))
	cmd.AddCommand(runnerCleanCommand(ctx))
	return cmd
}

func runnerServeCommand(ctx *context) *cobra.Command {
	var dir string
	var once bool
	cmd := &cobra.Command{
		Use:   "serve",
		Short: "Serve fixed KVM/QEMU runner requests",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("runner:serve")
			base := dir
			if base == "" {
				base = runner.DefaultDir(ctx.workspace)
			}
			ui.KeyValues("Runner", [][2]string{
				{"state", "serving"},
				{"dir", ctx.workspace.Rel(base)},
				{"actions", "build-kernel, qemu-dry-run, smoke"},
			})
			return runner.Serve(ctx.workspace, runner.ServeOptions{
				Dir:  dir,
				Once: once,
				OnResult: func(result runner.Result) {
					state := "passed"
					if !result.OK {
						state = "failed"
					}
					fmt.Printf("runner: processed id=%s action=%s state=%s exit=%d\n",
						result.ID,
						result.Action,
						state,
						result.ExitCode,
					)
				},
			})
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "runner queue directory")
	cmd.Flags().BoolVar(&once, "once", false, "process one pending request and exit")
	return cmd
}

func runnerRunCommand(ctx *context) *cobra.Command {
	var dir string
	var timeout time.Duration
	var marker string
	var wait time.Duration
	cmd := &cobra.Command{
		Use:   "run ACTION",
		Short: "Submit a fixed runner request and wait for the result",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			action := runner.Action(args[0])
			runOptions := runner.RunOptions{
				Dir:    dir,
				Action: action,
				Wait:   wait,
			}
			if action == runner.ActionSmoke {
				runOptions.Timeout = timeout
				runOptions.Marker = marker
			}
			ui.Task("runner:run")
			result, err := runner.SubmitAndWait(ctx.workspace, runOptions)
			if err != nil {
				return err
			}
			state := "passed"
			if !result.OK {
				state = "failed"
			}
			ui.KeyValues("Runner Result", [][2]string{
				{"state", state},
				{"id", result.ID},
				{"action", string(result.Action)},
				{"exit", fmt.Sprint(result.ExitCode)},
			})
			printRunnerOutput("stdout", result.Stdout)
			printRunnerOutput("stderr", result.Stderr)
			if !result.OK {
				if result.Error != "" {
					return fmt.Errorf("runner command failed: %s", result.Error)
				}
				return fmt.Errorf("runner command failed")
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "runner queue directory")
	cmd.Flags().DurationVar(&timeout, "timeout", 60*time.Second, "smoke timeout")
	cmd.Flags().StringVar(&marker, "marker", "[seed0root] ready", "smoke marker")
	cmd.Flags().DurationVar(&wait, "wait", 10*time.Minute, "maximum time to wait for a runner result")
	return cmd
}

func runnerStatusCommand(ctx *context) *cobra.Command {
	var dir string
	cmd := &cobra.Command{
		Use:   "status",
		Short: "Show runner queue status",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("runner:status")
			status, err := runner.GetStatus(ctx.workspace, dir)
			if err != nil {
				return err
			}
			ui.KeyValues("Runner", [][2]string{
				{"dir", ctx.workspace.Rel(status.Dir)},
				{"pending requests", fmt.Sprint(status.Requests)},
				{"completed request leftovers", fmt.Sprint(status.CompletedRequests)},
				{"stored results", fmt.Sprint(status.Results)},
			})
			return nil
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "runner queue directory")
	return cmd
}

func runnerCleanCommand(ctx *context) *cobra.Command {
	var dir string
	var olderThan time.Duration
	cmd := &cobra.Command{
		Use:   "clean",
		Short: "Remove old runner requests and results",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("runner:clean")
			result, err := runner.Cleanup(ctx.workspace, runner.CleanupOptions{
				Dir:       dir,
				OlderThan: olderThan,
			})
			if err != nil {
				return err
			}
			ui.KeyValues("Runner Cleanup", [][2]string{
				{"completed requests removed", fmt.Sprint(result.CompletedRequestsRemoved)},
				{"old requests removed", fmt.Sprint(result.RequestsRemoved)},
				{"old results removed", fmt.Sprint(result.ResultsRemoved)},
			})
			return nil
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "runner queue directory")
	cmd.Flags().DurationVar(&olderThan, "older-than", 24*time.Hour, "remove runner files older than this duration")
	return cmd
}

func printRunnerOutput(label string, output string) {
	output = strings.TrimSpace(output)
	if output == "" {
		return
	}
	fmt.Printf("\n[%s]\n%s\n", label, output)
}
