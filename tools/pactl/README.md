# pactl

Host-side build and setup orchestrator scaffold for CapabilityOS.

## Quick Start

Normal development loop:

```powershell
pactl plan
pactl setup
pactl run
```

- `pactl setup` is the normal incremental path.
- `pactl setup` now forces a userland rebuild before syncing, including kinds that are otherwise skipped during normal incremental `build userland`.
- `pactl setup full` recreates `disk.img` and rebuilds the runtime image from scratch.
- `pactl run --timed` records boot timing.
- `pactl run --no-kvm` disables KVM.

When you need to refresh a specific userland app explicitly:

```powershell
pactl build userland <app-id> --fresh
```

## Mental Model

- `pactl.conf` describes the workspace.
- `userland/apps/<app-id>/app.conf` describes each app.
- `.artifacts/` holds generated manifests, images, and cached build outputs.
- `kernel/build.zig` is for kernel boot artifacts, while `pactl` owns userland, manifests, disk sync, and QEMU launch.

## Commands

- `pactl plan`
- `pactl config path`
- `pactl app list`
- `pactl app show <id>`
- `pactl build userland [id] [--fresh]`
- `pactl disk ensure [--fresh]`
- `pactl setup [diff|full]`
- `pactl sync rootfs`
- `pactl sync bootfs`
- `pactl run [--timed] [--no-kvm] [--dry-run]`
- `pactl gen manifests`

This crate is intentionally dependency-free for the first scaffold so it can
build in restricted environments without fetching crates.
