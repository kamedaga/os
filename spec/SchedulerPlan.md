# Scheduler Plan (Step0)

## 目的
- LAPIC timer を使って `Process0/Process1` を時分割実行する。
- `int 0x80` と `#PF recover` の既存経路を壊さずに、最小プリエンプトを導入する。

## 原則
- Capability は Process に属する。
- Scheduler は Thread を扱う。
- したがって実行主体（Thread）は `owner_process` を持ち、capability 判定は常に `owner_process` で行う。

## 現在の前提
- ring3 実行は可能。
- `int 0x80` は `TrapFrame` 経由で kernel に入り `iretq` 復帰できる。
- `#PF` は capability 根拠があれば recover できる。
- per-process user CR3 (`Process0/Process1`) は構築済み。
- LAPIC timer 割り込みは配線済み（tick更新 + EOI）。

## 実装済み（ThreadContext 導入）
- `ThreadContext`（`id`, `owner_process`, `cr3`, `ready`, `TrapFrame`）を導入。
- 起動時に `Thread0->Process0` / `Thread1->Process1` の初期文脈を作成。
- `sys_switch_thread` は固定 `rip/rsp` 書換えをやめ、
  - 現在 thread の `TrapFrame` を保存
  - 対象 process 所有の thread の `TrapFrame` を復元
  する方式へ変更済み。
- これにより cooperative switch でも「続きから再開」できる土台ができた。

## 実装済み（timer preempt switch）
- `timerInterruptDispatch(frame)` に量子判定を追加。
  - `scheduler_quantum_ticks` ごとに context switch を実行。
- 切替手順:
  - 現在 thread の `TrapFrame` を保存
  - 次の `ready` thread を選択
  - `user_cr3_value` と `TrapFrame` を同時に切替
- `iretq` 復帰は既存経路をそのまま利用。
- debug用単一スレッドシナリオでは `Thread1.ready=false` にして自動切替を抑止。

## 実装済み（scheduler probe demo）
- 標準デモを `sys_switch_thread` 非依存で動かすため、ring3 に `scheduler probe` コードを追加。
  - `mov rax, imm64; int 0x80; jmp loop`
  - `Thread0/Thread1` で異なる syscall 番号を回し続ける。
- kernel ログ:
  - `INT80 dispatch ThreadX/ProcessY SYS=...`
  - `SCHED switch ThreadX/ProcessY -> ThreadA/ProcessB`
  でタイマ駆動切替を確認できる。

## Step1 範囲（最小スケジューラ）
- 単一CPU、2プロセス固定、round-robin のみ。
- 優先度・sleep・wait queue は未実装。
- kernel中プリエンプトは行わず、user実行中のみタイマで切替。

## 実装設計
1. `ThreadContext` を導入する。
   - 保持内容: `TrapFrame` 相当の実行文脈（`rip/rsp/rflags/汎用レジスタ`）
   - 併せて `owner_process`, `cr3`, `ready` を保持
2. scheduler 状態を導入する。
   - `current_index`
   - `tick_accum`
   - `quantum_ticks`（例: 5 or 10）
3. 初期化で `Thread0/Thread1` の初期文脈を事前作成する。
   - `rip = user_va`
   - `rsp = user_stack_top`
   - `cs/ss/rflags` は ring3 用
4. `timerInterruptDispatch(frame)` で量子境界判定を行う。
   - 境界でなければ現状どおり return
   - 境界なら:
    - 現在 thread の `frame` を保存
    - 次 thread を選択
    - 次 thread の `frame` を `*frame` に上書き
    - `current_user_principal(owner_process)` と `user_cr3_value` を次 thread に更新
5. 既存 `iretq` 復帰をそのまま利用し、次 thread へ遷移する。

## 安全条件
- context 切替時は必ず `user_cr3_value` と `frame` を同時更新する。
- `ready=false` thread には切替しない。
- `ThreadContext` の `cs/ss/rflags` は常に ring3 設定を維持する。
- syscall/例外経路の kernel CR3 切替規約を破らない。

## 検証項目
1. タイマで `Process0 <-> Process1` が定期切替される。
2. 切替後も双方で `int 0x80` が継続動作する。
3. 切替後も双方で `#PF recover` が継続動作する。
4. `sys_switch_thread` を使わないデモでも 2 process が進行する。

## 次段階（Step2）
- ready queue 化（2固定からNへ）
- ブロッキング syscall と sleep
- timer interrupt をトリガにした wakeup
