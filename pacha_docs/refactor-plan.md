# PachaOS リファクタリング計画 — 状態管理・結合・構造の単純化

対象: kernel / filed / Linux Personality Runtime (LPR) / termd / kobox(koboxd) / netd / lpr_supervisor
ゴール: デバッグ容易性と、clang 連続実行・mesa3d (まず llvmpipe) に耐える構造。
非ゴール: プロセス配置の変更 (koboxd / filed / termd / netd の分離・統合はしない)、ABI バージョニング、互換シム。

---

## 1. 調査結果 — 複雑性の所在

### 1.1 状態の多重管理 (最重要)

Linux プロセス 1 つの fd 状態が **3 箇所** にある:

| 場所 | 実体 |
|---|---|
| LPR in-process | `personality/linux/runtime/lpr_fd/table.c` |
| lpr_supervisor | `lprs_filedesc_t` (`lpr_supervisor/src/main.c`) — fork/exec 用に常時ミラー |
| filed | open_file / handle table (`filed/src/vfs/object.c`) |

さらにキャッシュが **4 層** それぞれ独自の無効化を持つ:

1. LPR: `lpr_file_map_cache` (lpr_dispatch.c) + `lpr_vfs/cache.inc`
2. filed: page cache / dir cache / negative lookup cache / file VMO cache (すべて dispatch.c の file-scope static)
3. kernel: native VMO + COW table (kernel.zig)

ライフタイム系バグ (clang 2 回目の OOM 等) はこの網の中で起きる。デバッグ時にどの層が古いのか特定する手段がない。

### 1.2 kernel の過剰複雑部

- **`kernel.zig` (5,969 行)**: `KernelState` が process / fd / VMA / pipe / IPC endpoint・channel・reply / native VMO / COW / IRQ publish / ASLR / free page list / VMO backing store free-range allocator を全部持つ god struct。pipe I/O や fork の COW コピーのロジックまで同居。
- **容量管理の二重化**: ほぼ全テーブルが `[N]T` 固定配列 + `_extra: []T` slice の二段構え (`fd_tables` + `fd_tables_extra` など)。同じパターンが 5 回以上重複。
- **VM object cap tree の意味論欠陥**: `SYSCALL_RELEASE_VM_OBJECT` が `revokeVmObjectCapTree()` を呼び、「自分の参照を drop」と「子 cap を無効化」が分離されていない。grant 後に親 (filed/RootVfs) が自分の cap を手放せず参照がリークする。**clang 連続実行 OOM の根本原因** (hikitugi.md 記載の懸案)。
- **`scheduler_connection.zig` (2,511 行)**: module-level `var` 約 30 個 (event queue, log mask 6 種, metric state, verified core state ×4, wake tsc 配列…) を 4 種の spinlock で守る。状態の全体像がコードから読めない。
- **`traps.zig` (1,320 行)**: `Hooks` 関数ポインタ間接層。モノリシックにリンクされる kernel 内で間接呼び出しにする必然性がなく、スタックトレースと grep を阻害。
- **計測の散在**: `ipc_metric.zig` / `page_fault_log.zig` / scheduler metric slots / filed の metric 配列 / LPR の `lpr_append_*` 手書きロガー — 全部フォーマットも取得経路も別。

### 1.3 filed の過剰複雑部

- **`dispatch.c` (7,524 行)**: IPC transport、op ハンドラ、キャッシュ 4 種、metrics、generation publish、lock、error conveyor が 1 ファイル。状態は file-scope static (`filed_page_cache` 等) で `filed_runtime_t` と二重管理。
- **exec の所有権が割れている**: `filed/src/exec/linux_lpr/` (約 5,400 行) が zpoline / LPR プロセスイメージのレイアウトを直接知っている (`#include <personality/zpoline.h>`)。exec を変えるには filed と personality の両方を触る必要がある。
- `filed_backend_*` 薄い転送ラッパが 40 個並ぶ (koboxd storage protocol への 1:1 転送)。

### 1.4 エラー・プロトコルの複雑性

- **error conveyor** (`libipc/error_conveyor.*`): domain 8 × component 6 × stage 13 のフレームチェーン機構。status 空間が境界ごとに別 (kernel status / pacha errno / filed status / termd status / lprs status / Linux errno / ipc status) なことへの対症療法。
- プロトコルヘッダ 7 本 (`*_v2.h`) + IPC fast path 3 backend (normal / shared VMO ring / pkey ring) + fallback reason 8 種。
- LPR の daemon クライアント (lpr_filed.c / lpr_socket.c / lpr_tty / lpr_process/client.c) が wire page 確保 → memset → payload cast → call → 破棄 を毎回手書き。

### 1.5 LPR の構造問題

- `lpr_dispatch.c` に巨大 switch が 3 つ、`.inc` の textual include (lpr_vfs/*.inc, lpr_process/*.inc, lpr_tty/*.inc) でコード分割。
- 状態がすべて file-scope static (rlimits, umask, fd table, caches) = **シングルスレッド前提**。

### 1.6 clang / mesa3d ギャップ (機能面)

| 必要機能 | 現状 | 根拠 |
|---|---|---|
| VM object の正しい参照カウント | release=subtree revoke でリーク | clang 2 回目 `libgcc_s.so.1: Out of memory` |
| CLONE_THREAD (pthread) | `-ENOSYS` (`lpr_process/syscalls.inc:15`) | mesa/llvmpipe はスレッド必須、LLVM 並列も |
| MAP_SHARED file mapping | `-ENOTSUP` (`lpr_dispatch.c` mmap) | mesa の shmem/dma-buf 経路 |
| memfd_create | なし | mesa の buffer 共有 |
| epoll | なし (eventfd はある) | GUI スタック全般 |

kernel 側には `thread_create/thread_start/futex_wait/futex_wake` が既にあり、ギャップは主に LPR の配線と LPR 自身の thread-safety。

---

## 2. リファクタリング原則

1. **状態ごとに所有者は 1 つ**。他コンポーネントは handle を持つだけ。ミラーを常時同期しない (転送が必要な瞬間だけ serialize)。
2. **status は Linux errno に一本化**。境界ごとの status 空間と error conveyor を廃止。変換は libipc の 1 箇所のみ。
3. **プロセス配置は変えない**。障害ドメインは現状維持。整理はプロセス内のモジュール境界で行う。
4. **互換を作らない**。境界単位で caller/server を一括変換。`_v2` のような世代 suffix も最終的に廃止。
5. **トレースは全層共通の 1 形式**。手書き文字列アペンダと ad-hoc metric slot を廃止。
6. **kernel は機構のみ**。今回の kernel 変更は「意味論の修正 (refcount/revoke 分離)」と「構造の分割」に限定。機能追加・userland 知識の混入はしない (AGENTS.md 準拠)。
6b. **syscall 番号はジャンル帯に正しく配置する**。fd / vm / process / ipc / capsule / runtime のジャンル別番号帯を維持し、追加はジャンル内の正しい位置へ。互換性維持は不要なので後続番号は全部ずらす。「めんどいから末尾に追加」は禁止 (AGENTS.md)。ずらした際は kernel/abi/ と userland 側 abi ヘッダ (musl/pachaos, libpacha) を同一コミットで追随させる。
7. 各タスクは「挙動を変えない移動」か「挙動を変える修正」のどちらかに寄せ、混ぜない。

---

## 3. フェーズ計画

```
Phase 0  観測基盤 (デバッグ容易化を最初に)
Phase 1  kernel: VM lifetime 意味論修正 + 状態分割
Phase 2  filed: dispatch 分解 + キャッシュ一本化 + exec 所有権整理
Phase 3  LPR: syscall table 化 + RPC 共通化 + 状態の構造体化 (thread-safe 準備)
Phase 4  機能ギャップ: threads / MAP_SHARED / memfd / epoll + clang 耐久テスト
Phase 5  プロトコル刷新 (_v2 廃止、envelope 統一)
```

依存: 0 → (1, 2, 3 は並行可) → 4。5 は最後 (機械的リネームが主)。
Phase 4 の T4.1 は T3.4 に、T4.2 は T2.2 に依存。T4.5 (clang 耐久) は T1.1 の受け入れテスト。

---

## 4. タスク分解 (codex 実装単位)

各タスクは 1 PR 相当。**受け入れ条件は必ず QEMU スモーク (`tests/run-lpr-qemu-*.sh`) の全通過を含む**。
実装者への共通指示: AGENTS.md を先に読む。成果物は `.artifacts/` へ。kernel を触るのは kernel と明記されたタスクのみ。

### Phase 0 — 観測基盤

**T0.1 共通トレース基盤 `libpacha/trace`**
- 新規: `userland/libpacha/include/pacha/trace.h` + 実装。イベント = (component id, event id, u64 args×6, tsc)。固定長 ring + serial への text dump 関数。
- LPR の `lpr_append_literal/lpr_append_u64` 手書きロガー群、filed の `filed_dump_dispatch_metrics`、`LPR_TRACE_*` の #if 群をこの基盤に置換。トレース有効化はビルドフラグでなく実行時 mask 1 つ。
- 受け入れ: QEMU 起動ログに従来と同等のトレースが出せる。全スモーク通過。

**T0.2 status の Linux errno 一本化 + error conveyor 廃止**
- filed/termd/lprs/netd の独自 status 定数 (`LPRS_ERR_*` 等) を Linux errno に統一。kernel status ↔ errno 変換を `libipc/status.{c,h}` の 1 箇所へ (現 `lpr_linux_pacha_status_to_errno` を移設・共通化)。
- `libipc/src/error_conveyor.c` (425 行) と全参照を削除。障害位置の特定は T0.1 のトレースで代替。
- 受け入れ: errno が LPR まで正しく伝播する差分テスト (open ENOENT, EACCES 等)。全スモーク通過。

### Phase 1 — kernel

**T1.1 [kernel] VM object の参照 drop と revoke の分離 (現行 FD モデルでの完成)** ※挙動変更
- 補足 (2026-07-09 調査): 旧 `revokeVmObjectCapTree` は FD-based 再設計で消滅し、fd 層は per-fd refcount (`closeFd` は自 entry のみ解放、transfer copy は retain) になっている。本タスクはこの分離を全経路で完成させる。ユーザーにより kernel 編集は許可済み。
- **参照経路の監査と修正**: fd entry / VMA マッピング (mmap 中の VMO) / IPC 転送中メッセージ / fork・COW / プロセス終了 cleanup / exec による address space 置換、の各経路が「自分の retain を持ち、drop は自分の参照だけを落とす」ことを監査。drop が他保持者の backing を壊す経路、または retain 漏れ・release 漏れ (リーク) を修正する。
- **明示 revoke の新設**: `vmo_revoke` 相当を新設。所有サービス (filed 等) が配布済み VMO を無効化できる (全 fd table / VMA から該当 object を除去し、以後の使用はエラー、backing を回収)。revoke 権限は fd rights で制御。プロセス終了時の自プロセス cleanup は従来通り。
- **kernel unit test**: `tests/kernel_state.zig` 系に retain/release 不変条件のテストを追加 (任意順の close で他保持者が壊れない / refcount 0 で free list に全ページが戻る / 二重 close 安全 / revoke 後の全保持者無効化)。
- 受け入れ: kernel unit test (`zig build test`) + QEMU スモーク 3 本通過。fork/exec 反復のリーク耐久スモーク (chibicc サイクル) は T4.5 で clang 版と併せて導入する。

**T1.2 [kernel] kernel.zig の分割** ※挙動変更なし
- `KernelState` のフィールドとメソッドをドメインごとに移動: `state/process.zig`, `state/fd.zig`, `state/vmo.zig`, `state/vma.zig`, `state/pipe.zig`, `state/ipc.zig`。kernel.zig は合成と初期化のみ (目安 1,000 行以下)。
- 固定配列 + `_extra` slice の二段容量パターンを、既存 `initRuntimeStorage` 上の共通ヘルパ 1 つに統一。
- 受け入れ: `zig build efi` 成功、`tests/kernel_state.zig` と全スモーク通過。diff は移動が主で意味変更なしをレビューで確認できる粒度に分割コミット。

**T1.3 [kernel] scheduler_connection.zig の状態集約** ※挙動変更なし
- module-level var 群を `SchedulerState` 構造体 1 つに集約し、lock との対応 (どの lock が何を守るか) をフィールドのグルーピングで表現。
- metric / log mask 群 (`*_log_mask` 6 変数, metric slots) を削除し T0.1 のトレース基盤へ。
- 受け入れ: SMP 起動 + 全スモーク通過。schedulerd 経路の動作確認。

**T1.4 [kernel] 間接層・死に経路の削除** ※挙動変更なし
- `traps.zig` の `Hooks` 関数ポインタを直接呼び出しに。
- IPC fast path の 3 backend (normal / shared VMO ring / pkey ring) の実使用を計測し、使われていない backend と fallback reason 機構を削除して 1 経路 (+normal fallback) に絞る。
- 受け入れ: 全スモーク通過 + ベンチ (`hikitugi.md` の batch 系) が悪化しない。

### Phase 2 — filed

**T2.1 dispatch.c の分割** ※挙動変更なし
- `src/dispatch/transport.c` (IPC loop, session, generation publish) / `src/dispatch/ops_*.c` (op ハンドラ) / `src/cache/` / metrics(T0.1 へ) に分割。file-scope static を全部 `filed_runtime_t` へ移し、グローバル状態をゼロに。
- 受け入れ: `filed/tests/vfs_test.c` と全スモーク通過。各 .c は 1,500 行以下。

**T2.2 キャッシュ一本化** ※挙動変更あり(内部)
- page / dir / negative-lookup / file-VMO の 4 キャッシュを、backend_object をキーとする単一の `filed_cache` モジュールに統合。無効化 API は `filed_cache_invalidate(object)` 1 つ。dirty 管理・flush も同モジュールへ。
- 受け入れ: ext4 永続化スモーク (`run-lpr-qemu-ext4-sync-persistence.sh`) 通過。書き込み→再起動→読み出しの一致。ベンチ非劣化。

**T2.3 exec 境界の契約明文化 (filed↔personality)**
- 改訂 (2026-07-09, ユーザー指摘): ビルダーを personality へ移すと、freestanding な runtime 動的ライブラリとサーバ側ビルダーが同居して逆に複雑化する。exec ビルダーは filed に残す。zpoline/LPR の知識が `filed/src/exec/linux_lpr/` 内に収まっていること自体は問題ではない。
- 問題は境界の契約が暗黙な点: filed が runtime 内部ヘッダ `personality/zpoline.h` を直接 include しており、レイアウト合意が「同じヘッダを見ている」ことでしか担保されない。
- 対処: zpoline トランポリン/イメージレイアウト定数・entry/bootstrap プロトコルだけを共有 ABI ヘッダ (`personality/include/personality/lpr_image_abi.h`、定数のみ・コードなし、`lpr_client_abi.h` と同パターン) に抽出。filed と LPR runtime の両方がこれを見る。filed から `zpoline.h` include を排除。
- 契約バージョン定数を bootstrap ページに埋め、LPR runtime 起動時に検証 (不一致は即エラー) — レイアウトドリフトを起動時に検出可能に。
- 受け入れ: execve スモーク通過。`execve_to_child_start` ベンチ非劣化。filed から personality の runtime 内部ヘッダへの include が 0 件。

### Phase 3 — LPR

**T3.1 syscall dispatch の table 化 + .inc 廃止**
- 3 つの switch を `{nr, handler}` の単一テーブルに。`lpr_vfs/*.inc` 等の textual include を通常の .c/.h に変換 (build script 更新込み)。
- 受け入れ: 全スモーク通過。ENOSYS トレース (T0.1 経由) が syscall 名付きで出る。

**T3.2 daemon RPC クライアント共通化 `lpr_rpc`**
- wire page 確保/破棄・call・reply 検証・errno 変換の定型を `lpr_rpc.{c,h}` に集約。lpr_filed / lpr_socket / lpr_tty / lpr_process の各クライアントを移行。
- 受け入れ: 全スモーク通過。コード量削減を PR に記録。

**T3.3 Linux fd 状態の単一所有**
- LPR in-process fd table を唯一の実体とし、lpr_supervisor 側の常時ミラー (`lprs_filedesc_t` 更新経路) を廃止。fork/execve の瞬間だけ snapshot を serialize して渡す形 (`supervisor_fd_snapshot.c` を唯一の転送経路に)。
- 受け入れ: fork/exec/pipe/dup を使うスモーク (`run-lpr-qemu-fd-pipe-smoke.sh`, coreutils-mini) 通過。

**T3.4 LPR 状態の構造体化と lock 導入 (thread-safe 準備)**
- file-scope static (fd table, rlimits, umask, file_map_cache, vfs cache, socket table…) を `lpr_state_t` 1 つに集約。fd table と mmap 経路に futex ベースの軽量 lock を入れる。single-thread 時のオーバーヘッドは fast path で回避。
- 受け入れ: 全スモーク通過。ベンチ非劣化 (±5%)。

### Phase 4 — clang / mesa3d ギャップ

**T4.1 CLONE_THREAD 対応** (依存: T3.4)
- `lpr_linux_clone` の CLONE_THREAD|CLONE_VM 経路を kernel `thread_create/thread_start` に配線。`set_tid_address` / CLONE_CHILD_CLEARTID の futex wake / `gettid` / TLS (clone の tls 引数) / `exit` (スレッド単位) を実装。
- 受け入れ: musl pthread テスト (create/join/mutex/cond, 4 スレッド) が QEMU で通る新規スモーク。

**T4.2 MAP_SHARED file mapping** (依存: T2.2)
- filed が file を backing する VMO をプロセスへ直接 shared map で渡す。filed キャッシュとの一貫性は「shared map 対象 object は同一 VMO を read/write 経路でも使う」ことで担保 (二重バッファ禁止)。
- 受け入れ: mmap(MAP_SHARED) → write → 別プロセスで read の新規スモーク + msync 後の ext4 永続化確認。

**T4.3 memfd_create + 匿名 MAP_SHARED の fork 継承**
- 受け入れ: memfd + ftruncate + 双方向 MAP_SHARED + fork 越し共有の新規スモーク。

**T4.4 epoll**
- `epoll_create1/epoll_ctl/epoll_wait` を kernel `fd_wait_many` の上に LPR 内で実装 (kernel 変更なし)。
- 受け入れ: pipe/socket/eventfd 混在の epoll 新規スモーク。

**T4.5 clang 復旧 + 耐久テスト (T1.1 の検収)**
- clang は FD-based 再設計前に動いていた (hikitugi.md) が、再設計後の rootfs には未導入。まず clang を rootfs に導入し、`clang --version` → 小さな .c のコンパイル → 実行、の順で復旧させる。動かない箇所は Phase 1〜3 で整理した構造の上で修正する。
- T1.1 の耐久スモーク (chibicc コンパイルループ) を clang 版に差し替え/併設し CI 化。
- 受け入れ: clang でのコンパイル 10 回連続成功、ページリークなし。これが本リファクタリング全体の完了基準。

### Phase 5 — プロトコル刷新

**T5.1 `_v2` 廃止と envelope 統一**
- 7 本の `*_v2.h` を suffix なしに改名し、request/response の共通ヘッダ (op, seq, status, payload len) を `libipc` に 1 定義。op 番号は境界ごとに 0 から振り直し (互換不要)。`pacha_docs/userland-service-abi-v2.md` を書き換え。
- 受け入れ: 全スモーク通過。grep で `_v2` が残っていない。

---

## 5. 実施順序まとめ

| 順 | タスク | 依存 |
|---|---|---|
| 1 | T0.1, T0.2 | — |
| 2 | T1.1 (最優先: clang 根治), T1.2, T2.1, T3.1 | Phase 0 |
| 3 | T1.3, T1.4, T2.2, T2.3, T3.2, T3.3 | 上記 |
| 4 | T3.4 → T4.1, T2.2 → T4.2, T4.3, T4.4, T4.5 | Phase 1–3 |
| 5 | T5.1 | 全部 |
