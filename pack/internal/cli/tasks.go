package cli

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"capabilityos/pack/internal/bootfs"
	bootloaderlimine "capabilityos/pack/internal/bootloader/limine"
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

func fsckCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "fsck",
		Short: "Check filesystem images",
	}
	cmd.AddCommand(fsckRootfsCommand(ctx))
	return cmd
}

func fsckRootfsCommand(ctx *context) *cobra.Command {
	return &cobra.Command{
		Use:   "rootfs",
		Short: "Run e2fsck against the ext4 rootfs partition",
		RunE: func(cmd *cobra.Command, args []string) error {
			partition, ok := ctx.workspace.Disk.Partitions["rootfs"]
			if !ok {
				return fmt.Errorf("missing disk.partitions.rootfs")
			}
			if strings.ToLower(partition.Format) != "ext4" {
				return fmt.Errorf("rootfs fsck currently supports ext4, got %q", partition.Format)
			}
			diskPath := ctx.workspace.Path(ctx.workspace.Disk.Image)
			disk, err := os.Open(diskPath)
			if err != nil {
				return err
			}
			defer disk.Close()

			region, err := rootsync.OpenPartitionRegion(disk, partition.Index)
			if err != nil {
				return err
			}
			partitionBytes := (region.LastLBA - region.FirstLBA + 1) * 512
			if partitionBytes == 0 || partitionBytes > uint64(^uint(0)>>1) {
				return fmt.Errorf("invalid rootfs partition size: %d", partitionBytes)
			}

			tempDir := ctx.workspace.Path(ctx.workspace.Artifacts, "tmp")
			if err := os.MkdirAll(tempDir, 0o755); err != nil {
				return err
			}
			imagePath := filepath.Join(tempDir, "rootfs-fsck.ext4")
			image, err := os.Create(imagePath)
			if err != nil {
				return err
			}
			reader := io.NewSectionReader(
				disk,
				int64(region.FirstLBA*512),
				int64(partitionBytes))
			if _, err := io.Copy(image, reader); err != nil {
				_ = image.Close()
				return err
			}
			if err := image.Close(); err != nil {
				return err
			}
			defer os.Remove(imagePath)

			ui.Task("fsck:rootfs")
			fsck := exec.Command("e2fsck", "-fn", imagePath)
			output, err := fsck.CombinedOutput()
			if err != nil {
				return fmt.Errorf("e2fsck failed: %w\n%s", err, string(output))
			}
			ui.KeyValues("Rootfs fsck", [][2]string{
				{"disk", ctx.workspace.Rel(diskPath)},
				{"partition", fmt.Sprint(partition.Index)},
				{"filesystem", "ext4"},
				{"state", "clean"},
			})
			return nil
		},
	}
}

func qemuLimineCommand(ctx *context) *cobra.Command {
	var opts qemu.Options
	var prepare bool
	cmd := &cobra.Command{
		Use:   "qemu-limine",
		Short: "Boot QEMU through Limine BIOS",
		RunE: func(cmd *cobra.Command, args []string) error {
			if prepare {
				if err := prepareLimineBootImage(ctx, &opts); err != nil {
					return err
				}
			} else if opts.LimineImage == "" {
				opts.LimineImage = filepath.Join(ctx.workspace.Artifacts, "limine-boot.img")
			}
			ui.Task("qemu:limine")
			opts.Progress = ui.NewProgressReporter()
			result, err := qemu.Run(ctx.workspace, opts)
			if err != nil {
				return err
			}
			state := "finished"
			if result.DryRun {
				state = "dry-run"
			} else if result.Started {
				state = "started"
			}
			ui.KeyValues("QEMU Limine", [][2]string{
				{"state", state},
				{"firmware", firstNonEmpty(opts.Firmware, "bios")},
				{"image", ctx.workspace.Rel(opts.LimineImage)},
				{"log", ctx.workspace.Rel(result.Log)},
				{"host time log", ctx.workspace.Rel(result.HostTimeLog)},
				{"command", qemuCommandLine(result.Command)},
			})
			return nil
		},
	}
	cmd.Flags().BoolVar(&prepare, "prepare", false, "build Limine kernel/userland/bootfs image before booting")
	cmd.Flags().BoolVar(&opts.NoKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().BoolVar(&opts.NoNet, "no-net", false, "run QEMU without network")
	cmd.Flags().BoolVar(&opts.Fast, "fast", true, "reduce QEMU-side diagnostics")
	cmd.Flags().BoolVar(&opts.DryRun, "dry-run", false, "print the QEMU command without launching")
	cmd.Flags().StringVar(&opts.Memory, "memory", "2G", "QEMU memory size")
	cmd.Flags().IntVar(&opts.CPUs, "cpus", 4, "QEMU virtual CPU count (1..256)")
	cmd.Flags().StringVar(&opts.Display, "display", "none", "QEMU display backend")
	cmd.Flags().StringVar(&opts.Firmware, "firmware", "bios", "firmware path: bios or uefi")
	cmd.Flags().StringVar(&opts.LimineImage, "image", "", "Limine boot image path")
	cmd.Flags().StringArrayVar(&opts.ExtraArgs, "qemu-arg", nil, "append one raw argument to QEMU")
	return cmd
}

func prepareLimineBootImage(ctx *context, opts *qemu.Options) error {
	ui.Task("build:kernel:limine")
	kernel, err := buildsys.BuildKernel(ctx.workspace, buildsys.KernelOptions{
		StepOverride: "limine",
		Progress:     ui.NewProgressReporter(),
	})
	if err != nil {
		return err
	}
	ui.KeyValues("Kernel", [][2]string{
		{"step", kernel.Step},
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

	ui.Task("gen:manifests")
	generated, err := generateOrReuseManifests(ctx, userland.DirectoryArtifactsChanged == 0)
	if err != nil {
		return err
	}
	ui.KeyValues("Manifests", [][2]string{
		{"bootfs", ctx.workspace.Rel(generated.Outputs.Bootfs)},
		{"bootfs entries", fmt.Sprint(generated.Bootfs)},
	})

	ui.Task("build:bootfs")
	bootfsPath := ctx.workspace.Path(ctx.workspace.Artifacts, "BOOTFS.IMG")
	image, err := bootfs.BuildImageWithOptions(generated.Outputs.Bootfs, bootfsPath, bootfs.Options{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	ui.KeyValues("Bootfs", [][2]string{
		{"image", ctx.workspace.Rel(image.Path)},
		{"entries", fmt.Sprint(image.Entries)},
		{"bytes", fmt.Sprint(image.Bytes)},
	})

	initApp, ok := bootInitApp(ctx)
	if !ok {
		return fmt.Errorf("missing enabled boot app")
	}

	ui.Task("build:limine-image")
	limine, err := bootloaderlimine.BuildImageWithOptions(ctx.workspace, bootloaderlimine.Inputs{
		KernelELF:   kernel.Output,
		InitELF:     ctx.workspace.ArtifactPath(initApp),
		BootfsImage: image.Path,
	}, bootloaderlimine.Options{Progress: ui.NewProgressReporter()})
	if err != nil {
		return err
	}
	opts.LimineImage = limine.Image
	ui.KeyValues("Limine", [][2]string{
		{"image", ctx.workspace.Rel(limine.Image)},
		{"config", ctx.workspace.Rel(limine.Config)},
		{"limine", limine.Limine},
		{"bytes", fmt.Sprint(limine.FilesBytes)},
	})
	return nil
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
		Use:     "qemu",
		Aliases: []string{"run"},
		Short:   "Boot QEMU through Limine",
		RunE: func(cmd *cobra.Command, args []string) error {
			if opts.Prepare {
				if err := prepareLimineBootImage(ctx, &opts); err != nil {
					return err
				}
			} else if opts.LimineImage == "" {
				opts.LimineImage = filepath.Join(ctx.workspace.Artifacts, "limine-boot.img")
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
				{"firmware", firstNonEmpty(opts.Firmware, "bios")},
				{"image", ctx.workspace.Rel(opts.LimineImage)},
				{"log", ctx.workspace.Rel(result.Log)},
				{"host time log", ctx.workspace.Rel(result.HostTimeLog)},
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
	cmd.Flags().BoolVar(&opts.Fast, "fast", true, "reduce QEMU-side diagnostics")
	cmd.Flags().BoolVar(&opts.DryRun, "dry-run", false, "print the QEMU command without launching")
	cmd.Flags().StringVar(&opts.Memory, "memory", "2G", "QEMU memory size")
	cmd.Flags().IntVar(&opts.CPUs, "cpus", 4, "QEMU virtual CPU count (1..256)")
	cmd.Flags().StringVar(&opts.Display, "display", "none", "QEMU display backend")
	cmd.Flags().StringVar(&opts.Console, "console", "pty", "virtio console backend: pty or off")
	cmd.Flags().StringVar(&opts.Firmware, "firmware", "bios", "firmware path: bios or uefi")
	cmd.Flags().StringVar(&opts.LimineImage, "image", "", "Limine boot image path")
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
	cmd.Flags().DurationVar(&timeout, "timeout", 30*time.Second, "maximum time to wait for the smoke marker")
	cmd.Flags().StringVar(&marker, "marker", "[seed2_root] manifest scheduler done", "serial log marker required for success")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	return cmd
}

func profileSmokeCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "smoke",
		Short: "Run focused boot smoke profiles",
	}
	cmd.AddCommand(profileRunCommand(ctx, "memory", "memory", "[seed0root] libc alloc probe completed state=2 exit=0", 30*time.Second))
	cmd.AddCommand(profileRunCommand(ctx, "fs-write", "fs-write", "[seed0root] storage clean checkpoint status=0", 30*time.Second))
	cmd.AddCommand(profileRunCommand(ctx, "all", "all", "[seed0root] storage clean checkpoint status=0", 30*time.Second))
	return cmd
}

func profileBenchCommand(ctx *context) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "bench",
		Short: "Run focused boot benchmark profiles",
	}
	cmd.AddCommand(profileRunCommand(ctx, "runtime", "chibicc", "[seed0root] storage clean checkpoint status=0", 30*time.Second))
	cmd.AddCommand(profileRunCommand(ctx, "apk", "apk", "[seed0root] storage clean checkpoint status=0", 60*time.Second))
	return cmd
}

func profileRunCommand(ctx *context, use string, profile string, marker string, defaultTimeout time.Duration) *cobra.Command {
	var timeout time.Duration
	var noKVM bool
	cmd := &cobra.Command{
		Use:   use,
		Short: "Boot with seed0root profile " + profile,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runSeed0rootProfile(ctx, profile, marker, timeout, noKVM)
		},
	}
	cmd.Flags().DurationVar(&timeout, "timeout", defaultTimeout, "maximum time to wait for the profile marker")
	cmd.Flags().BoolVar(&noKVM, "no-kvm", false, "run QEMU without KVM")
	return cmd
}

func runSeed0rootProfile(ctx *context, profile string, marker string, timeout time.Duration, noKVM bool) (err error) {
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	if err := writeSeed0rootBootProfile(ctx, profile); err != nil {
		return err
	}
	defer func() {
		restoreErr := restoreSeed0rootBootProfile(ctx)
		if err == nil {
			err = restoreErr
		}
	}()

	var prepareOpts qemu.Options
	if err := prepareLimineBootImage(ctx, &prepareOpts); err != nil {
		return err
	}
	scratchImage, err := copySeed0rootProfileBootImage(ctx, profile, prepareOpts.LimineImage)
	if err != nil {
		return err
	}
	scratchDisk, err := createSeed0rootProfileDiskScratch(ctx, profile)
	if err != nil {
		return err
	}

	ui.Task("smoke:" + profile)
	result, smokeErr := qemu.Smoke(ctx.workspace, qemu.SmokeOptions{
		Timeout:     timeout,
		NoKVM:       noKVM,
		LimineImage: scratchImage,
		DiskImage:   scratchDisk,
		Marker:      marker,
		Progress:    ui.NewProgressReporter(),
	})
	state := "passed"
	if smokeErr != nil {
		state = "failed"
	}
	ui.KeyValues("Boot Profile", [][2]string{
		{"state", state},
		{"profile", profile},
		{"marker", result.Marker},
		{"timeout", result.Timeout.String()},
		{"serial", ctx.workspace.Rel(result.Serial)},
		{"qemu log", ctx.workspace.Rel(result.Log)},
	})
	return smokeErr
}

func writeSeed0rootBootProfile(ctx *context, profile string) error {
	path := ctx.workspace.Path(".artifacts", "seed0root_boot_profile.txt")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	if !strings.HasSuffix(profile, "\n") {
		profile += "\n"
	}
	return os.WriteFile(path, []byte(profile), 0o644)
}

func copySeed0rootProfileBootImage(ctx *context, profile string, source string) (string, error) {
	if source == "" {
		source = ctx.workspace.Path(ctx.workspace.Artifacts, "limine-boot.img")
	}
	cleanProfile := strings.NewReplacer("/", "_", "\\", "_", " ", "_").Replace(profile)
	dest := ctx.workspace.Path(ctx.workspace.Artifacts, "limine-boot-profile-"+cleanProfile+".img")
	data, err := os.ReadFile(source)
	if err != nil {
		return "", err
	}
	if err := os.WriteFile(dest, data, 0o644); err != nil {
		return "", err
	}
	return dest, nil
}

func createSeed0rootProfileDiskScratch(ctx *context, profile string) (string, error) {
	cleanProfile := strings.NewReplacer("/", "_", "\\", "_", " ", "_").Replace(profile)
	source := ctx.workspace.Path(ctx.workspace.Disk.Image)
	dest := ctx.workspace.Path(ctx.workspace.Artifacts, "disk-profile-"+cleanProfile+".img")
	_ = os.Remove(dest)
	cmd := exec.Command("cp", "--reflink=auto", "--sparse=always", source, dest)
	cmd.Dir = ctx.workspace.Root
	if out, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("disk scratch copy failed: %w: %s", err, strings.TrimSpace(string(out)))
	}
	return dest, nil
}

func restoreSeed0rootBootProfile(ctx *context) error {
	if err := writeSeed0rootBootProfile(ctx, ""); err != nil {
		return err
	}
	ui.Task("restore:seed0root-profile")
	userland, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{
		AppID:    "seed0root_boot_profile",
		Progress: ui.NewProgressReporter(),
	})
	if err != nil {
		return err
	}
	printUserland(userland)
	return runRootfsSync(ctx, userland, false)
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
			if len(result.Screendumps) != 0 {
				rows = append(rows, [2]string{"screendumps", strings.Join(result.Screendumps, ", ")})
			}
			ui.KeyValues("QEMU Test", rows)
			if err != nil {
				return err
			}
			return nil
		},
	}
	cmd.Flags().DurationVar(&opts.Timeout, "timeout", 30*time.Second, "maximum time to wait for boot and console expectations")
	cmd.Flags().StringVar(&opts.BootMarker, "boot-marker", "[seed0boot] hvc console spawn status=0", "serial log marker required before sending input")
	cmd.Flags().StringArrayVar(&opts.Send, "send", nil, "string to send to the TTY; repeatable")
	cmd.Flags().StringArrayVar(&opts.Expect, "expect", nil, "console output substring required for success; repeatable")
	cmd.Flags().StringVar(&opts.Python, "python", "", "python3 script for detailed TTY testing")
	cmd.Flags().BoolVar(&opts.NoKVM, "no-kvm", false, "run QEMU without KVM")
	cmd.Flags().IntVar(&opts.CPUs, "cpus", 4, "QEMU virtual CPU count (1..256)")
	cmd.Flags().StringVar(&opts.Display, "display", "none", "QEMU display backend")
	cmd.Flags().StringArrayVar(&opts.ScreendumpCheck, "screendump-check", nil, "capture at MARKER and require MARKER@X,Y,W,H=#RRGGBB[:TOLERANCE]; repeatable")
	cmd.Flags().StringVar(&opts.ScreendumpDevice, "screendump-device", "pachagpu", "QEMU display device id captured by screendump")
	cmd.Flags().StringArrayVar(&opts.InputSendEvent, "input-send-event", nil, "inject QMP input at MARKER using MARKER@key:a=down,rel:x=4,btn:left=up; repeatable")
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
	var noBuild bool
	cmd := &cobra.Command{
		Use:   "rootfs",
		Short: "Sync generated rootfs manifest into the FAT32 rootfs partition",
		RunE: func(cmd *cobra.Command, args []string) error {
			var userland buildsys.UserlandResult
			if !noBuild {
				ui.Task("build:userland")
				result, err := buildsys.BuildUserland(ctx.workspace, buildsys.UserlandOptions{Progress: ui.NewProgressReporter()})
				if err != nil {
					return err
				}
				printUserland(result)
				userland = result
			}
			return runRootfsSyncWithOptions(ctx, userland, force, noBuild)
		},
	}
	cmd.Flags().BoolVar(&force, "force", false, "force rootfs rewrite even when fingerprint is unchanged")
	cmd.Flags().BoolVar(&noBuild, "no-build", false, "sync rootfs from existing artifacts without building userland")
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
	return runRootfsSyncWithOptions(ctx, userland, force, false)
}

func runRootfsSyncWithOptions(ctx *context, userland buildsys.UserlandResult, force bool, noBuild bool) error {
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
	manifestReuse := !force && !noBuild && userland.DirectoryArtifactsChanged == 0 && !hasDirectoryPath(changedSources)
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
	bootfsPath := ctx.workspace.Path(ctx.workspace.Artifacts, "BOOTFS.IMG")
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

	if err := syncBootfsToLimineImage(ctx, image.Path); err != nil {
		return err
	}

	return nil
}

func syncBootfsToLimineImage(ctx *context, bootfsPath string) error {
	const limineFatPartitionByte = int64(2 * 1024 * 1024)
	liminePath := ctx.workspace.Path(ctx.workspace.Artifacts, "limine-boot.img")
	if _, err := os.Stat(liminePath); err != nil {
		if os.IsNotExist(err) {
			ui.KeyValues("Limine ESP", [][2]string{
				{"state", "skipped"},
				{"reason", "limine image missing"},
			})
			return nil
		}
		return err
	}

	initApp, ok := bootInitApp(ctx)
	if !ok {
		return fmt.Errorf("missing enabled boot app")
	}
	kernelStep := ctx.workspace.Kernel.Step
	if kernelStep == "" {
		kernelStep = "limine"
	}
	kernelPath := filepath.Join(ctx.workspace.Path(ctx.workspace.Kernel.Dir), "zig-out", "bin", kernelStep)
	if kernelStep == "limine" {
		kernelPath = filepath.Join(ctx.workspace.Path(ctx.workspace.Kernel.Dir), "zig-out", "bin", "limine", "pacha-kernel.elf")
	}
	initPath := ctx.workspace.ArtifactPath(initApp)
	configPath := ctx.workspace.Path(ctx.workspace.Artifacts, "limine.conf")
	for _, path := range []string{kernelPath, initPath, bootfsPath, configPath} {
		if info, err := os.Stat(path); err != nil {
			return err
		} else if info.Size() == 0 {
			return fmt.Errorf("empty limine input: %s", path)
		}
	}

	ui.Task("sync:limine-esp")
	tmpDir, err := os.MkdirTemp(ctx.workspace.Path(ctx.workspace.Artifacts), "limine-esp-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmpDir)

	mtoolsImage := fmt.Sprintf("%s@@%d", liminePath, limineFatPartitionByte)
	limineBios := filepath.Join(tmpDir, "limine-bios.sys")
	bootx64 := filepath.Join(tmpDir, "BOOTX64.EFI")
	if err := runExternal("mcopy", "-o", "-i", mtoolsImage, "::/limine-bios.sys", limineBios); err != nil {
		return err
	}
	if err := runExternal("mcopy", "-o", "-i", mtoolsImage, "::/EFI/BOOT/BOOTX64.EFI", bootx64); err != nil {
		return err
	}
	if err := runExternal("mformat", "-i", mtoolsImage, "-F", "-v", "LIMINEBOOT", "::"); err != nil {
		return err
	}
	if err := runExternal("mmd", "-i", mtoolsImage, "::/EFI"); err != nil {
		return err
	}
	if err := runExternal("mmd", "-i", mtoolsImage, "::/EFI/BOOT"); err != nil {
		return err
	}
	copies := [][2]string{
		{limineBios, "::/limine-bios.sys"},
		{bootx64, "::/EFI/BOOT/BOOTX64.EFI"},
		{kernelPath, "::/KERNEL.ELF"},
		{initPath, "::/INITAPP.ELF"},
		{bootfsPath, "::/BOOTFS.IMG"},
		{configPath, "::/limine.conf"},
	}
	for _, item := range copies {
		if err := runExternal("mcopy", "-o", "-i", mtoolsImage, item[0], item[1]); err != nil {
			return err
		}
	}
	ui.KeyValues("Limine ESP", [][2]string{
		{"state", "synced"},
		{"image", ctx.workspace.Rel(liminePath)},
		{"bootfs", ctx.workspace.Rel(bootfsPath)},
	})
	return nil
}

func runExternal(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	var combined bytes.Buffer
	cmd.Stdout = &combined
	cmd.Stderr = &combined
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s %s failed: %w\n%s", name, strings.Join(args, " "), err, combined.String())
	}
	return nil
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

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if value != "" {
			return value
		}
	}
	return ""
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
