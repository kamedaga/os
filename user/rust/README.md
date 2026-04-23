# Rust Workspace

`user/rust/` は CapabilityOS の Rust runtime / substrate workspace です。

runtime 実装と public facade は `user/rust/` に残し、
demo app は必要に応じて `userland/` 側へ置きます。

## 1. Runtime substrate

- `rt_core`
  - `_start`、panic、syscall、stdio / exit bootstrap、共通 stack extension
- `rt_alloc`
  - global allocator
- `rt_handle`
  - typed handle、spawn builder、bootstrap handoff
- `rt_io`
  - rooted fs、clock、random、persistent_fs client

## 2. Public facade

- `cap_std`
  - CapabilityOS-native な std 風 API
  - `io`, `fs`, `time`, `path`, `env`, `process`

## 3. Wasmtime support

- `rt_wasmtime_platform`
  - Wasmtime custom platform shim
- `wasmtime_config`
  - host-side builder と guest-side loader が共有する target / tunables
- `wasmtime_host`
  - CapabilityOS 上の Wasmtime embedder helper

## 4. Demo crates

### Bring-up / runtime demos

- `rust_hello`
- `rust_fs_demo`
- `rust_fs_mut_demo`
- `rust_open_exec_demo`
- `rust_spawn_demo`

### `cap_std` demos

- `cap_std_child_exit_demo`
- `cap_std_exec_demo`
- `cap_std_fs_demo`
- `cap_std_wait_demo`

### Wasmtime demos

Wasmtime demo crate 本体は `userland/rust/` に置きます。

- `userland/rust/rust_wasmtime_platform_demo`
- `userland/rust/rust_wasmtime_host_demo`
- `userland/rust/rust_wasmtime_loader_demo`
- `userland/rust/rust_wasmtime_hostcall_demo`
- `userland/rust/rust_wasmtime_memory_demo`
- `userland/rust/rust_wasmtime_writeback_demo`
- `userland/rust/rust_wasmtime_trap_demo`

## Publish layout

Rust app の publish 定義は `userland/apps/<app-id>/app.conf` にあります。

例:

- `userland/apps/rust_wasmtime_loader_demo/app.conf`
- `userland/apps/rust_wasmtime_hostcall_demo/app.conf`

Wasmtime module artifact 自体は file app として publish しています。

- `userland/apps/wasmtime_minimal_module/app.conf`
- `userland/apps/wasmtime_hostcall_module/app.conf`

artifact の生成元は `tools/wasmtime_artifact_builder/` です。
artifact builder と guest loader の Wasmtime config は `wasmtime_config` で共有します。

## よく使うコマンド

workspace build:

```powershell
cargo build --manifest-path user/rust/Cargo.toml --release --target x86_64-unknown-none --target-dir .artifacts/cargo-target
```

単体 publish:

```powershell
.\pactl.cmd build userland rust_wasmtime_loader_demo
.\pactl.cmd build userland rust_wasmtime_hostcall_demo
```

disk 反映:

```powershell
.\pactl.cmd setup diff
```

EFI build:

```powershell
cd kernel
zig build efi
```

## 現在の Wasmtime 状態

現在の Wasmtime 実行は native code ではなく `pulley64` artifact 前提です。

確認済み:

- serialized artifact load
- `Module::deserialize`
- instantiate / export call
- host function 1 本 (`host_log_i32`)
- linear memory の host read / write-back

次の継続点は trap demo と、そこから先の WASI 前段です。
