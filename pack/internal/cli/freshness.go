package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"capabilityos/pack/internal/bootfs"
	bootloaderlimine "capabilityos/pack/internal/bootloader/limine"
	"capabilityos/pack/internal/buildsys"
	"capabilityos/pack/internal/manifests"
	"capabilityos/pack/internal/qemu"
	"capabilityos/pack/internal/rootsync"
	"capabilityos/pack/internal/ui"
)

// prepareSelection names the build stages a --prepare run rebuilds. The
// packaging stages after them (rootfs, manifests, bootfs, Limine image) always
// run; each one is skipped by its own record when nothing changed.
type prepareSelection struct {
	Kernel   bool
	Userland bool
	Apps     []string
}

func allStages() prepareSelection {
	return prepareSelection{Kernel: true, Userland: true}
}

func parseOnly(values []string) (prepareSelection, error) {
	if len(values) == 0 {
		return allStages(), nil
	}
	var selection prepareSelection
	seenApps := make(map[string]bool)
	for _, value := range values {
		switch {
		case value == "kernel":
			selection.Kernel = true
		case value == "userland":
			selection.Userland = true
			selection.Apps = nil
		case strings.HasPrefix(value, "userland:"):
			if selection.Userland && len(selection.Apps) == 0 {
				return prepareSelection{}, fmt.Errorf("--only userland already selects every app; drop %q", value)
			}
			for _, app := range strings.Split(strings.TrimPrefix(value, "userland:"), ",") {
				if app == "" {
					return prepareSelection{}, fmt.Errorf("empty app in --only %q", value)
				}
				if !seenApps[app] {
					seenApps[app] = true
					selection.Apps = append(selection.Apps, app)
				}
			}
			selection.Userland = true
		default:
			return prepareSelection{}, fmt.Errorf("unknown --only stage %q (want kernel, userland or userland:<app>[,<app>...])", value)
		}
	}
	return selection, nil
}

type freshnessIssue struct {
	Stage  string
	Reason string
}

type freshnessScope struct {
	Kernel    bool
	Userland  bool
	Packaging bool
	Rootfs    bool
}

const maxListedStaleRecipes = 8

// bootFreshness reports, without building or writing images, which parts of
// the boot path no longer match their sources.
func bootFreshness(ctx *context, imagePath string, scope freshnessScope) ([]freshnessIssue, error) {
	var issues []freshnessIssue
	if scope.Kernel {
		reason, err := buildsys.KernelFreshness(ctx.workspace, "limine")
		if err != nil {
			return nil, err
		}
		if reason != "" {
			issues = append(issues, freshnessIssue{"kernel", reason})
		}
	}
	if scope.Userland {
		statuses, err := buildsys.ActiveRecipeStatuses(ctx.workspace)
		if err != nil {
			return nil, err
		}
		var stale []buildsys.RecipeStatus
		for _, status := range statuses {
			if !status.UpToDate {
				stale = append(stale, status)
			}
		}
		for index, status := range stale {
			if index == maxListedStaleRecipes {
				issues = append(issues, freshnessIssue{"userland", fmt.Sprintf("%d more stale recipes (see ./pacgo state status)", len(stale)-index)})
				break
			}
			issues = append(issues, freshnessIssue{"userland", status.ID + ": " + status.Reason})
		}
	}
	if !scope.Packaging {
		return issues, nil
	}
	diskPath := ctx.workspace.Path(ctx.workspace.Disk.Image)
	if _, err := os.Stat(diskPath); os.IsNotExist(err) {
		// The bootfs storage policy is derived from the disk's filesystem.
		return append(issues, freshnessIssue{"disk", ctx.workspace.Rel(diskPath) + " missing"}), nil
	} else if err != nil {
		return nil, err
	}
	generated, fresh, err := manifests.ExistingIfFresh(ctx.workspace)
	if err != nil {
		return nil, err
	}
	if !fresh {
		// Rootfs and bootfs are derived from the manifests; comparing them with
		// outdated manifests would be meaningless.
		return append(issues, freshnessIssue{"manifests", "generated manifests are out of date; rootfs and bootfs follow them"}), nil
	}
	if scope.Rootfs {
		pending, err := dirtyArtifacts(ctx)
		if err != nil {
			return nil, err
		}
		if len(pending) != 0 {
			issues = append(issues, freshnessIssue{"rootfs", fmt.Sprintf("%d changed artifacts not yet synced", len(pending))})
		} else {
			synced, err := rootsync.PartitionUpToDate(ctx.workspace, "rootfs", generated.Outputs.Rootfs)
			if err != nil {
				return nil, err
			}
			if !synced {
				issues = append(issues, freshnessIssue{"rootfs", "disk rootfs does not match the rootfs manifest"})
			}
		}
	}
	bootfsPath := ctx.workspace.Path(ctx.workspace.Artifacts, "BOOTFS.IMG")
	current, err := bootfs.ImageUpToDate(generated.Outputs.Bootfs, bootfsPath, bootfsOptions(ctx, false))
	if err != nil {
		return nil, err
	}
	if !current {
		issues = append(issues, freshnessIssue{"bootfs", ctx.workspace.Rel(bootfsPath) + " does not match the bootfs manifest"})
	}
	initApp, ok := bootInitApp(ctx)
	if !ok {
		return nil, fmt.Errorf("missing enabled boot app")
	}
	if !filepath.IsAbs(imagePath) {
		imagePath = ctx.workspace.Path(imagePath)
	}
	reasons, err := bootloaderlimine.ImageFreshness(ctx.workspace, imagePath, bootloaderlimine.Inputs{
		KernelELF:   buildsys.KernelOutputPath(ctx.workspace, "limine"),
		InitELF:     ctx.workspace.ArtifactPath(initApp),
		BootfsImage: bootfsPath,
	})
	if err != nil {
		return nil, err
	}
	for _, reason := range reasons {
		issues = append(issues, freshnessIssue{"limine", reason})
	}
	return issues, nil
}

// reportFreshness prints the issues and, when strict, turns them into an
// error before QEMU starts.
func reportFreshness(issues []freshnessIssue, strict bool, hint string) error {
	if len(issues) == 0 {
		return nil
	}
	ui.Task("check:boot-freshness")
	for _, issue := range issues {
		ui.Pending(issue.Stage + ": " + issue.Reason)
	}
	ui.Pending(hint)
	if strict {
		return fmt.Errorf("boot inputs are stale (%d issues); refusing to boot because --require-fresh is set", len(issues))
	}
	return nil
}

func bootfsOptions(ctx *context, withProgress bool) bootfs.Options {
	opts := bootfs.Options{Storage: &bootfs.StorageOptions{
		DiskPath:      ctx.workspace.Path(ctx.workspace.Disk.Image),
		RootPartition: ctx.workspace.Disk.Partitions["rootfs"].Index,
		RAMBytes:      ctx.workspace.Storage.SandboxRAMMiB << 20,
		QueueSize:     ctx.workspace.Storage.QueueSize,
		ArenaBytes:    ctx.workspace.Storage.QueueArenaMiB << 20,
	}}
	if withProgress {
		opts.Progress = ui.NewProgressReporter()
	}
	return opts
}

func requireFile(path string, what string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("%s missing (%s): %w", what, path, err)
	}
	if info.Size() == 0 {
		return fmt.Errorf("%s is empty: %s", what, path)
	}
	return nil
}

// prepareOrCheckBoot either rebuilds the selected stages (--prepare) or, for a
// plain boot, checks without building that the image still matches its
// sources. Stages left out by --only are checked the same way.
func prepareOrCheckBoot(ctx *context, opts *qemu.Options, prepare bool, only []string, requireFresh bool) error {
	if !prepare {
		if len(only) != 0 {
			return fmt.Errorf("--only selects stages for --prepare")
		}
		if opts.LimineImage == "" {
			opts.LimineImage = filepath.Join(ctx.workspace.Artifacts, "limine-boot.img")
		}
		issues, err := bootFreshness(ctx, opts.LimineImage, freshnessScope{Kernel: true, Userland: true, Packaging: true, Rootfs: !opts.NoStorage})
		if err != nil {
			return err
		}
		return reportFreshness(issues, requireFresh, "booting the existing image; run with --prepare to rebuild")
	}
	selection, err := parseOnly(only)
	if err != nil {
		return err
	}
	if err := prepareLimineBootImage(ctx, opts, selection); err != nil {
		return err
	}
	if selection.Kernel && selection.Userland && len(selection.Apps) == 0 {
		return nil
	}
	issues, err := bootFreshness(ctx, opts.LimineImage, freshnessScope{Kernel: !selection.Kernel, Userland: !selection.Userland || len(selection.Apps) != 0})
	if err != nil {
		return err
	}
	return reportFreshness(issues, requireFresh, "stages outside --only were not rebuilt")
}
