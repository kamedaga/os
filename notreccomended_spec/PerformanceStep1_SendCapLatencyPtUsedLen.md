# Performance Step1: send_cap Latency (PT used_len)

## Goal
- `send_cap` / `grant_cap` 時の遅延を減らす
- シリアルログは維持したまま、cap 同期処理の計算量を下げる

## Root Cause
- cap 変更後の `pte_sync_hook` (`syncPageTableRightsForPaddr`) が、
  - 全プロセス
  - 全 PT スロット (`max_dynamic_pt_pages = 512`)
  - 各 512 エントリ
  を毎回走査していた
- 実際に使っている PT は少数でも、未使用スロットまで毎回走査していた

## Changes
- `UserAddressSpace` に `pt_page_used_len: u16` を追加
- PT スロット確保を「先頭から詰める」方式に統一
  - 新規確保時は `slot = used_len`
  - 確保後に `used_len += 1`
- 走査上限を `max_dynamic_pt_pages` から `pt_page_used_len` に変更
  - `capability.findPtSlotForPd`
  - `capability.mapUserPageFromCapability`
  - `capability.dropPresentForUserMappedPaddr`
  - `capability.syncPageTableRightsForPaddr`
  - `main.findUserPtSlotForPd`
  - `main.ensureUserPtSlotForPd`
- `buildUserAddressSpace(...)` 初期化時に `space.pt_page_used_len = 0` を追加

## Result
- 仕様上のログ量を変えずに、`send_cap from=Process0 ...` 前後の待ち時間を短縮
- cap 変化時の同期コストが「実使用 PT 数」に比例する形になった

## Verification
- `zig build efi` succeeded
- `zig build test` succeeded

## Follow-up (Step2)
- `pte_sync_hook` を `syncPageTableRightsForPrincipalPaddr(...)` に変更
- cap 変更時の同期対象を全プロセスではなく変更対象 principal のみに限定
  - `moveCap`: `from` と `to` の2者を同期
  - `grantCap` / `installCap`: 対象 principal のみ同期
  - `revokeCapTree`: 実際に削除された principal のみ同期

## Follow-up (Step3)
- `flushUserTlbForPrincipalVa(...)` を軽量化（実質 no-op）
- 理由:
  - 本カーネルは syscall/interrupt 復帰時に `CR3` を必ず再ロードする設計
  - そのため `map/sync` 直後に別CR3へ切り替えて `invlpg` する即時 flush は冗長
  - 冗長な `CR3` 切替コストを除去して、`queue ready` 前後の syscall 群を高速化

## Follow-up (Step4)
- `mouse_driver` に boot phase の協調スケジューリングを追加
  - `MouseDriver: queue ready` 直後に `switch_thread(3)` を1回だけ発行
- 目的:
  - mouse 側の待機ループで keyboard の `queue ready` が遅延する現象を回避

## Follow-up (Step5)
- `switch_thread ok ...` の syscall 成功ログをデフォルトOFF化
- 目的:
  - 協調スケジューリング導入時のログ/割り込みコスト増を抑える
