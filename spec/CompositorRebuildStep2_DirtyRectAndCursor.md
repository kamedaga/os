# Compositor Rebuild Step2 (Dirty-Rect + Cursor Overlay)

## 目的
- Step1 の「VFB -> FB 合成」に対して、更新差分だけを描く `dirty-rect` とマウスカーソル重畳を追加する。

## 変更内容
- `kernel/user_programs/compositor.zig`
  - `virtual_framebuffer_va (0x2000_B000)` を毎ループ監視し、前回フレーム `shadow` と比較。
  - 差分ピクセルのみ物理Framebufferへ反映（最小単位は拡大後セル）。
  - `back_buffer`（シーン本体）を保持し、物理Framebufferは表示用として扱う。
  - `back_buffer` は静的巨大配列ではなく、起動時に確保したページ群を仮想連続にマップして利用。
  - `recv_cap` / `map_page` / `grant_cap` で mouse shared page を受信して Process0 に返却。
  - shared page (`MSHR`) の `x/y/buttons/seq` を読み取り、カーソル位置・ボタン状態を更新。
  - マウスのみ更新時は「前回カーソル矩形だけ `back_buffer` から復元 -> 新カーソル描画」。
  - シーン dirty 時は dirty 矩形だけ `back_buffer -> 物理Framebuffer` を反映し、最後にカーソルを重畳。

## ログ
- `Compositor: started`
- `Compositor: vfb compose ready`
- `Compositor: mouse shared page received via IPC`（shared受信時）

## 補足
- Step2 時点では dirty 伝搬は「ピクセル差分比較ベース」。
- 将来 Step3 でアプリ側 dirty-rect 通知（矩形キュー）へ置換可能。
- 静的巨大配列を避けることで `COMPOS.ELF` のサイズ増大を防止し、deferred compositor launch の ELF ロード失敗を回避。
