# Memory Capability Step1

## 目的
メモリを Capability で制御するための最初の一歩として、UEFI memory map から Region を自動生成し、観測値をシリアルに出力する。

## 今回やること
1. UEFI `GetMemoryMap` から `conventional_memory` を走査する。
2. `conventional_memory` の各 descriptor を 1 Region として数える。
3. `number_of_pages * 4096` を合算して usable memory を算出する。
4. 生成した Region 数を使って Kernel の Region 配列を初期化する。
5. シリアルに次を表示する。

```text
Detected 37 regions
Total usable memory: 512MB
```

## 実装メモ
- Phase1 時点では Region の物理アドレス範囲は保持しない（ID のみ）。
- Region ID は `0..N-1` の連番とする。
- 初期保持者は capability 走査（`scanCapTables`）で判定する。
- DMA 対象は Region ID ではなく `paddr` で指定する。

## 受け入れ条件
- 起動時に Region 数と合計 usable memory がシリアル出力される。
- Region 数に応じてカーネル内部の Region 配列が初期化される。
- 既存の DMA デモが継続して動作する。

# Memory Capability Step2: Region -> Free Page List

## 目的
`memory map` から生成した Region を、実際に使える 4KB ページ列（free page list）へ落とし込む。

## データ構造
`kernel/src/kernel.zig` に以下を定義する。

```text
RegionFreeRange {
  region_id: u64
  start_index: usize
  len: usize
  physical_start: u64
}

FreePageList {
  pages: [max_pages]u64
  len: usize
  ranges: [max_ranges]RegionFreeRange
  range_len: usize
}
```

意味:

- `pages`: free な 4KB ページの物理先頭アドレス配列
- `ranges`: 各 Region が `pages` のどの範囲に対応するか
- `appendRegion(region_id, physical_start, number_of_pages)` で Region 単位に投入

## 現段階のルール
- `UEFI MemoryType.conventional_memory` を free 対象とする
- 1 descriptor を 1 Region とみなす
- 1 page = 4096 bytes
- `pages[i] = physical_start + i * 4096`
- 以下の reserved 領域は free list から除外する
  - `pml4`
  - `pdp`
  - `pd`
  - `mmap_buffer`

除外判定イメージ:

```text
fn isReserved(paddr: u64) bool {
    return paddr >= reserved_start and paddr < reserved_end;
}
```

free list 生成時に `isReserved(paddr)` が true のページはスキップする。

## デバッグ確認
起動時に以下をシリアル出力する。

```text
Detected <N> regions
Total usable memory: <M>MB
free pages: <P>
```

確認例:

```text
Detected 37 regions
Total usable memory: 461MB
free pages: 118016
```

期待値目安:

- `461MB / 4KB ≈ 118k pages`

## 受け入れ条件
- `free pages` が 0 以外で出る
- `Total usable memory` と `free pages * 4KB` が概ね一致する
- 既存の Capability DMA デモが継続して動作する

# Memory Capability Step3: Free List 健全性テスト

## 目的
free list がページ単位で正しく順序管理されているかを、連続確保ログで確認する。

## テスト内容
起動後に 10 ページを連続確保する。

```text
var i: usize = 0;
while (i < 10) : (i += 1) {
    const cap = try allocPage(.Process0);
    serialWrite("alloc page: ");
    printHex(cap.paddr);
    serialWrite("\n");
}
```

## 期待値
- 連続する出力アドレス間の差分が `0x1000`（4KB）
- 10 行すべて確保成功

確認例:

```text
alloc page: 0x12345000
alloc page: 0x12346000
alloc page: 0x12347000
```

# Memory Capability Step4: paddrベースDMA移譲

## 目的
Capability の識別子を `region_id` から `paddr` に切り替え、`allocPageTo(Process0)` で配布したページを `start_dma(paddr)` で移譲する。

## 実装
- Capability:
  - `Capability { paddr, rights }`
  - `Rights { cpu_read, cpu_write, dma }`
- API:
  - `allocPageTo(.Process0, &free_list)` で 3 ページ配布
  - `startDma(target_paddr)` 実行
- デバッグ表示:
  - `scanCapTables()` だけでなく、Principal ごとの capability 一覧を表示

## 確認シーケンス
1. `allocPageTo(.Process0)` を3回
2. capability 一覧表示
3. 2ページ目の `paddr` を指定して `start_dma(paddr)`
4. capability 一覧を再表示

期待ログ例:

```text
Process0 caps:
  0x1000
  0x2000
Device0 caps:
  none

start_dma 0x2000

Process0 caps:
  0x1000
Device0 caps:
  0x2000 (dma)
```

# Memory Capability Step5: moveCap 強制

## 目的
Capability の移譲を `moveCap` に一本化し、二重所有を構造的に防ぐ。

## ルール
- Capability の主体間移譲は必ず `moveCap(from, to, paddr, rights)` を使う。
- `startDma(paddr)` / `completeDma(paddr)` は内部で `moveCap` を呼ぶだけにする。
- `from == to` は禁止。
- `to` 側が既に同じ `paddr` を持つ場合は失敗。
- `dma` を保持する間、`cpu_read=false` かつ `cpu_write=false` を強制する。
- `cpu_read/cpu_write` は copy 可能な CPU 権限、`dma` は move-only で扱う。

## 効果
- 同一 `paddr` の二重所有を移譲API単位で防止
- 移譲経路が単一になり、監査と検証が容易
- 実装の変更点が `moveCap` に集約される

# Memory Capability Step6: User Address Space 最小構築

## 目的
ユーザー空間分離に向けた最初の土台として、`user_va` 1ページだけを持つ最小ページテーブルを作る。

## 今回やること
1. `const user_va: u64 = 0x400000` を固定する。
2. `allocPageTo(.Process0)` で 1ページ確保し、CNode に capability を追加する。
3. `user_pml4/user_pdp/user_pd/user_pt` を作成する。
4. `user_pml4` の上位 256 エントリ（`256..511`）に、カーネル側 `pml4` の同範囲をコピーする。
5. `user_va` の PTE を1本だけ作る。

## ページテーブル構成
- `PML4[user_va>>39] -> user_pdp`
- `PDP[user_va>>30] -> user_pd`
- `PD[user_va>>21] -> user_pt`
- `PT[user_va>>12] -> user_page_paddr`

各段に `Present | RW | USER` を付与する。

## 備考
- 初期段階では `user_va` の identity map でもよい。
- 現実装は `allocPageTo` で得た capability を根拠に `user_va` をマップする。
- まだ `CR3` はユーザー用へ切り替えず、生成と検証ログ出力までを範囲とする。

## ログ確認
起動ログに以下が出れば成功:

```text
user page table ready
  user_va=0x400000
  user_pa=0x...
```

# Memory Capability Step7: moveCap と PTE 権限同期

## 目的
Capability の移譲結果を実際のページテーブルへ反映し、CPU からのアクセス可否をハードウェアで強制する。

## 実装方針
- `KernelState.moveCap` に `pte_sync_hook` を追加。
- `moveCap`（および `allocPageTo`）成功後に `pte_sync_hook(state, paddr)` を呼ぶ。
- フック側で対象 `paddr` の PTE を探索し、以下を適用する。
  - Process0 が `paddr` capability を持たない:
    - PTE を `0`（unmap）にする。
  - Process0 が `cpu_read=true`:
    - `Present=1`、`RW` は `cpu_write` に合わせる。
  - 同一 `paddr` の複数 PTE は 1本だけ残し、他は unmap する。
- 反映後は user CR3 側で `invlpg` して TLB を更新する。

## 期待効果
- DMA へ move したページは CPU から実アクセス不能になる。
- Capability 状態とページテーブル状態の乖離を防止できる。

# Memory Capability Step8: Capability -> PTE 一意根拠

## 目的
ページテーブル生成の唯一根拠を capability に限定する。  
`free list -> Capability -> PTE` の順序を壊せない実装にする。

## 実装ルール
1. free list からページを取る時は `allocPageTo(principal, free_list)` を使う。
2. `allocPageTo` により CNode へ `Capability{paddr, rights}` が追加される。
3. `buildUserAddressSpaceFromCapabilities(...)` は CNode を参照し、
   - capability が存在すること
   - `cpu_read=true` かつ `cpu_write=true` であること
   を満たす場合のみ PTE を生成する。
4. capability がない `paddr` は PTE 生成不可とする。

## 現在のコード対応
- `user_page` / `user_stack_page` は `allocPageTo(.Process0, &global_free_list)` で確保。
- `buildUserAddressSpaceFromCapabilities(&state, .Process0, user_page, user_stack_page)` を経由しないと user PTE が作られない。
- これにより、`allocPage` 単体で取得した未所有ページを user map する経路を排除した。

## 効果
- 「Capability を持たないページが user map される」状態を防止。
- capability モデルと仮想メモリモデルの整合性を保つ。

# Memory Capability Step9: 本物の user CR3 切替

## 目的
ring3 実行中に kernel CR3 を使い続ける状態をやめ、`user_cr3` と `kernel_cr3` を明確に切り替えてアドレス空間分離を強制する。

## 実装
1. `kernel_cr3_value` を保持する。
   - 自前 PML4 を `writeCr3(...)` した直後の値を保存する。
2. `user_cr3_value` を保持する。
   - `user_pml4` 構築後にその物理アドレスを保存する。
3. ring3 入場前に `CR3 = user_cr3_value` を設定する。
4. `int 0x80` ハンドラ入口で `CR3 = kernel_cr3_value` に戻す。
5. syscall の `iretq` 復帰直前で `CR3 = user_cr3_value` を再設定する。

## 期待効果
- ring3 実行時は user 用ページテーブルだけが有効になる。
- syscall/例外処理時は kernel 用ページテーブルで安全に処理できる。
- ユーザー未マップアクセスは確実に `#PF` になる。

## 実測ログ
```text
enter ring3 with iretq (sys_alloc/map/move + expected #PF)
PAGE FAULT
  CR2=0x0000000000500000
  EC=0x0000000000000006
```

解釈:
- `CR2=0x500000`: テスト対象の user 側未許可/未存在アドレス
- `EC=0x6`: `U=1` かつ `W=1` かつ `P=0`（user write to not-present）

## 受け入れ条件
- ring3 直前に user CR3 へ切替している。
- `int 0x80` 往復で kernel/user CR3 が往復切替される。
- ring3 から未マップ（または capability により剥奪済み）領域へ書き込み時に `#PF (CR2, EC)` が再現する。

# Memory Capability Step10: Strict PTE-Capability Synchronization

## 目的
`cap` と `PTE` の不整合を「設計上の前提」ではなく、カーネル実装で強制的に排除する。

## 強制ルール
1. `cap remove`（Process0から消える）時は対応 PTE を必ず unmap。
2. PTE 変更時は必ず user CR3 側で `invlpg`。
3. 同一 `paddr` の user PTE は1本のみ（1:1）に強制。
4. `moveCap` の同期は「移譲先 rights」ではなく「Process0の実cap状態」を参照して決定。

## 実装反映
- `pte_sync_hook` 署名を `hook(state, paddr)` へ変更。
- `allocPageTo` と `moveCap` の両方で同期フックを呼ぶ。
- `syncPageTableRightsForPaddr(state, paddr)` が Process0 CNode を参照して
  - unmap / read-only / read-write を決定する。
- `mapUserPageFromCapability` は同一 `paddr` の alias PTE を削除してから map する。
- `flushUserTlbForVa(va)` を導入し、現在 CR3 が kernel のときでも user CR3 側 TLB を確実に無効化する。

## 効果
- `cap` 削除後に stale mapping が残る経路を排除。
- DMA 移譲直後の CPU 側アクセス無効化が即時反映。
- 複数 CR3 環境でも PTE/TLB 不整合を抑制できる。

## 即時無効化デモ
`debug_trigger_dma_unmap_verify_demo = true` のとき、次を実行して検証できる。

1. `start_dma` 対象ページを事前に `user_dma_verify_va(0x510000)` へ map
2. `start_dma(paddr)` を実行（Process0 cap が消える）
3. ring3 で `*(u32*)0x510000 = ...` を実行

期待:
- `start_dma` 直後に PTE が unmap + user CR3 側 `invlpg` 済み
- ring3 最初の書き込みで即 `#PF`（`CR2=0x510000`）

# Memory Capability Step11: Per-Process CR3 Separation

## 目的
`Process0` だけでなく、各プロセスが独立した user CR3 を持つ構成へ拡張する。

## 実装
1. `PrincipalId` に `Process1` を追加。
2. 各プロセス用に独立 `UserAddressSpace`（`pml4/pdp/pd/pt/cr3`）を保持。
3. 起動時に `Process0` と `Process1` の user address space を個別構築。
4. `mapUserPageFromCapability / dropPresent / syncPageTableRightsForPaddr` は
   principal 指定で対象プロセス空間を操作する。
5. `#PF` 復帰判断も current process の fault capability を根拠に処理する。

## Kernel direct mapping について（現段階）
- user CR3 から広域 identity map を削除し、次のみ残す:
  - user page / user stack
  - 例外・syscall 入口に必要な最小 supervisor bridge（stub, IDT/GDT/TSS, ring0 stack 等）
- これにより、従来の「0..1GiB 全体を supervisor で鏡写し」状態を縮退した。

## 備考
- `int80/#PF` トランポリン導入により、user CR3 は最小 supervisor bridge 方式で運用可能になった。

# Memory Capability Step12: #PF Recover 標準化 + Process1 同一シナリオ

## 目的
- `#PF` 回復をデバッグ専用分岐ではなく、通常経路の標準動作にする。
- 同一の recovery シナリオを `Process0` だけでなく `Process1` でも実行して確認する。

## 実装
1. 通常ブート時の user テストコードを recovery ベースに変更。
2. `sys_switch_process(0x5)` を追加。
   - 引数: `RDI=target process id` (`0`=`Process0`, `1`=`Process1`)
   - 動作:
     - `current_user_principal` を切替
     - `user_cr3_value` を対象 process の CR3 に切替
     - `TrapFrame.rip/rsp` を `user_va/user_stack_top` に差し替え
     - `iretq` 復帰先を次プロセスへ切替
3. 標準デモを2段構成にした。
   - `Process0`: `sys_alloc -> sys_map -> sys_drop_present -> write(#PF recover) -> sys_switch_process(1)`
   - `Process1`: 同じ recovery シーケンスを実行後、最終 fatal `#PF` で停止

## 期待ログ
```text
enter ring3 with iretq (std #PF recover: Process0 then Process1)
PAGE FAULT RESOLVED
  CR2=...
PAGE FAULT RESOLVED
  CR2=...
PAGE FAULT
  CR2=...
```

## 意味
- `capability` が残っている not-present fault は復帰可能。
- capability 根拠がない fault は停止（安全側）。
- recovery ロジックが process 固有 CR3 分離と整合した状態で動作する。


# Capability Revoke Tree

## 目的
- capability の移譲先が多段化しても、root 側から回収（revoke）できるようにする。
- 追跡不能な権限リーク（DMAバッファ残存など）を防ぐ。

## 追加した中核
1. capability lineage
- `Capability` に以下を追加:
  - `cap_id`
  - `root_cap_id`
  - `parent_cap_id`
- `allocPageTo` は root capability を発行する（`parent_cap_id=0`）。
- `moveCap` は `cap_id/root/parent` を保持したまま所有者だけ移す。
- `grantCap` は child capability を新規発行する（`parent_cap_id=source.cap_id`）。

2. revoke tree
- `revokeCapTree(owner, paddr)` を追加。
- owner が持つ `paddr` の capability を起点に、`parent_cap_id` を辿って subtree 全体を削除する。
- 削除された各capについて `pte_sync_hook` を発火し、PTE整合性も維持する。

3. endpoint capability 連携
- `send_cap` は endpoint cap により宛先解決し、内部的には `moveCap` を通る。
- したがって send で移動した cap も lineage の追跡対象になる。

## syscall
- `sys_send_cap = 0x6` (`RSI=endpoint_id`)
- `sys_revoke_tree = 0x7` (`RDI=paddr`)
- `sys_grant_cap = 0x8` (`RDI=paddr`, `RSI=dst process id`, `RDX=rights bits`)

## 戻り値（主なもの）
- `RAX=0`: 成功
- `RAX=10`: revoke 失敗
- `RAX=11`: grant 失敗

## 期待される挙動
- 例:
  - `Process0` が root cap を保持
  - `grant` で `Process1` に child
  - `Process1` がさらに `grant`（将来 `Process2` へ）
- `revokeTree(Process0, capX)` で subtree が一括で無効化される。

## テスト
- `grantCap creates child and revokeCapTree at root removes descendants`
- `revokeCapTree from child only removes child subtree`
