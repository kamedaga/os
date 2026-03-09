# Keyboard Input Process Step1

## 目的
- `BootLogConsole` 系の起動モードに keyboard 入力取得プロセスを追加する。
- 既存の `MouseDriver + Compositor + BootLogSender` 経路とは競合しないようにする。

## 方針
- 既存 `Process2/Thread2` を `BootLogConsole` モード時のみ再利用。
- `MouseCompositor` モード時は従来どおり `Process2=BootLogSender`。
- keyboard は `mouse_driver_cfg == null` のときだけ probe/起動し、同時駆動を避ける。

## 追加ファイル
- `kernel/user_programs/keyboard_driver.zig`
  - virtio-input queue 初期化
  - `EV_KEY` + `SYN_REPORT` を読み取り
  - `userLog` に `key code=... value=...` を出力

## 変更点
- `kernel/build.zig`
  - `KEYBDRV.ELF` のビルド/インストールを追加
- `kernel/src/main.zig`
  - `enable_virtio_input_keyboard` を追加
  - `KEYBDRV.ELF` のディスクロードを追加
  - `KeyboardDriverProcessSetup` と `setupKeyboardDriverProcess(...)` を追加
  - `publishKeyboardDriverConfigPage(...)` を追加
  - `setupUserProcessesForMode(...)` に keyboard cfg を追加
  - `BootLogConsole` 分岐で `Thread2` を keyboard driver として起動可能にした
  - ELF マップ時に `KeyboardDriver ELF mapped` ログを追加
  - ring3 進入ログに `boot log console + keyboard driver` を追加
- `setupDisk.sh`
  - `KEYBDRV.ELF` のコピーを追加

## 検証
- `zig build test` 成功
- `zig build efi` 成功
