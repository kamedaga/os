# BootLog -> Compositor On Enter

## 目的
- BootLog 表示を Compositor から分離する。
- BootLog 画面で Enter が押されたら Compositor を起動する。

## 変更概要
- `BootRuntimeMode` に `BootLogGateCompositor` を追加。
- `enable_bootlog_wait_for_enter=true` かつ mouse/keyboard 検出時は、
  - 起動直後に `BootLogConsole(Process1/Thread1)` を実行
  - `MouseDriver(Process0/Thread0)` と `KeyboardDriver(Process3/Thread3)` を並行常駐
  - Compositor ELF は遅延起動待ち

## Enter トリガ
- `syscall_log` 経由で `Thread3` のキーログを監視。
- 以下を Enter 押下として扱う:
  - `key code=0x1c value=1` (`KEY_ENTER`)
  - `key code=0x60 value=1` (`KEY_KPENTER`)
- Enter 検知時:
  - Process1 の user page を Compositor ELF で再ロード
  - Thread1 の entry/rsp を Compositor 用に更新
  - 可能なら即時 `switchToThread(1)` で Compositor に遷移

## BootLog 画面の入力ガイド表示
- BootLog 描画完了後に、画面末尾へ英語ガイドを表示:
  - `Press Enter to launch compositor.`
- ガイド文字色は yellow (`0x00FF_FF55`)。

## BootLog ローディングUI
- ローディングUIは撤回し、BootLogConsole は従来どおり ANSI パーサ描画に戻した。

## Process3/Thread3
- keyboard は `Process3/Thread3` 常駐に移動。
- `Process2/Thread2` は `MouseCompositor` モードで BootLogSender 専用。

## BootLog先行描画のための起動順固定
- `BootLogGateCompositor` モードのみ、`Thread0(Mouse)` / `Thread3(Keyboard)` の `ready` を一時的に下げる。
- ring3移行後、LAPIC timer tick が一定回数 (`bootlog_gate_input_start_delay_ticks=8`) 経過したら `ready=true` に戻す。
- これにより、BootLogConsole (`Thread1`) が先に画面描画してから `Keyboard/Mouse queue ready` が走る順序を安定化。

## 追加/調整箇所
- `kernel/src/main.zig`
  - `BootLogGateCompositor` モード
  - deferred compositor launch state と Enter 検知処理
  - 起動/所有者ログに Process3/Thread3 を反映
- `kernel/src/kernel.zig`
  - `PrincipalId.Process3` と principal table 拡張
- `kernel/src/capability.zig`
  - Process3 index 変換・dump 対応
- `run.sh`
  - `virtio-keyboard-pci` を追加（mouse+keyboard 同時接続）

## 検証
- `zig build test` 成功
- `zig build efi` 成功
