# IDT Step1: #PF のみ実装

## 目的
まずは例外処理の土台として、IDT を導入し `#PF`（Page Fault, vector 14）だけを受ける。

## 実装範囲
- IDT エントリ構造体の定義（x86_64 16byte gate）
- IDT テーブル `idt[256]` の確保
- `setIdtEntry(vec, handler)` 実装
- `lidt` による IDT ロード
- `#PF` ハンドラ実装（ログ出力して停止）

## コード要点
- `IdtEntry`:
  - `offset_low`
  - `selector`
  - `ist`
  - `type_attr`
  - `offset_mid`
  - `offset_high`
  - `zero`
- `selector = 0x08`（kernel code segment）
- `type_attr = 0x8E`（interrupt gate + present）

## #PF ハンドラ
```text
pageFaultHandler():
  serialWrite("PAGE FAULT")
  serialWrite("CR2=<fault address>")
  serialWrite("EC=<error code>")
  halt loop
```

ログ例:

```text
PAGE FAULT
  CR2=0xdeadbeef
  EC=0x2
```

## 初期化順序（現状）
1. UEFI 起動
2. Memory / Capability 初期化
3. `ExitBootServices`
4. 自前ページテーブル構築 + `CR3` 切替
5. `initIdtPageFaultOnly()` 実行
6. 以降のベアメタル処理へ

## 受け入れ条件
- 起動ログに `IDT loaded (#PF only)` が出る
- #PF 発生時に `PAGE FAULT` / `CR2` / `EC` が出て停止する

## 次ステップ
- `#GP` / `#UD` など他例外の追加
- エラーコードや fault address (`CR2`) のログ化

---

# IDT Step2: #PF/#GP を error_code 付き TrapFrame へ統一

## 目的
`#PF` / `#GP` の例外入口を、明示的な構造体境界（TrapFrame）で統一する。

## 追加した構造体
```zig
const ExceptionTrapFrame = extern struct {
    r15: u64, r14: u64, r13: u64, r12: u64, r11: u64, r10: u64, r9: u64, r8: u64,
    rbp: u64, rdi: u64, rsi: u64, rdx: u64, rcx: u64, rbx: u64, rax: u64,
    error_code: u64,
    rip: u64, cs: u64, rflags: u64, rsp: u64, ss: u64,
};
```

## 実装内容
1. `#PF` / `#GP` スタブで汎用レジスタを保存。
2. `ExceptionTrapFrame` 先頭ポインタを共通 C 関数へ渡す。
3. 共通関数 `exceptionWithErrorCommon(vec, frame)` でログ出力:
   - 例外名
   - `EC`（`frame.error_code`）
   - `RIP`（`frame.rip`）
   - `#PF` の場合のみ `CR2`
4. 最後は停止（`hlt` loop）。

## レイアウト安全性
`comptime` で以下オフセットを検証する:
- `rax = 112`
- `error_code = 120`
- `rip = 128`
- `ss = 160`

これにより、スタブと構造体定義のズレをビルド時に検出できる。

## 効果
- #PF/#GP の例外経路が同じフレーム規約で動く。
- デバッグ時に `error_code` と `rip` を安定して取得できる。
- 将来の例外ハンドラ拡張（TS/NP/SS など）を同じ形式へ揃えやすくなる。

---

# IDT Step3: #GP 実装と動作確認

## 実装
1. IDT vector 13 に `generalProtectionHandlerStub` を登録済み。
2. `generalProtectionHandlerStub` は `ExceptionTrapFrame` を構築し、`exceptionWithErrorCommon(13, frame)` を呼ぶ。
3. 共通ハンドラは以下を出力して停止する。
   - `GENERAL PROTECTION`
   - `EC=<error_code>`
   - `RIP=<fault RIP>`

## ring3 からの #GP テスト
`debug_trigger_general_protection_test = true` のとき、ring3 テストコードを次へ切り替える:

```text
cli
jmp $
```

`cli` は特権命令のため CPL3 では `#GP(0)` が発生する。

## 期待ログ
```text
enter ring3 with iretq (expected #GP by user CLI)
GENERAL PROTECTION
  EC=0x0000000000000000
  RIP=0x...
```

## 備考
- 通常運用では `debug_trigger_general_protection_test = false` のままにし、従来の capability + #PF テストを実行する。
- #GP 実装は有効化済みで、テストフラグは検証シナリオを切り替えるためのもの。

---

# IDT Step4: #PF 復帰準備（Step2: 判定と capability 検索）

## 目的
`#PF` を将来的に復帰可能にするため、まずはハンドラ内で必要判定を揃える。

## 今回実装したこと
1. `CR2` を取得して fault VA を確定。
2. `EC` bit2 から user mode fault か判定。
3. fault VA が user canonical 範囲か判定。
4. 現行 user PT から candidate `paddr` を抽出（present=0 でも `paddr` は読む）。
5. `Process0` の CNode を検索し、candidate `paddr` の capability 有無をログ出力。

出力例:

```text
PAGE FAULT
  CR2=0x0000000000500000
  EC=0x0000000000000006
  USER_MODE=1
  USER_VA=1
  PF_CAP=issued
  CAND_PADDR=0x0000000001785000
  CAP_LOOKUP=none(Process0)
```

## まだやらないこと（Step3で実装）
- PTE 動的生成
- `invlpg`
- `iretq` で faulting 命令へ復帰

## 運用ルール（注意点）
1. 復帰対象は `P=0`（not-present）fault のみ。
2. `P=1`（protection violation）は権限違反として停止。
3. map 根拠は capability のみ（capability なしなら map しない）。
4. #PF ハンドラは最小作業に留める（重い処理を避ける）。
5. map 失敗時は必ずログを残して停止（再帰faultを避ける）。

---

# IDT Step5: #PF 復帰（PTE 動的再有効化 + iretq）

## 目的
`#PF` のうち安全に回復可能なケースだけを復帰させる。

## 実装
1. `pageFaultDispatch(frame)` を追加。
2. 復帰条件:
   - `EC.P=0`（not-present）
   - `EC.U=1`（user fault）
   - fault VA が user canonical 範囲
   - `Process0` が候補 `paddr` の capability を保有
   - write fault の場合は `cpu_write` 権限あり
3. 条件を満たすと:
   - capability を根拠に PTE を再生成（`Present` 復帰、`RW` は capability に従う）
   - `invlpg(fault_page_va)` 実行
   - `#PF` stub で `error_code` を捨てて `iretq` 復帰
4. 条件不一致は従来どおり停止ログへフォールバック。

## スタック/CR3 の扱い
- #PF 入口で `kernel_cr3` に切替。
- 復帰経路では `user_cr3` を復帰してから `iretq`。
- #PF は error code を積むため、`iretq` 前に `add rsp, 8` で除去。

## 期待効果
- 「未マップ」は capability 次第で回復可能になる。
- capability を失ったページ（例: DMA へ move 済み）は回復不能のまま停止し、安全側に倒れる。

---

# IDT Step6: #PF 復帰成功の実証ケース

## 目的
`Process0` が capability を保持したまま `Present` だけ落とした場合に、`#PF` から復帰できることを実証する。

## 追加 syscall
- `sys_drop_present = 0x4`
  - 引数: `RDI=paddr`
  - 条件: `Process0` が当該 `paddr` capability を持つ
  - 動作: 対応 PTE の `Present` のみ落とす（capability は保持）

## ring3 デモ（切替）
`debug_trigger_pf_recovery_demo = true` のとき、ring3 テストを次へ切り替える:
1. `sys_alloc_page`
2. `sys_map_page(0x500000, paddr, writable=1)`
3. `sys_drop_present(paddr)`
4. `*(u32*)0x500000 = ...`  
   - 1回目アクセスで `#PF`（not-present）
   - kernel が capability を確認して map 復帰
   - `iretq` 復帰後、同じ命令が再実行され成功
5. `*(u32*)0x600000 = ...` で最終停止用の fatal `#PF`

## 期待ログ
```text
enter ring3 with iretq (expected #PF recover + final fatal #PF)
PAGE FAULT RESOLVED
  CR2=0x0000000000500000
PAGE FAULT
  CR2=0x0000000000600000
  ...
```

この 2 段ログで「回復成功」と「回復不能停止」の両方を確認できる。

---

# IDT Step7: #PF の Capability 型昇格（FaultCap）

## 目的
`#PF` を「条件分岐の寄せ集め」ではなく、明示的な capability オブジェクトとして扱う。

## 導入した型
```zig
const PageFaultCapability = struct {
    principal: PrincipalId,
    fault_va: u64,
    fault_page_va: u64,
    fault_rip: u64,
    present_violation: bool,
    write_access: bool,
    instruction_fetch: bool,
    candidate_paddr: ?u64,
};
```

## 新しい流れ
1. `issuePageFaultCapability(frame, cr2)` で FaultCap を発行
   - user fault かつ user canonical VA のときだけ発行
2. `resolvePageFaultCapability(fault_cap)` で回復可否を判定
   - `present_violation=false`（`P=0`）であること
   - capability を保持していること
   - 書き込み時は `cpu_write` 権限があること
3. 成功時は `mapUserPageFromCapability(...)` で PTE を復帰し `iretq` 復帰

## 効果
- #PF 復帰条件が `PageFaultCapability` に集約され、例外処理の境界が明確化。
- 将来、FaultCap をユーザー空間 pager へ委譲する設計に拡張しやすい。

---

# IDT Step8: int80/#PF 専用トランポリン（常時マップ最小コード）

## 目的
user CR3 をより強く分離しても、`int 0x80` / `#PF` を確実に kernel 側へ遷移させる。

## 実装
1. 4KB ページ単位のトランポリンコード領域を用意。
   - `int80`, `#PF`, `#GP`, `#DF`, `#UD`, `#TS`, `#NP`, `#SS`
2. 各トランポリンは最小命令のみ:
   - `push rax`
   - `mov rax, kernel_cr3_value`
   - `mov cr3, rax`
   - `pop rax`
   - `jmp [rip+0]`（実ハンドラアドレス）
3. IDT は実ハンドラ直参照をやめ、トランポリン先を登録する。

## 意味
- 例外/割り込み入口で必ず kernel CR3 へ切替してから本体処理へ入る。
- user CR3 から kernel 本体マッピングを縮退する段階でも、入口の安全性を保てる。

## 現段階の位置づけ
- user CR3 は広域 kernel direct map を持たず、トランポリン + 例外入口に必要な最小 supervisor bridge のみを常時マップする。
- これにより、入口の安全性を維持したまま per-process CR3 分離を進められる。

---

# IDT Step9: #PF Recover 標準フロー化と Process1 実行

## 目的
- `#PF` 回復を通常運用に組み込み、capability 根拠がある fault を自動復帰させる。
- 同一フローを `Process1` にも適用し、per-process CR3 分離下で再現する。

## 実装
1. 通常デモを `#PF recover` ベースへ変更。
2. `int 0x80` syscall に `sys_switch_process(0x5)` を追加。
   - `RDI=0/1` で target process を指定。
   - kernel 側で `current_user_principal` / `user_cr3_value` を切替。
   - TrapFrame の `rip/rsp` を対象 process の user 入口へ差し替え、`iretq` で継続。
3. 実行順:
   - Process0 で `sys_drop_present` 後アクセスし `PAGE FAULT RESOLVED`
   - syscall で Process1 へ切替
   - Process1 でも同様に recover
   - 最後は capability 根拠なし fault で停止

## 判定ルール（標準）
- `P=0` かつ user fault で、candidate `paddr` の capability があれば復帰。
- それ以外（capability なし / protection violation 等）は停止ログへフォールバック。

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
