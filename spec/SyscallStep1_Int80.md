# Syscall Step1: ring3 -> kernel エントリ（int 0x80）

## 目的
ring3 から kernel へ制御を移す最小 syscall 入口を実装し、ユーザー空間からの特権移行経路を確立する。

## 方針
- まずは `SYSCALL/SYSRET` ではなく `int 0x80` を使って段階的に導入する。
- 目的は「入口が動くこと」の確認であり、戻り値や復帰は次ステップに回す。

## 実装内容
1. IDT vector `0x80` を追加。
2. gate 属性は `0xEE`（Present + DPL=3 + interrupt gate）に設定。
3. ring3 側テストコードで `int 0x80` を発行。
4. kernel 側 `syscallHandlerCommon(sysno)` でシリアルログを出して停止。

## 現在の挙動
- ring3 で mapped ページ書き込み後、`int 0x80` で kernel に遷移する。
- kernel は次のように出力して停止する。

```text
SYSCALL INT80
  SYSNO=0x1
```

## 注意点
- この段階は「入口確認」のみで、syscall から ring3 へ復帰しない。
- 本格化には以下が必要:
  - syscall 番号ディスパッチ
  - 戻り値 ABI（`rax`）
  - レジスタ保存/復帰と `iretq` 復帰
  - capability 操作 syscall (`alloc/map/move`) の追加

---

# Syscall Step2: free list / CNode / PTE 操作

## 目的
`free list -> CNode -> PTE` を syscall 経由で操作できる最小経路を作る。  
PTE 生成は capability を唯一根拠とする。

## syscall ABI（暫定）
- 呼び出し: `int 0x80`
- 入力:
  - `RAX`: syscall 番号
  - `RDI`,`RSI`,`RDX`: 引数
- 出力:
  - `RAX`: 戻り値（`0` 成功、または `paddr`、失敗時は小さいエラーコード）

## 実装した syscall
1. `sys_alloc_page = 0x1`
   - 処理: `allocPageTo(.Process0, &free_list)`
   - 効果: free list から取り出し + CNode に capability 追加
   - 戻り値: 新規 `paddr`（成功）/ エラーコード

2. `sys_map_page = 0x2`
   - 引数:
     - `RDI=va`
     - `RSI=paddr`
     - `RDX=flags`（bit0: writable）
   - 処理: Process0 の CNode に `paddr` capability があり、`cpu_read`（+ writableなら`cpu_write`）を満たす時のみ user PTE 作成
   - 効果: capability なしページは map 不可
   - 戻り値: `0` 成功 / エラーコード

3. `sys_move_cap = 0x3`
   - 引数:
     - `RDI=paddr`
     - `RSI=to`（`0=Process0`, `1=Device0`）
     - `RDX=rights_bits`（bit0 read, bit1 write, bit2 dma）
   - 処理: `moveCap` を実行
   - 効果: `pte_sync_hook` により PTE 権限が同期される
   - 戻り値: `0` 成功 / エラーコード

## テストコード（ring3）
以下を順に実行:
1. `sys_alloc_page`
2. `sys_map_page(user_unmapped_test_va, allocated_paddr, writable=1)`
3. `user_unmapped_test_va` へ書き込み（成功）
4. `sys_move_cap(allocated_paddr, Device0, dma-only)`
5. 同じ書き込みを再実行（`Present` が落ちて `#PF` 期待）

## 期待ログ
`#PF` 発生時:
- `CR2 = user_unmapped_test_va`
- `EC` は user write fault 相当

## Step2 実測結果（統合確認）
実測で次を確認済み:
1. `sys_alloc_page` で `paddr` を取得
2. `sys_map_page` で `user_unmapped_test_va` へ map
3. ring3 書き込み成功
4. `sys_move_cap(..., Device0, dma-only)` 実行
5. 同一 `va` への再書き込みで `#PF`

観測された fault:

```text
PAGE FAULT
  CR2=0x500000
  EC=0x6
```

意味:
- `P=0`（not present）
- `W=1`（write）
- `U=1`（user）

これにより、capability の移譲が PTE 権限に反映され、ring3 CPU 書き込みが遮断されることを確認した。
