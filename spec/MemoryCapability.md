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
- `moveCap` 成功後に `pte_sync_hook(paddr, rights)` を呼ぶ。
- フック側で対象 `paddr` の PTE を探索し、以下を適用する。
  - `cpu_read=false` かつ `cpu_write=false`:
    - `Present` を落として CPU アクセス不可にする。
  - `cpu_read=true` かつ `cpu_write=false`:
    - `Present=1`, `RW=0` にして read-only にする。
  - `cpu_read=true` かつ `cpu_write=true`:
    - `Present=1`, `RW=1` にする。
- 反映後は CR3 リロードで TLB を更新する。

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
