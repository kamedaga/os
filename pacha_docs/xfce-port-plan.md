# PachaOS Xfce Full Desktop Port Plan

## Goal

PachaOS 上で Xfce を部品単位ではなく、`Xorg`、`xfwm4`、`xfce4-session` を中心とする完全なデスクトップ環境として動作させる。
対象は安定した Xfce 4.20 系とし、最終構成では Sway、Xwayland、`xfwl4` に依存しない。
既存の `filed`、`drmd`、`inputd`、`termd` と LPR を活用し、Linux アプリケーションは通常の PachaOS process として実行する。

## Development Policy

- 最初からフル構成を直接起動し、実際に発生したエラーを基準に不足機能を直す。
- Xvfb と Xwayland は通常の移植フェーズに含めず、原因を分離できない場合だけ診断に使う。
- 不足機能は LPR または適切な userland service で実装し、安易に kernel へ追加しない。
- kernel または ABI の変更が不可避と証明された場合は、理由と影響を説明して許可を得てから着手する。
- 各フェーズの完了条件を自動テスト、ログ、スクリーンショット、process tree で確認できるようにする。

## Phase 1: フルパッケージ rootfs と標準 Xfce session 経路を一括作成

- Xorg、modesetting、libinput、Xfce 4.20 core、Thunar、terminal、notifyd まで含むパッケージ集合を固定する。
- Xfce 専用 rootfs を再現可能なスクリプトで構築し、使用した package version、hash、設定を記録する。
- PachaOS は LPR process の登録と通常の login environment だけを用意し、Xfce 固有の設定や component 順序を持たない。
- Alpine の `dbus-run-session`、`startxfce4`、`xinit`、`xinitrc` をそのまま使って Xorg と `xfce4-session` を起動する。
- 必要な executable、shared library、theme、icon、font、XKB、D-Bus service が rootfs 内に揃うことを検査する。

### Build and Verification

Phase 1 の実装入口は `tools/build_wsl_alpine_xfce.sh` である。
Alpine v3.22 x86_64 の依存閉包は `tools/manifests/alpine-xfce-v3.22-x86_64.lock` に version と SHA-256 を固定する。
生成物は `.artifacts/userland-fixtures/alpine-xfce-root` に置き、PachaOS 固有の Xfce launcher は作らない。
既定 pack rootfs は Xfce layer を選択し、Sway layer と世代の異なる offline APK/Lua fixture layer は含めない。
Phase 1 の一括検査は次のコマンドで実行する。

```bash
bash tests/run-xfce-phase1-check.sh
```

この検査は全 APK の hash、rootfs 必須ファイル、主要 ELF の loader、pack manifest、Sway 非依存を確認する。
seed0root は標準の `/usr/bin/dbus-run-session -- /usr/bin/startxfce4` を直接起動する。実画面の検証は Phase 2 で行う。

## Phase 2: Xorg・xfwm4・panel・xfdesktop の初回表示

- Xorg を `/dev/dri/card0` と `/dev/input` に直接接続し、modesetting、glamor、libinput を有効にして起動する。
- `dbus-run-session` から `xfce4-session` を開始し、正規の Xfce session として各 component を管理させる。
- `xfwm4` compositor、`xfce4-panel`、`xfdesktop`、`xfsettingsd` を最初から同時に起動する。
- 壁紙、panel、xfwm4 の window decoration を表示し、menu から Thunar と terminal を起動する。
- 初回表示時の console log、LPR trace、Xorg log、process tree、スクリーンショットを成果物として保存する。

## Phase 3: 起動を止めた LPR・DRM・input 問題を実エラー駆動で突破

- boot を繰り返し、各回で最初に進行を止めた syscall、ioctl、FD、protocol error を一つずつ特定する。
- 推測による互換機能の先行追加は避け、再現ログと最小テストで必要性を証明した機能だけを実装する。
- Linux ABI の意味論は LPR、display は `drmd`、input は `inputd`、filesystem は `filed` の責務として修正する。
- 修正ごとに unit test と実 boot regression test を追加し、既存の Sway、GTK、D-Bus、Thunar 実績を壊していないことを確認する。
- keyboard、mouse、window move、resize、maximize、workspace 切替が継続して操作できる状態を完了条件とする。

## Phase 4: notifyd・power manager・screensaver・volman を統合

- `xfce4-notifyd` を session D-Bus に統合し、desktop notification の表示、置換、timeout を確認する。
- `xfce4-power-manager` を display power control と接続し、idle、blank、復帰を PachaOS の authority 境界内で処理する。
- `xfce4-screensaver` の idle detection、lock、認証、unlock を実装し、lock 中に desktop を操作できないことを検証する。
- Thunar、Tumbler、GVfs、`thunar-volman` を接続し、mount、unmount、removable media、thumbnail を機能させる。
- system D-Bus、polkit、UPower などが必要な場合は、PachaOS service への userland adapter として責務を明確に実装する。

## Phase 5: 設定保存・ログアウト・再起動・連続起動・性能改善

- xfconf、panel、xfwm4、desktop の設定を永続化し、再起動後に theme、layout、shortcut、wallpaper が復元されることを確認する。
- logout、session restart、shutdown、reboot を `xfce4-session` から安全に要求し、全 process と capability を回収する。
- cold boot、application launch、window operation、logout を含む連続試験を最低 10 回実行し、不安定な race や resource leak を除去する。
- boot-to-desktop と主要 application の起動時間、CPU、memory、syscall 数を計測し、trace に基づいて bottleneck を改善する。
- 最終 process tree から Sway、wlroots、Xwayland、`xfwl4` への依存がないことを確認し、build と操作手順を文書化する。

## Final Acceptance

PachaOS の cold boot から Xfce desktop が自動的に立ち上がり、`xfwm4` が window management と compositing を担当すること。
Panel、desktop、settings、file manager、terminal、notification、lock、power、volume management が同一の Xfce session 内で機能すること。
通常操作、設定保存、logout、shutdown、reboot、連続起動が再現可能な自動試験と保存済み成果物によって確認できること。
