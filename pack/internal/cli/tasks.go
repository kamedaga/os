package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"capabilityos/pack/internal/bootfs"
	"capabilityos/pack/internal/boottest"
	"capabilityos/pack/internal/buildsys"
	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/diskimage"
	"capabilityos/pack/internal/manifests"
	"capabilityos/pack/internal/qemu"
	"capabilityos/pack/internal/rootsync"
	"capabilityos/pack/internal/ui"
	"github.com/spf13/cobra"
)

func buildCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "build",
		Short: "Build artifacts",
	}
	cmd.AddCommand(buildKernelCommand(ctx))
	cmd.AddCommand(buildUserlandCommand(ctx))
	return cmd
}

func genCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "gen",
		Short: "Generate derived files",
	}
	cmd.AddCommand(genManifestsCommand(ctx))
	return cmd
}

func syncCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "sync",
		Short: "Sync images",
	}
	cmd.AddCommand(syncBootfsCommand(ctx))
	cmd.AddCommand(syncRootfsCommand(ctx))
	return cmd
}

func taskStub(task string, use string) *cobra.Command {
	return &cobra.Command{
		Use:   use,
		Short: task + " task",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task(task)
			ui.Pending("not implemented yet")
			return nil
		},
	}
}

func qemuCommand(ctx *context) *cobra.Command {
	var opts qemu.Options
	cmd := &cobra.Command{
		Use:   "qemu",
		Short: "Boot QEMU",
		RunE: func(cmd *cobra.Command, args []string) error {
			if opts.Prepare {
				ui.Task("build:kernel")
				kernel, err := buildsys.BuildKernel(ctx.workspace, buildsys.KernelOptions{Progress: ui.NewProgressReporter()})
				if err != nil {
					return err
				}
				kernelState := "built"
				if kernel.Skipped {
					kernelState = "up-to-date"
				}
				ui.KeyValues("Kernel", [][2]string{
					{"state", kernelState},
					{"output", ctx.workspace.Rel(kernel.Output)},
				})

				ui.Task("build:userland")
				userland, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{Progress: ui.NewProgressReporter()})
				if err != nil {
					return err
				}
				printUserland(userland)
				if err := runRootfsSync(ctx, userland, false); err != nil {
					return err
				}
				if err := runBootfsSync(ctx, false); err != nil {
					return err
				}
			}
			ui.Task("qemu")
			var result qemu.Result
			var err error
			opts.Progress = ui.NewProgressReporter()
			result, err = qemu.Run(ctx.workspace, opts)
			if err != nil {
				return err
			}
			state := "finished"
			if result.DryRun {
				state = "dry-run"
			} else if result.Started {
				state = "started"
			}
			ui.KeyValues("QEMU", [][2]string{
				{"state", state},
				{"log", ctx.workspace.Rel(result.Log)},
				{"vars", ctx.workspace.Rel(result.Vars)},
				{"command", qemuCommandLine(result.Command)},
			})
			if len(result.ConsoleCommand) > 0 {
				ui.KeyValues("Virtio Console", [][2]string{
					{"socket", result.ConsoleSocket},
					{"command", qemuCommandLine(result.ConsoleCommand)},
				})
			}
			return nil
		},
	}
	cmd.Flags().BoolVar(&opts.Prepare, "prepare", false, "build and sync boot/root filesystems before booting")
	cmd.Flags().BoolVar(&opts.NoBuild, "no-build", false, "deprecated no-op; qemu does not build by default")
	_ = cmd.Flags().MarkHidden("no-build")
	cmd.Flags().BoolVar(&opts.NewTerminal, "new-terminal", false, "open virtio-console in a new terminal window")
	cmd.Flags().BoolVar(&opts.NewTerminal, "terminal", false, "alias for --new-terminal")
	cmd.Flags().BoolVar(&opts.NoKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().BoolVar(&opts.NoNet, "no-net", false, "run QEMU without virtio-net")
	cmd.Flags().BoolVar(&opts.DryRun, "dry-run", false, "print the QEMU command without launching")
	cmd.Flags().StringVar(&opts.Memory, "memory", "2G", "QEMU memory size")
	cmd.Flags().StringVar(&opts.Display, "display", "gtk,grab-on-hover=off", "QEMU display backend")
	cmd.Flags().StringVar(&opts.Console, "console", "pty", "virtio console backend: pty or off")
	cmd.Flags().StringArrayVar(&opts.ExtraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	return cmd
}

func testCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "test",
		Short: "Run tests",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runSmokeTest(ctx, 45*time.Second, "[seed2_root] manifest scheduler done", false, false)
		},
	}
	cmd.AddCommand(smokeTestCommand(ctx))
	cmd.AddCommand(fdIPCSmokeTestCommand(ctx))
	cmd.AddCommand(deviceFDSmokeTestCommand(ctx))
	cmd.AddCommand(muslPachaOSSmokeTestCommand(ctx))
	cmd.AddCommand(seed0BootTestCommand(ctx))
	cmd.AddCommand(qemuTestCommand(ctx, "qemu"))
	return cmd
}

func smokeTestCommand(ctx *context) *cobra.Command {
	var timeout time.Duration
	var marker string
	var noKVM bool
	cmd := &cobra.Command{
		Use:   "smoke",
		Short: "Boot QEMU until the startup manifest completes",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runSmokeTest(ctx, timeout, marker, noKVM, false)
		},
	}
	cmd.Flags().DurationVar(&timeout, "timeout", 45*time.Second, "maximum time to wait for the smoke marker")
	cmd.Flags().StringVar(&marker, "marker", "[seed2_root] manifest scheduler done", "serial log marker required for success")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	return cmd
}

func fdIPCSmokeTestCommand(ctx *context) *cobra.Command {
	var timeout time.Duration
	var noKVM bool
	var extraArgs []string
	cmd := &cobra.Command{
		Use:   "fd-ipc-smoke",
		Short: "Boot QEMU with the minimal fd IPC init smoke",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("test:fd-ipc-smoke")
			result, err := boottest.RunFDIPCSmoke(ctx.workspace, boottest.FDIPCSmokeOptions{
				Timeout:   timeout,
				NoKVM:     noKVM,
				ExtraArgs: extraArgs,
			})
			state := "passed"
			if err != nil {
				state = "failed"
			}
			ui.KeyValues("FD IPC Smoke", [][2]string{
				{"state", state},
				{"marker", result.Marker},
				{"timeout", result.Timeout.String()},
				{"smoke elf", ctx.workspace.Rel(result.SmokeELF)},
				{"kernel", ctx.workspace.Rel(result.KernelEFI)},
				{"disk", ctx.workspace.Rel(result.Disk)},
				{"serial", ctx.workspace.Rel(result.Serial)},
				{"qemu log", ctx.workspace.Rel(result.Log)},
				{"restored", fmt.Sprint(result.Restored)},
			})
			if err != nil {
				return err
			}
			return nil
		},
	}
	cmd.Flags().DurationVar(&timeout, "timeout", 30*time.Second, "maximum time to wait for the fd IPC smoke marker")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().StringArrayVar(&extraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	return cmd
}

func deviceFDSmokeTestCommand(ctx *context) *cobra.Command {
	var timeout time.Duration
	var noKVM bool
	var extraArgs []string
	cmd := &cobra.Command{
		Use:   "device-fd-smoke",
		Short: "Boot QEMU with the minimal device fd init smoke",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("test:device-fd-smoke")
			result, err := boottest.RunDeviceFDSmoke(ctx.workspace, boottest.DeviceFDSmokeOptions{
				Timeout:   timeout,
				NoKVM:     noKVM,
				ExtraArgs: extraArgs,
			})
			state := "passed"
			if err != nil {
				state = "failed"
			}
			ui.KeyValues("Device FD Smoke", [][2]string{
				{"state", state},
				{"marker", result.Marker},
				{"timeout", result.Timeout.String()},
				{"smoke elf", ctx.workspace.Rel(result.SmokeELF)},
				{"kernel", ctx.workspace.Rel(result.KernelEFI)},
				{"disk", ctx.workspace.Rel(result.Disk)},
				{"serial", ctx.workspace.Rel(result.Serial)},
				{"qemu log", ctx.workspace.Rel(result.Log)},
				{"restored", fmt.Sprint(result.Restored)},
			})
			if err != nil {
				return err
			}
			return nil
		},
	}
	cmd.Flags().DurationVar(&timeout, "timeout", 30*time.Second, "maximum time to wait for the device fd smoke marker")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().StringArrayVar(&extraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	return cmd
}

func muslPachaOSSmokeTestCommand(ctx *context) *cobra.Command {
	var timeout time.Duration
	var noKVM bool
	var extraArgs []string
	cmd := &cobra.Command{
		Use:   "musl-pachaos-smoke",
		Short: "Boot QEMU with the PachaOS musl crt/syscall scaffold",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("test:musl-pachaos-smoke")
			result, err := boottest.RunMuslPachaOSSmoke(ctx.workspace, boottest.MuslPachaOSSmokeOptions{
				Timeout:   timeout,
				NoKVM:     noKVM,
				ExtraArgs: extraArgs,
			})
			state := "passed"
			if err != nil {
				state = "failed"
			}
			ui.KeyValues("Musl PachaOS Smoke", [][2]string{
				{"state", state},
				{"marker", result.Marker},
				{"timeout", result.Timeout.String()},
				{"smoke elf", ctx.workspace.Rel(result.SmokeELF)},
				{"kernel", ctx.workspace.Rel(result.KernelEFI)},
				{"disk", ctx.workspace.Rel(result.Disk)},
				{"serial", ctx.workspace.Rel(result.Serial)},
				{"qemu log", ctx.workspace.Rel(result.Log)},
				{"restored", fmt.Sprint(result.Restored)},
			})
			if err != nil {
				return err
			}
			return nil
		},
	}
	cmd.Flags().DurationVar(&timeout, "timeout", 30*time.Second, "maximum time to wait for the musl PachaOS smoke marker")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().StringArrayVar(&extraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	return cmd
}

func seed0BootTestCommand(ctx *context) *cobra.Command {
	var timeout time.Duration
	var noKVM bool
	var extraArgs []string
	var marker string
	var buildInit bool
	cmd := &cobra.Command{
		Use:   "seed0boot",
		Short: "Boot QEMU with the libc-based seed0boot init",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("test:seed0boot")
			result, err := boottest.RunSeed0Boot(ctx.workspace, boottest.Seed0BootOptions{
				Timeout:   timeout,
				NoKVM:     noKVM,
				ExtraArgs: extraArgs,
				Marker:    marker,
				BuildInit: buildInit,
			})
			state := "passed"
			if err != nil {
				state = "failed"
			}
			ui.KeyValues("Seed0boot", [][2]string{
				{"state", state},
				{"marker", result.Marker},
				{"timeout", result.Timeout.String()},
				{"seed elf", ctx.workspace.Rel(result.SeedELF)},
				{"kernel", ctx.workspace.Rel(result.KernelEFI)},
				{"disk", ctx.workspace.Rel(result.Disk)},
				{"serial", ctx.workspace.Rel(result.Serial)},
				{"qemu log", ctx.workspace.Rel(result.Log)},
				{"restored", fmt.Sprint(result.Restored)},
			})
			if err != nil {
				return err
			}
			return nil
		},
	}
	cmd.Flags().DurationVar(&timeout, "timeout", 30*time.Second, "maximum time to wait for the seed0boot marker")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().StringArrayVar(&extraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	cmd.Flags().StringVar(&marker, "marker", "", "serial log marker required for success")
	cmd.Flags().BoolVar(&buildInit, "build-init", false, "build seed0boot before launching QEMU")
	return cmd
}

func qemuTestCommand(ctx *context, use string) *cobra.Command {
	var opts qemu.TTYTestOptions
	cmd := &cobra.Command{
		Use:   use,
		Short: "Run QEMU TTY interaction test",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("test:qemu")
			opts.Progress = ui.NewProgressReporter()
			result, err := qemu.TTYTest(ctx.workspace, opts)
			state := "passed"
			if err != nil {
				state = "failed"
			}
			rows := [][2]string{
				{"state", state},
				{"boot marker", result.BootMarker},
				{"timeout", result.Timeout.String()},
				{"sent", fmt.Sprint(result.Sent)},
				{"expected", dash(strings.Join(result.Expected, ", "))},
				{"matched", dash(strings.Join(result.Matched, ", "))},
				{"serial", ctx.workspace.Rel(result.Serial)},
				{"console", ctx.workspace.Rel(result.Console)},
				{"qemu log", ctx.workspace.Rel(result.Log)},
				{"socket", result.ConsoleSocket},
			}
			if result.Python != "" {
				rows = append(rows,
					[2]string{"python", result.Python},
					[2]string{"python log", ctx.workspace.Rel(result.PythonLog)},
				)
			}
			ui.KeyValues("QEMU Test", rows)
			if err != nil {
				return err
			}
			return nil
		},
	}
	cmd.Flags().DurationVar(&opts.Timeout, "timeout", 60*time.Second, "maximum time to wait for boot and console expectations")
	cmd.Flags().StringVar(&opts.BootMarker, "boot-marker", "[seed2_root] manifest scheduler done", "serial log marker required before sending input")
	cmd.Flags().StringArrayVar(&opts.Send, "send", nil, "string to send to the TTY; repeatable")
	cmd.Flags().StringArrayVar(&opts.Expect, "expect", nil, "console output substring required for success; repeatable")
	cmd.Flags().StringVar(&opts.Python, "python", "", "python3 script for detailed TTY testing")
	cmd.Flags().BoolVar(&opts.NoKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().StringArrayVar(&opts.ExtraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	return cmd
}

func runSmokeTest(ctx *context, timeout time.Duration, marker string, noKVM bool, prepare bool) error {
	if prepare {
		if err := runBootPrepare(ctx); err != nil {
			return err
		}
	}
	ui.Task("test:smoke")
	result, err := qemu.Smoke(ctx.workspace, qemu.SmokeOptions{
		Timeout:  timeout,
		NoKVM:    noKVM,
		Marker:   marker,
		Progress: ui.NewProgressReporter(),
	})
	state := "passed"
	if err != nil {
		state = "failed"
	}
	ui.KeyValues("Smoke", [][2]string{
		{"state", state},
		{"marker", result.Marker},
		{"timeout", result.Timeout.String()},
		{"serial", ctx.workspace.Rel(result.Serial)},
		{"qemu log", ctx.workspace.Rel(result.Log)},
	})
	if err != nil {
		return err
	}
	return nil
}

func runBootPrepare(ctx *context) error {
	ui.Task("build:kernel")
	kernel, err := buildsys.BuildKernel(ctx.workspace, buildsys.KernelOptions{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	kernelState := "built"
	if kernel.Skipped {
		kernelState = "up-to-date"
	}
	ui.KeyValues("Kernel", [][2]string{
		{"state", kernelState},
		{"output", ctx.workspace.Rel(kernel.Output)},
	})

	ui.Task("build:userland")
	userland, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	printUserland(userland)
	if err := runRootfsSync(ctx, userland, false); err != nil {
		return err
	}
	return runBootfsSync(ctx, false)
}

func buildKernelCommand(ctx *context) *cobra.Command {
	var force bool
	cmd := &cobra.Command{
		Use:   "kernel",
		Short: "Build kernel EFI artifact",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("build:kernel")
			result, err := buildsys.BuildKernel(ctx.workspace, buildsys.KernelOptions{Force: force, Progress: ui.NewProgressReporter()})
			if err != nil {
				return err
			}
			state := "built"
			if result.Skipped {
				state = "up-to-date"
			}
			ui.KeyValues("Kernel", [][2]string{
				{"step", result.Step},
				{"dir", ctx.workspace.Kernel.Dir},
				{"state", state},
				{"output", ctx.workspace.Rel(result.Output)},
			})
			return nil
		},
	}
	cmd.Flags().BoolVar(&force, "force", false, "force zig build even when kernel output is up to date")
	return cmd
}

func buildUserlandCommand(ctx *context) *cobra.Command {
	var force bool
	var noRootfs bool
	cmd := &cobra.Command{
		Use:   "userland [app]",
		Short: "Build userland artifacts and sync rootfs",
		Args:  cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("build:userland")
			appID := ""
			if len(args) == 1 {
				appID = args[0]
			}
			result, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{
				AppID:    appID,
				Force:    force,
				Progress: ui.NewProgressReporter(),
			})
			if err != nil {
				return err
			}
			printUserland(result)
			if noRootfs {
				if err := markDirtyArtifacts(ctx, result.ChangedArtifacts); err != nil {
					return err
				}
				return nil
			}
			return runRootfsSync(ctx, result, false)
		},
	}
	cmd.Flags().BoolVar(&force, "force", false, "force file source rebuild commands")
	cmd.Flags().BoolVar(&noRootfs, "no-rootfs", false, "build userland artifacts without syncing rootfs")
	return cmd
}

func genManifestsCommand(ctx *context) *cobra.Command {
	return &cobra.Command{
		Use:   "manifests",
		Short: "Generate bootfs, rootfs, and startup manifests",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("gen:manifests")
			result, err := manifests.GenerateWithOptions(ctx.workspace, manifests.Options{Progress: ui.NewProgressReporter()})
			if err != nil {
				return err
			}
			ui.KeyValues("Manifests", [][2]string{
				{"bootfs", ctx.workspace.Rel(result.Outputs.Bootfs)},
				{"bootfs entries", fmt.Sprint(result.Bootfs)},
				{"rootfs", ctx.workspace.Rel(result.Outputs.Rootfs)},
				{"rootfs entries", fmt.Sprint(result.Rootfs)},
				{"startup", ctx.workspace.Rel(result.Outputs.Startup)},
				{"startup entries", fmt.Sprint(result.Startup)},
			})
			return nil
		},
	}
}

func syncRootfsCommand(ctx *context) *cobra.Command {
	var force bool
	cmd := &cobra.Command{
		Use:   "rootfs",
		Short: "Sync generated rootfs manifest into the FAT32 rootfs partition",
		RunE: func(cmd *cobra.Command, args []string) error {
			ui.Task("build:userland")
			userland, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{Progress: ui.NewProgressReporter()})
			if err != nil {
				return err
			}
			printUserland(userland)
			return runRootfsSync(ctx, userland, force)
		},
	}
	cmd.Flags().BoolVar(&force, "force", false, "force rootfs rewrite even when fingerprint is unchanged")
	return cmd
}

func syncBootfsCommand(ctx *context) *cobra.Command {
	var force bool
	var noBuild bool
	cmd := &cobra.Command{
		Use:   "bootfs",
		Short: "Build BOOTFS.IMG and sync the EFI system partition",
		RunE: func(cmd *cobra.Command, args []string) error {
			if !noBuild {
				ui.Task("build:userland")
				userland, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{Progress: ui.NewProgressReporter()})
				if err != nil {
					return err
				}
				printUserland(userland)
			}
			return runBootfsSync(ctx, force)
		},
	}
	cmd.Flags().BoolVar(&force, "force", false, "force ESP rewrite even when fingerprint is unchanged")
	cmd.Flags().BoolVar(&noBuild, "no-build", false, "sync bootfs from existing artifacts without building userland")
	return cmd
}

func printUserland(result buildsys.UserlandResult) {
	rows := [][2]string{
		{"rebuilt sources", fmt.Sprint(result.RebuiltSources)},
		{"copied artifacts", fmt.Sprint(result.CopiedArtifacts)},
		{"reused artifacts", fmt.Sprint(result.ReusedArtifacts)},
		{"changed dirs", fmt.Sprint(result.DirectoryArtifactsChanged)},
		{"skipped apps", fmt.Sprint(result.SkippedApps)},
	}
	if len(result.RebuiltApps) > 0 {
		rows = append(rows, [2]string{"rebuilt apps", strings.Join(result.RebuiltApps, ", ")})
	}
	ui.KeyValues("Userland", rows)
}

func runRootfsSync(ctx *context, userland buildsys.UserlandResult, force bool) error {
	ui.Task("init:disk")
	disk, err := diskimage.EnsureWithOptions(ctx.workspace, diskimage.Options{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	diskState := "exists"
	if disk.Created {
		diskState = "created"
	}
	ui.KeyValues("Disk", [][2]string{
		{"state", diskState},
		{"image", ctx.workspace.Rel(disk.Path)},
		{"size", fmt.Sprintf("%d MiB", disk.SizeMiB)},
		{"partitions", fmt.Sprint(disk.Partitions)},
	})

	ui.Task("gen:manifests")
	if err := markDirtyArtifacts(ctx, userland.ChangedArtifacts); err != nil {
		return err
	}
	changedSources, err := dirtyArtifacts(ctx)
	if err != nil {
		return err
	}
	rootfsForce := force
	manifestReuse := !force && userland.DirectoryArtifactsChanged == 0 && !hasDirectoryPath(changedSources)
	generated, err := generateOrReuseManifests(ctx, manifestReuse)
	if err != nil {
		return err
	}
	manifestState := "generated"
	if generated.Cached {
		manifestState = "up-to-date"
	}
	ui.KeyValues("Manifests", [][2]string{
		{"state", manifestState},
		{"rootfs", ctx.workspace.Rel(generated.Outputs.Rootfs)},
		{"rootfs entries", fmt.Sprint(generated.Rootfs)},
	})

	ui.Task("sync:rootfs")
	result, err := rootsync.SyncRootfs(ctx.workspace, generated.Outputs.Rootfs, rootsync.Options{
		Force:          rootfsForce,
		Full:           force,
		ChangedSources: changedSources,
		Progress:       ui.NewProgressReporter(),
	})
	if err != nil {
		return err
	}
	if err := clearDirtyArtifacts(ctx); err != nil {
		return err
	}
	state := "synced"
	if result.Skipped {
		state = "up-to-date"
	}
	ui.KeyValues("Rootfs", [][2]string{
		{"state", state},
		{"disk", ctx.workspace.Rel(result.Disk)},
		{"partition", fmt.Sprint(result.Partition)},
		{"filesystem", dash(result.Filesystem)},
		{"files", fmt.Sprint(result.Files)},
		{"dirs", fmt.Sprint(result.Dirs)},
		{"updated files", fmt.Sprint(result.Updated)},
		{"bytes", fmt.Sprint(result.Bytes)},
		{"clusters", fmt.Sprint(result.Clusters)},
	})
	return nil
}

func runBootfsSync(ctx *context, force bool) error {
	ui.Task("init:disk")
	disk, err := diskimage.EnsureWithOptions(ctx.workspace, diskimage.Options{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	diskState := "exists"
	if disk.Created {
		diskState = "created"
	}
	ui.KeyValues("Disk", [][2]string{
		{"state", diskState},
		{"image", ctx.workspace.Rel(disk.Path)},
		{"size", fmt.Sprintf("%d MiB", disk.SizeMiB)},
		{"partitions", fmt.Sprint(disk.Partitions)},
	})

	ui.Task("gen:manifests")
	generated, err := generateOrReuseManifests(ctx, !force)
	if err != nil {
		return err
	}
	manifestState := "generated"
	if generated.Cached {
		manifestState = "up-to-date"
	}
	ui.KeyValues("Manifests", [][2]string{
		{"state", manifestState},
		{"bootfs", ctx.workspace.Rel(generated.Outputs.Bootfs)},
		{"bootfs entries", fmt.Sprint(generated.Bootfs)},
	})

	ui.Task("build:bootfs")
	bootfsPath := ctx.workspace.Path(ctx.workspace.Kernel.Dir, "zig-out", "bin", "EFI", "BOOT", "BOOTFS.IMG")
	image, err := bootfs.BuildImageWithOptions(generated.Outputs.Bootfs, bootfsPath, bootfs.Options{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	imageState := "built"
	if image.Skipped {
		imageState = "up-to-date"
	}
	ui.KeyValues("Bootfs", [][2]string{
		{"state", imageState},
		{"image", ctx.workspace.Rel(image.Path)},
		{"entries", fmt.Sprint(image.Entries)},
		{"bytes", fmt.Sprint(image.Bytes)},
	})

	ui.Task("sync:bootfs")
	espManifest, err := writeESPManifest(ctx, bootfsPath)
	if err != nil {
		return err
	}
	result, err := rootsync.SyncBootfs(ctx.workspace, espManifest, rootsync.Options{Force: force, Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	state := "synced"
	if result.Skipped {
		state = "up-to-date"
	}
	ui.KeyValues("ESP", [][2]string{
		{"state", state},
		{"disk", ctx.workspace.Rel(result.Disk)},
		{"partition", fmt.Sprint(result.Partition)},
		{"filesystem", dash(result.Filesystem)},
		{"files", fmt.Sprint(result.Files)},
		{"dirs", fmt.Sprint(result.Dirs)},
		{"updated files", fmt.Sprint(result.Updated)},
		{"bytes", fmt.Sprint(result.Bytes)},
	})
	return nil
}

func writeESPManifest(ctx *context, bootfsPath string) (string, error) {
	bootx64 := ctx.workspace.Path(ctx.workspace.Kernel.Dir, "zig-out", "bin", "EFI", "BOOT", "BOOTX64.EFI")
	initApp, ok := bootInitApp(ctx)
	if !ok {
		return "", fmt.Errorf("missing enabled boot app")
	}
	initAppPath := ctx.workspace.ArtifactPath(initApp)
	for _, path := range []string{bootx64, initAppPath, bootfsPath} {
		info, err := os.Stat(path)
		if err != nil {
			return "", err
		}
		if info.Size() == 0 {
			return "", fmt.Errorf("empty ESP source: %s", path)
		}
	}
	path := ctx.workspace.Path(ctx.workspace.Manifests.Dir, "esp.generated.txt")
	content := fmt.Sprintf("# Generated by pacgo. DO NOT EDIT.\n/EFI/BOOT/BOOTX64.EFI=%s\n/EFI/BOOT/INITAPP.ELF=%s\n/EFI/BOOT/BOOTFS.IMG=%s\n", filepath.ToSlash(bootx64), filepath.ToSlash(initAppPath), filepath.ToSlash(bootfsPath))
	if existing, err := os.ReadFile(path); err == nil && string(existing) == content {
		return path, nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return "", err
	}
	return path, os.WriteFile(path, []byte(content), 0o644)
}

func bootInitApp(ctx *context) (config.App, bool) {
	for _, app := range ctx.workspace.Apps() {
		if app.Role == "boot" && !ctx.workspace.Skipped(app) {
			return app, true
		}
	}
	return config.App{}, false
}

func generateOrReuseManifests(ctx *context, allowReuse bool) (manifests.Result, error) {
	if allowReuse {
		if result, ok, err := manifests.ExistingIfFreshWithOptions(ctx.workspace, manifests.Options{Progress: ui.NewProgressReporter()}); err != nil {
			return manifests.Result{}, err
		} else if ok {
			return result, nil
		}
	}
	return manifests.GenerateWithOptions(ctx.workspace, manifests.Options{Progress: ui.NewProgressReporter()})
}

func dirtyArtifactsPath(ctx *context) string {
	return ctx.workspace.Path(ctx.workspace.State, "dirty-artifacts.txt")
}

func markDirtyArtifacts(ctx *context, paths []string) error {
	if len(paths) == 0 {
		return nil
	}
	existing, err := dirtyArtifacts(ctx)
	if err != nil {
		return err
	}
	seen := map[string]bool{}
	for _, path := range existing {
		seen[path] = true
	}
	for _, path := range paths {
		if path == "" {
			continue
		}
		abs, err := filepath.Abs(path)
		if err != nil {
			return err
		}
		seen[filepath.Clean(abs)] = true
	}
	out := make([]string, 0, len(seen))
	for path := range seen {
		out = append(out, path)
	}
	sort.Strings(out)
	if err := os.MkdirAll(filepath.Dir(dirtyArtifactsPath(ctx)), 0o755); err != nil {
		return err
	}
	return os.WriteFile(dirtyArtifactsPath(ctx), []byte(strings.Join(out, "\n")+"\n"), 0o644)
}

func dirtyArtifacts(ctx *context) ([]string, error) {
	data, err := os.ReadFile(dirtyArtifactsPath(ctx))
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var out []string
	seen := map[string]bool{}
	for _, line := range strings.Split(string(data), "\n") {
		path := strings.TrimSpace(line)
		if path == "" || seen[path] {
			continue
		}
		seen[path] = true
		out = append(out, path)
	}
	sort.Strings(out)
	return out, nil
}

func clearDirtyArtifacts(ctx *context) error {
	err := os.Remove(dirtyArtifactsPath(ctx))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func hasDirectoryPath(paths []string) bool {
	for _, path := range paths {
		info, err := os.Stat(path)
		if err == nil && info.IsDir() {
			return true
		}
	}
	return false
}

func qemuCommandLine(args []string) string {
	if len(args) == 0 {
		return ""
	}
	out := ""
	for i, arg := range args {
		if i > 0 {
			out += " "
		}
		if arg == "" || containsShellSpecial(arg) {
			out += "'" + strings.ReplaceAll(arg, "'", "'\\''") + "'"
		} else {
			out += arg
		}
	}
	return out
}

func containsShellSpecial(value string) bool {
	for _, ch := range value {
		if ch == ' ' || ch == '\t' || ch == '\n' || ch == '\'' || ch == '"' || ch == '$' || ch == '&' || ch == ';' || ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '|' {
			return true
		}
	}
	return false
}
