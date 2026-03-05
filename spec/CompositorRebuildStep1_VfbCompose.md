# Compositor Rebuild Step1 (VirtualFramebuffer Compose)

## 目的
- `compositor.zig` を最小構成で作り直し、仮想Framebuffer共有バッファから物理Framebufferへ合成する経路を先に成立させる。

## 実装内容
- `kernel/user_programs/compositor.zig` を新規実装。
- 入力:
  - 仮想Framebuffer: `0x2000_B000` (`32x32`, `pitch=32`)
- 出力:
  - 物理Framebuffer: `0x2000_4000` (`832x624`, `pitch=832`)
- 合成方式:
  - 最近傍スケーリング
  - 画面中央へセンタリング配置
  - 毎ループで `clear -> compose` 実行

## ログ
- 起動時:
  - `Compositor: started`
  - `Compositor: vfb compose ready`

## 備考
- 旧 `mouse shared page` / `bootlog IPC` / カーソル重畳ロジックは Step1 では削除。
- Step2 で dirty-rect / 複数ウィンドウ / カーソル重畳を再導入する前提。
