# Rust Demo Apps

`userland/rust/` には CapabilityOS の Rust demo app を置く。

runtime 実装そのものは `user/rust/` に残す。

## 役割分担

- `user/rust/`
  - `rt_*`
  - `cap_std`
  - `rt_wasmtime_platform`
  - `wasmtime_host`
- `userland/rust/`
  - Wasmtime demo app
  - 今後の user-facing Rust demo app

## 現在の Wasmtime demos

- `rust_wasmtime_platform_demo`
- `rust_wasmtime_host_demo`
- `rust_wasmtime_loader_demo`
- `rust_wasmtime_hostcall_demo`
- `rust_wasmtime_memory_demo`
- `rust_wasmtime_writeback_demo`
- `rust_wasmtime_trap_demo`

publish 定義は `userland/apps/<app-id>/app.conf` にある。
