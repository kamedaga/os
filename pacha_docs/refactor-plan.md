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
Phase 4 の T4.1 は T3.3 に、T4.2 は T2.2 に依存。T4.5 (clang 耐久) は T1.1 の受け入れテスト。

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

**T3.0 pipe 安定化 (調査 → 再現スモーク → 修正)**
- 現象 (2026-07-09 ユーザー報告): busybox でのパイプ利用が全然動かない・不安定。clang はパイプ多用のため本リファクタの目標に直結する。
- 再現スモークを `tests/` に新設: 多段パイプライン (`ls | grep | wc`)、パイプバッファ超えの大量データ、`yes | head` (EPIPE/SIGPIPE)、writer close → reader EOF 伝播、O_NONBLOCK、dup2 での stdio 差し替え、fork 越しの pipe fd 継承、をカバーし失敗モードを分類・記録する。
- 構造的疑い: LPR が fd 種別ごとに別シャドウテーブル (`lpr_pipe_fds` / socket / eventfd / filed fd) を持ち相互 negative check で判別している分裂構造。これが原因の失敗は、kind タグ付きの単一 fd table への統合 (T3.3 の前倒し) で直す。
- kernel pipe (`state/pipe.zig`) 側の blocking/wakeup/EOF バグはその場で修正 (kernel 編集許可済み)。
- 受け入れ: 新パイプスモーク全ケース通過 + 既存スモーク 3 本通過。以後このスモークを標準検証セットに加える。

**Phase 3 実行順の改訂 (2026-07-10, ユーザー判断)**: T3.0 のバグ探索は部分修正 3 件 (17bebd0, 94f1cd9) の後で一時停止。残る症状 (16K+ パイプ停止 / grep -q 後のシェル停止 / 実行反復での OOM 劣化 / loader 非決定失敗) はサブシステム横断で「共有状態の腐敗が別々の顔で見えている」パターンであり、未リファクタの LPR 構造 (fd シャドウテーブル分裂、.inc、static 状態) が原因を見えにくくしている。よって **T3.1 (機械的変換) → T3.3 (状態モデル再設計) を先に行い、その上で T3.0 の残バグに戻る**。

判定基準はタスク種別で分ける:
- **移動系 (T3.1)**: green は green のまま、red は同一の red のまま (症状が変わったら挙動を変えた兆候)。
- **再設計系 (T3.3)**: 挙動保存を目的にしない。目標意味論は古いコードではなく **Linux の fd/pipe セマンティクス**。red スモーク (pipe-stress CASE3+, gnu CASE2) の green 化が受け入れ基準の一部。元の状態管理が酷い部分を挙動ごと温存しても改善しないため、根本から作り直す (ユーザー判断)。

**結果 (2026-07-10, aea43e9 で T3.0/T3.3 完了)**: 構造先行の判断は正解だった。再設計の過程で点修正では到達不能だった泥バグ 5 件を発見・修正: ① teardown/kill/exec 時の pipe peer wake 欠落 ② fork 子の RPC scratch VMO 共有破壊 ③ filed IO reply の payload 上限超過 memcpy (「clone 子 fault」の正体) ④ legacy RPC の reply result 上書き → read/write が偽 0/EOF (両 CASE3 ハングの共通真因) ⑤ プロセス終了時の FILED handle 全リーク → 256 枠枯渇 (Bug D = 反復 OOM の実体)。Bug C (loader) は③で解消。全スモーク green (pipe-stress 5 反復 40 ケース、gnu 全ケース + loader 10 回、state-leak 50 回)。

**T3.1 syscall dispatch の table 化 + .inc 廃止**
- 3 つの switch を `{nr, handler}` の単一テーブルに。`lpr_vfs/*.inc` 等の textual include を通常の .c/.h に変換 (build script 更新込み)。
- 受け入れ: 全スモーク通過。ENOSYS トレース (T0.1 経由) が syscall 名付きで出る。

**T3.2 daemon RPC クライアント共通化 `lpr_rpc`**
- wire page 確保/破棄・call・reply 検証・errno 変換の定型を `lpr_rpc.{c,h}` に集約。lpr_filed / lpr_socket / lpr_tty / lpr_process の各クライアントを移行。
- 受け入れ: 全スモーク通過。コード量削減を PR に記録。

**T3.3 LPR 状態モデルの再設計 (旧 T3.3+T3.4 統合)** ※挙動変更あり — 目標意味論は Linux
- 改訂 (2026-07-10, ユーザー判断): 元の状態管理が酷い部分は挙動保存の移動では改善しない。根本から作り直す。
- **設計を先に書く**: 実装前に `pacha_docs/lpr-state-design.md` として目標モデルを文書化し、それに沿って実装する。
  - **単一 fd table**: fd 種別ごとのシャドウテーブル (`lpr_pipe_fds` / socket / eventfd / filed fd / tty) と相互 negative check を全廃し、kind タグ + kind 別 payload を持つ 1 枚の fd table に統合。fd の判定・dup/dup2・close・CLOEXEC はすべてこの 1 枚の上の一様な操作。
  - **明示的ライフサイクル**: open / dup / dup2 / pipe / socket 作成、fork 時の snapshot serialize、execve 時の継承、close の各遷移を単一モジュールが所有。lpr_supervisor の常時ミラー (`lprs_filedesc_t` 更新経路) は廃止し、fork/exec の瞬間の snapshot 転送 (`supervisor_fd_snapshot.c`) だけにする。
  - **状態の集約**: 残りの file-scope static (rlimits, umask, file_map_cache, vfs cache…) を `lpr_state_t` に集約。thread-safe 化 (lock 導入) は T4.1 の前提として fd table と mmap 経路に futex ベースで入れる (single-thread fast path 維持)。
  - **可観測性を設計に含める**: fd table 全体と各 kind の状態を 1 回のトレースイベント列でダンプできる debug 機能 (`lpr_state_dump()`) を最初から持たせる。T3.0 残バグの診断に使う。
- 受け入れ: green スモーク維持 + **red スモークの green 化または「診断された理由付きの red」** (pipe-stress 全ケース×5反復, gnu-coreutils 全ケース)。fd-pipe / ext4 通過。ベンチ非劣化 (±10%)。

### Phase 4 — clang / mesa3d ギャップ

**T4.1 CLONE_THREAD 対応** (依存: T3.3)
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

**結果 (2026-07-10, 3a1f3c3 + _kobox 47b3181 で完了)**: 受け入れ達成 — compile→run 10/10 成功 (guest 65秒 ≈ 6.5秒/回)、LPR 状態 [open=4 live=1] 前後一致、kobox object table は [used=256 ref=67 cached=189 evictions=5] に収束し eviction の実動作を初確認。前提だった koboxd 64 枠枯渇は 256 枠 + refcount/LRU eviction で解消。cold `clang --version` は当初 219 秒 → **guest 4 秒 / host 8 秒**。真因は全て _kobox 側の 4 件: ① NVMe completion 毎の無条件 nanosleep(50µs、PachaOS 実測 6ms) — 44,223 read × 6ms = 265 秒 ② IRQ wait の内部 poll が 450 万 syscall → CQ spin pre-poll で 20.8→1.86 秒 ③ ext4 extent metadata の生 read が I/O を 44,223 回に細分化 → buffer cache 利用で約 575 回 ④ PRP list の非整列 calloc で大 read の後半がゼロのまま完了扱い (DSO ロード後 fault の真因)。泥バグ 8・9 匹目も本タスクで発見・修正: ⑧ readlink/symlink payload (8,176B) が service header 64B シフトで wire page 8,192B を 48B あふれ → LPR heap 破壊 (140 exec 後に顕在化) ⑨ clone が child_stack≠0 でも child_frame.rsp を設定せず → posix_spawn 子が親 stack から戻り先を pop して GPF (link 段 exit 13 の正体)。sigaltstack は ENOSYS のまま clang 動作に支障なし (正実装は T4.6 の signal 配送とセット)。スモークは clang cold-measure / endurance を加えた 11 本体制。

**T4.6 [kernel] 非同期シグナル配送 (CPU-bound プロセスへの割込み)**
- 現状 (2026-07-09 ユーザー情報): signal は syscall 待機中のプロセスにしか届かず、CPU-bound 実行中のプロセスには Ctrl-C も kill も効かない。バグではなく未実装。
- timer interrupt からのユーザー復帰時に pending signal をチェックして配送する経路を kernel に追加。SIGKILL は handler なしで即終了、それ以外は LPR の signal 経路 (lpr_supervisor) へ。
- clang (Ctrl-C 中断)、timeout ベースのツール、mesa のウォッチドッグ系に必要。
- 受け入れ: CPU-bound ループ (`while :; do :; done` 相当) が Ctrl-C / kill / busybox timeout で確実に止まる QEMU スモーク。

**結果 (2026-07-10, 7b82bef で完了)**: 受け入れ達成。kernel は timer interrupt のユーザー復帰時に pending signal を claim し、GPR/IRET/FX state を 704B native frame へ保存して登録済み LPR entry に redirect する機構のみを持ち、policy (disposition/mask/siginfo/ucontext/sigaltstack/rt_sigreturn) は全て LPR 側 (lpr_signal.c 新設)。SIGKILL は kernel 即 teardown。syscall 12 は旧 pull 型 process_consume_signal を process_signal_ctl に非互換置換 (帯維持・シフトなし)。sigaltstack 正実装 (SS_ONSTACK 検証込み)。async-signal スモーク (5 ケース) + termd pgrp queue unit を新設し 12 本体制。全検証 green、clang cold guest 4 秒維持。既知の別件: PTY teardown 時 session_clear_tty の task->signal NULL deref (fault 0x1a0、証拠取得済み・未修正)、real-time signal queue / stop/continue 完全 semantics / SS_AUTODISARM は範囲外。pacgo qemu-test は --send 全送出後に --expect 開始のため対話的 Ctrl-C (0x03) の phase 制御は不可 → kill/timeout + termd 内部 pgrp 生成テストで代替。

### Phase 5 — プロトコル刷新

**T5.1 `_v2` 廃止と envelope 統一**
- 7 本の `*_v2.h` を suffix なしに改名し、request/response の共通ヘッダ (op, seq, status, payload len) を `libipc` に 1 定義。op 番号は境界ごとに 0 から振り直し (互換不要)。`pacha_docs/userland-service-abi-v2.md` を書き換え。
- 受け入れ: 全スモーク通過。grep で `_v2` が残っていない。

**結果 (2026-07-11, 435fcef で完了 — 本計画の全タスク完了)**: 受け入れ達成。共通 envelope は libipc `service_abi.h` の `pacha_service_envelope_t` (64B、request/reply を union 表現、wire layout 不変) に一本化し、各サービスの重複ヘッダ・magic・version 定数を削除。`_v2` ヘッダは実際には 8 本 (koboxd が control/storage の 2 本) で、全て suffix なしへ改名、identifier の `_v2`/`_V2` 全除去、op 番号は境界ごとに 0 から振り直し (filed 0-46 / storage 0-18 / termd 0-14 / lpr-supervisor 0-19 / netd 0-6 / coordinator 0-7 / kobox 各系統)。kobox 拡張 transport は 96/80B の別 layout のため構造維持で magic/version のみ共通化。suffix 除去で露呈した filed の flag enum 重複は flags.h に集約、seed0root の関数名衝突は page/service call に分離。仕様書は userland-service-abi.md へ改名・全面更新。`git grep '_v2\|_V2'` (userland/kernel/tests/pack/musl/pachaos) 0 件。全検証 green (QEMU 12 本 + kernel zig test + filed VFS/tmpfs/cache + termd unit + ABI layout)、clang cold guest 4 秒・endurance 10 回 guest 64 秒維持。

---

## 5. 実施順序まとめ

| 順 | タスク | 依存 |
|---|---|---|
| 1 | T0.1, T0.2 | — |
| 2 | T1.1 (最優先: clang 根治), T1.2, T2.1, T3.1 | Phase 0 |
| 3 | T1.3, T1.4, T2.2, T2.3, T3.0→T3.1→T3.3 (状態モデル再設計)→T3.0再開, T3.2 | 上記 |
| 4 | T3.3 → T4.1, T2.2 → T4.2, T4.3, T4.4, T4.5 | Phase 1–3 |
| 5 | T5.1 | 全部 |

---

# 第二章 — グラフィクス: QEMU window 復活から Sway + Wayland まで

第一章 (Phase 0〜5) で clang 耐性は達成した。第二章の最終目標は **QEMU window 上で Sway (wlroots) が起動し、Wayland ミニアプリが表示・操作できる**こと。レンダリングは mesa **llvmpipe** (ソフトウェア)。GPU パススルーは対象外。**osmesa / offscreen への矮小化はしない** — 各 Phase は必ず「window に何かが見える」形で進み、画面という実物で段階検証する。第一章と同じ方式: 調査と修正を分離、証拠なき修正はしない、実装は codex に委任、タスクは実測から分解して追記。

自動検証の方針: window は手動確認、CI/スモークは QMP screendump (または同等) で framebuffer をキャプチャしピクセル値を検証する。serial マーカー方式は従来どおり併用。

既知の資産 (2026-07-11 調査): kernel は limine framebuffer を `primary_display` (paddr/width/height/pitch, kernel/src/boot/init_setup.zig) として publish 済み。pacgo には `Display` オプションの配管が既にあり現状 "none" 固定 (pack/internal/qemu/qemu.go)。kobox の virtio モジュールは net 系のみ導入済み (tools/download_kobox_virtio_modules.sh)、GPU/input は未導入。

```
Phase M2  DRM/KMS (/dev/dri/card0、dumb buffer で modeset 描画、window 復活込み)
Phase M3  mesa llvmpipe + GBM/EGL (kmscube 相当が window に出る)
Phase M4  入力: virtio-input/evdev → libinput、seat 管理
Phase M5  wlroots/Sway + Wayland ミニアプリ (章の完了基準)
```

(Phase M1 は完了済み: PTY teardown 安定化 186a71f / _kobox 658d5ac。framebuffer テストパターンは過去に実施済みのため独立タスクにせず、QEMU window 復活は M2.2 に含める — 2026-07-11 ユーザー判断)

### Phase M2 — DRM/KMS

**M2.1 /dev/dri/card0 の成立**
- kobox に DRM デバイスを立てる。候補は simpledrm (boot framebuffer 由来、依存最小) と virtio-gpu (本物の KMS/複数平面)。両者の .ko 依存関係と kobox shim の不足を調査し、根拠付きで選定 (最終的に Sway/wlroots が要求する ioctl 群を満たせる方)。/dev/dri/card0 を LPR プロセスから open できるところまで配線。
- 受け入れ: fixture が card0 を open し DRM_IOCTL_VERSION / GET_CAP が返る。

**結果 (2026-07-11, fb2445b + _kobox 0249fd3 で完了)**: 受け入れ達成 (open + VERSION=virtio_gpu 0.1.0 + GET_CAP(DUMB_BUFFER)=1)。選定は virtio-gpu — simpledrm は simple-framebuffer platform device への配線 (Limine fb の kobox 側 map) が未成立で M2.1 の境界を越える一方、virtio-gpu は既存 virtio PCI スタックに virtio_dma_buf を足すだけで kernel 変更不要、MODESET/ATOMIC/dumb/page-flip/event が wlroots 要件に直結。実装は termd の tty island と同型の**専用 drmd プロセス新設** (DRM module stack と file handle を隔離、filed は endpoint 受け渡し、LPR は FD 表現 + ioctl marshalling のみ)。Arch 6.8 実測レイアウトを根拠に DRM core shim を実装、VERSION/GET_CAP は実状態を返す (no-op 偽装なし)。固定 endpoint 挿入で LPR image ABI 更新 (互換分岐なし)。発見・修正 2 件: boot scratch 16MiB 超過 (drmd の section GC/-Oz で解消)、supervisor boot config の fd 245 衝突 (246 へシフト + ABI 一意性 assert)。スモークは drm-card0 を加えた 14 本体制。全検証 green、clang cold guest 5 秒。M2.2 残作業: MODE_* ioctl 群 / master / resource 列挙 / dumb create・map / DRM mmap の VMO 返却 / commit・scanout / event queue・vblank IRQ / GEM・fence helpers の実状態化 / window 復活との統合。

**M2.2 QEMU window 復活 + dumb buffer modeset 描画**
- pacgo の `Display` オプションを `pacgo run` / `pacgo qemu-test` から指定可能にし、window あり起動を復活させる (既定は従来どおり headless)。QMP screendump によるピクセル検証をスモーク手段として新設し、以後の Phase の標準検証にする。
- KMS dumb buffer + modeset でグラデーションを表示する fixture (kmscube 以前の modeset-test 相当)。mmap した dumb buffer への書き込み → page flip まで。
- 受け入れ: window に表示 (手動確認) + screendump ピクセル検証 green + 既存回帰 green。

**結果 (2026-07-11, 55badbe + _kobox 3c8ca07 で完了)**: 受け入れ達成。pacgo に `run` alias / `--display` / `--screendump-check 'MARKER@X,Y,W,H=#RRGGBB[:TOL]'` / `--screendump-device` を追加し、QMP screendump + P6 PPM 矩形ピクセル検証を標準基盤化 (headless でも display surface を維持しキャプチャ可)。firmware VGA と virtio-gpu の併存で QMP 既定 console が VGA を掴む問題は virtio-gpu に id=pachagpu を与えて明示指定で解決 (-vga none は Limine fb 喪失のため不採用)。KMS は legacy 先行で master / 列挙 / dumb create・map・destroy / ADDFB(2)・RMFB / SETCRTC / 同期 PAGE_FLIP / VMO FD 転送 + mmap / 実 virtio-gpu resource・scanout・flush まで実装。責務: drmd = KMS/dumb/FB/scanout の実状態所有、_kobox = device 取得境界のみ、LPR = ioctl marshalling + VMO mmap、kernel 変更なし。drmd IPC payload 512→3072B。kms-modeset スモーク新設 (グラデーション 2 枚、SETCRTC→PAGE_FLIP、screendump 赤→シアン検証)。スモーク 15 本体制、全検証 green、clang cold guest 5 秒。window 手動確認: `.artifacts/bin/pacgo run --display 'gtk,show-tabs=on' --new-terminal`。M3 残: atomic property/blob・client caps・state validation / page-flip event・vblank IRQ・event queue / hotplug・EDID・動的 mode / GEM shmem helper 完全化・reservation/fence・PRIME/dma-buf/GBM。

### Phase M3 — mesa llvmpipe

**M3.1 mesa 導入と実行棚卸し**
- alpine の mesa (llvmpipe / GBM / EGL) を rootfs に導入し、EGL+GBM で GL 描画 → KMS 表示する最小プログラム (kmscube 相当) を実行して、落ちる箇所・未実装 syscall・不足機能を証拠付きで棚卸しする。**このタスクでは修正しない** (T3.0 の教訓: 点修正は棚卸しを汚す)。
- 予想される領域: llvmpipe worker thread プール、大規模 mmap、shm/memfd、dma-buf、udev 情報の取得経路。
- 受け入れ: 棚卸しレポート (問題・証拠・タスク分解案) → M3.2 以降として本計画へ追記。

**結果 (2026-07-11, working tree / commit なし)**: 製品機能を修正せず受け入れ達成。Alpine v3.22 の Mesa 25.1.9 (`mesa-gl`, `mesa-egl`, `mesa-gbm`, `mesa-gles`, `mesa-dri-gallium` と runtime 依存閉包) を clang と同じ LLVM 20.1.8 資産を共有する増分 root overlay として導入した。増分は runtime data (`drirc.d`, PCI IDs 等) を含む 344 files / 155,739,272 bytes (`du` 150 MiB)、guest fixture binary + script を含む rootfs publish 増分は 155,769,172 bytes、最終 rootfs は 837,685,813 bytes。BOOTFS は 15,927,512 bytes で 16 MiB 制約内 (849,704 bytes 余裕)、Mesa は rootfs のみで boot scratch を消費しない。

resolver が展開した runtime package は `mesa`, `mesa-gl`, `mesa-egl`, `mesa-gbm`, `mesa-gles`, `mesa-dri-gallium`, `libdrm`, `llvm20-libs`, `spirv-tools`, `libpciaccess`, `libxau`, `libxdmcp`, `libxshmfence`, `libgcc`, `libstdc++`, `libxml2`, `libffi`, `libexpat`, `libbsd`, `libxxf86vm`, `libxext`, `libxcb`, `zlib`, `xz-libs`, `libmd`, `libx11`, `zstd-libs`, `wayland-libs-server`, `wayland-libs-client`, `libelf`, `hwdata-pci`。clang overlay と同一 byte の LLVM/libc 周辺 file は重複 publish せず共有し、`musl` package は既存 LPR runtime と衝突するため closure から除外した。build-only は `mesa-dev`, `libdrm-dev`, `linux-headers` で rootfs には publish しない。

fixture `lpr_mesa_inventory` は target `a`〜`e` を持ち、最終 target `e` は一つのプロセスで全段を通る。実測は以下。

| 段 | 到達点と証拠 | 判定 |
|---|---|---|
| a | `/dev/dri/card0`、`DRM_VERSION name=virtio_gpu 0.1.0`、`gbm_backend=drm`、status 0 | GBM device 作成成功。`MESA-LOADER: failed to retrieve device information` は毎回 5 回出るが非致命。 |
| b | `eglGetPlatformDisplay` は非 NULL / `EGL_SUCCESS(0x3000)`、`eglInitialize=1.5`、vendor=`Mesa Project`、40 configs 中 XRGB8888 (`0x34325258`) を選択、ES2 context 作成成功 | EGL/GBM platform と context 作成まで成功。 |
| c | 1024x768 XRGB8888、`GBM_BO_USE_SCANOUT|RENDERING` の GBM surface と EGL window surface 作成成功 | surface 宣言時点では PRIME 不要。 |
| d | renderer=`llvmpipe (LLVM 20.1.8, 128 bits)`、OpenGL ES 3.2 Mesa 25.1.9、triangle 中央 `RGBA=128,63,64,255`、GL error 0、`eglSwapBuffers=1` | softpipe/swrast への降格なし。初回 cold paging は20秒を超えたが90秒窓で成功し、warm fresh boot は約11秒で e まで完走。 |
| e | GBM BO handle=1/2、stride=4096、1024x768。`gbm_bo_get_fd=-1 errno=25 (ENOTTY)` のまま ADDFB2→SETCRTC→同期 PAGE_FLIP(flags=0) 成功。frame 2 marker=`#00ffff`、QMP `(8,8,8,8)` pixel check green | dumb handle 経路だけで表示可能。PRIME/dma-buf と page-flip event/vblank queue はこの最小 loop には不要。 |

追加対照として `LP_NUM_THREADS=2` でも renderer=llvmpipe、triangle、swap が成功した。したがって現 LPR pthread は llvmpipe の複数 worker を実動可能。default 実行も成功しており、worker pool 起因の停止は再現しなかった。大規模な libgallium/LLVM file-backed paging/JIT を含む default 描画で fault、ENOMEM、mmap failure はなく、現時点で mmap/物理メモリ量の機能 blocker は観測されない。ただし cold 初回は20秒超のため性能課題として残す。

実行中の LPR trace では x86_64 syscall 439=`faccessat2`、204=`sched_getaffinity`、99=`sysinfo`、285=`fallocate`、157=`prctl`、324=`membarrier` が ENOSYS になった。当初 324 を `mlock2` と記録したのは番号の取り違えであり、実引数 command=16 も membarrier registration と一致する。いずれも Mesa/musl の fallback 後に上記描画が成功したため M3 の機能 blocker ではない。runner の shell/tee 由来で 40=`sendfile` も ENOSYS だが Mesa 本体の壁とは分類しない。製品側の一時計測コードは追加していない。

明示判定:

- renderer: **llvmpipe が実選択**。`glGetString(GL_RENDERER)` の実値で確認し、softpipe/swrast 降格なし。
- GBM/PRIME: **kmscube 相当は dumb handle だけで足りる**。PRIME export は ENOTTY でも表示成功。ただし wlroots の dmabuf/modifier path には後続実装が必要になり得る。
- flip event/vblank: **単発・同期 loop には不要**。flags=0 の同期 PAGE_FLIP で表示成功。compositor の非同期 frame loop には未実装のまま。
- pthread: **問題なし (今回の負荷範囲)**。default と `LP_NUM_THREADS=2` の両方で llvmpipe draw/swap 成功。
- mmap/memory: **機能失敗なし、cold paging 遅延あり**。fault/ENOMEM/mmap error なし。初回だけ20秒 timeout では不足し、90秒で成功、cache warm 後は e まで約11秒。

問題棚卸し:

| 症状 | 証拠 | 推定領域 | 想定作業量 |
|---|---|---|---|
| Mesa loader の device information 取得失敗 | stage a〜e で同警告が各5回。ただし GBM/EGL/llvmpipe は成功 | LPR/filed の sysfs・device metadata または Mesa loader 設定 | S〜M: 要求 path/ioctl を trace し、実データ境界を決める |
| tolerated ENOSYS 群 | 439/204/99/285/157/324 の LPR ENOSYS trace、直後も描画成功 | LPR syscall、`fallocate` は filed も関与 | M: fallback 依存を一つずつ仕様化・実装・unit 化 |
| PRIME export 未実装 | `gbm_bo_get_fd=-1 errno=25`、一方 handle 1/2 の ADDFB2 は成功 | drmd DRM ioctl + LPR marshalling。kernel は既存 fd/VMO 転送で不足が証明されるまで対象外 | L: dma-buf lifetime/rights、PRIME handle↔fd、modifier/GBM test |
| page-flip event/vblank 未実装 | flags=0 は成功、event queue は fixture が明示的に未使用 | drmd + LPR poll/read event queue + _kobox display IRQ | L: IRQ/vblank state、event ownership、poll/read、耐久 test |
| rapid multi-process probe の master handoff race | a〜d を別 process で直列実行した初回だけ、次の e で `drmSetMaster=-1 errno=16`。fresh single e は status 0 | drmd handle close/master ownership と LPR process teardown ordering | M: close completionを証明し restart smoke を追加 |
| cold Mesa paging が20秒超 | 初回 d は20秒 KILL、同一 artifact の90秒実行は成功、warm fresh e は約11秒 | filed VMO/page cache、LPR file-backed mmap、LLVM/Mesa cold footprint | M: profile-only で内訳採取。機能変更は測定後 |

導入例外として行ったのは (1) fixture build-only sysroot への `linux-headers` 追加 (`drm.h` が `linux/types.h` を要求)、(2) rootfs symlink sentinel をホスト linker に渡さず実体 versioned `.so` を使う link 手順、(3) 既存 LPR musl と衝突した Mesa dependency closure 内の `musl` 除外だけ。いずれも package/fixture 導入を成立させる変更で、guest 機能不足は修正していない。

最終回帰は新規 Mesa inventory+screendump、既存 QEMU 15 本 (drm-card0 / kms-modeset / pty-teardown / async-signal / fd-pipe / ext4-sync-persistence / gnu-coreutils / state-leak / pthread / shared-mapping / epoll / shell-interaction / pipe-stress `ITERS=5` / clang-cold-measure / clang-endurance) が全て green。`kernel zig build test`、filed VFS、termd pgrp signal unit、userland service ABI layout、`pack go test ./...` も全て green。clang cold は guest 5秒 / host 9秒で20秒制約を維持した。

**M3.2 DRM client teardown / master handoff**
- rapid compositor restart を模した open→GBM/EGL→close→即 reopen で master が同期的に移ることを証明する。drmd handle close と LPR process teardown の順序を修正対象とし、kernel へ移さない。
- 受け入れ: 連続 20 restart で SET_MASTER/SETCRTC が EBUSY/EACCES にならず object leak なし。

**結果 (2026-07-11, working tree / commit なし)**: 受け入れ達成。最小再現条件は fresh boot で Mesa stage `a` を1 processだけ完了させ、待ち時間0 msで別 processの stage `e`を起動することだった。修正前は最初の後続 `SET_MASTER` が `EBUSY`、fresh単発 `e`は成功する。待ち時間で解消するレースではなく、LPRのprocess-exit cleanupがFILED handleだけをcloseし、DRM handleを一度もdrmdへ送らないlifetime欠落だった。原因traceは、pid=2終了直前にfd 3/4が同じDRM handle=1 (refcount=2)を保持 → drmdにclose到達なし → 次processがhandle=2をopenしてもmaster=1・active handles=2 → `SET_MASTER(handle=2)=-16`、という列を記録した。通常の`close(2)`は元から同期RPC完了後に返るため、修正は`lpr_linux_prepare_process_exit()`のobject walkへDRM backend handleの同期closeを1分岐追加しただけで、kernel/ABI/_kobox変更はない。一時計測は全除去済み。

restart smokeを新設し、Mesa DSOをロード済みのparentから20 childをfork/waitで直列起動する。各childは独立processとしてopen → GBM device作成 → EGL 1.5 initialize → SET_MASTER → 1024x768 dumb/FB作成 → SETCRTC → application cleanup → card0をprocess teardownに残して終了し、child間sleepは0 ms。20/20でEBUSY/EACCESなし、drmdの各close完了後20回すべて`handles=0 fb=0 dumb=0 master=0`、先頭handle=1と末尾handle=20の前後状態一致をrunnerがserialから機械検証した (host 18.8秒)。20回を別々にcold execする予備fixtureは9〜11回目に3 MiB DMA VMOの物理連続化が`PACHA_ERR_INVALID`となる別のcold-exec断片化を露呈したため、master handoff検証からDSO pagingの交絡を除いた。これはdrmd count/IOVA回収とは別で、M3.6のcold-path計測候補とする。既存QEMU 16本、kernel test、filed VFS、termd unit、service ABI layout、pack Go testは全green。clang coldはguest 5秒 / host 9秒で20秒未満維持、mesa-inventoryはhost 25秒でgreen。

**M3.3 page-flip event / vblank / DRM event queue**
- async PAGE_FLIP_EVENT、vblank/display IRQ、per-file event queue、poll/read を実状態で実装。同期 flip fallback は作らない。
- 追加観測 (2026-07-11 ユーザー手動確認): 三角形 (単発フレーム) は window に表示されたが、cube 系の連続アニメーションは表示されず青い背景のみ。kmscube 相当は PAGE_FLIP_EVENT 待ちでフレームループを回すため event queue 未実装と符合するが、原因は本タスクで証拠を取って確定すること (断定しない)。
- 受け入れ: event sequence/user_data/timestamp を検証し、1000 flip 耐久 + screendump green。**回転 cube 相当の連続アニメーションが window に実表示されること**。

**結果 (2026-07-11, working tree / commit なし)**: 受け入れ達成。製品修正前に Mesa/GBM 回転 cube fixture を作り、llvmpipe 初期化・初期 SETCRTC 成功後、最初の `drmModePageFlip(..., DRM_MODE_PAGE_FLIP_EVENT, ...)` が `errno=22` で失敗し、poll には一度も到達しない証拠列を得た。現行 drmd の `flip->flags != 0` 拒否と一致し、今回の「青背景のみ」は poll 永久待ちや Mesa draw failure ではなく EVENT flip の submit 時 EINVAL と確定した。

実装責務は、drmd が logical DRM handle ごとの event ring、CRTC pending flip、sequence/timestamp と virtio-gpu fence lifetime を所有し、LPR が private drmd read/poll IPC の marshalling と Linux read/poll/epoll/select 配線を担当、_kobox が Linux 6.8 `dma_fence_init` の実 refcount/list/context 初期化と既存 capsule IRQ→virtio ISR→deferred controlq dequeue を提供する形に分離した。EVENT flip は同期 flip へ降格せず、TRANSFER_TO_HOST_2D→SET_SCANOUT→RESOURCE_FLUSH を実 fence 付きで submit して RPC を返す。virtio display IRQ 後も `fence_drv.last_fence_id >= submitted fence_id` になるまで event 化しない。Arch 6.8 module の一時 offset probe で `virtio_gpu_device.fence_drv=0xf278`、`virtio_gpu_fence.fence_id=0x48` を実測し、probe は削除済み。close は pending fence 完了を同期的に待ってから queue/FB/dumb/master を破棄し、restart oracle は各回 `handles=0 fb=0 dumb=0 eventq=0 events=0 master=0` を検証する。kernel/公開 kernel ABI 変更はない。private drmd protocol は既存 0〜5 に自然に続く READ=6/POLL=7 を追加し、全 producer/consumer を同時更新、互換 shim は置いていない。

新設 1 runner は常設既定 20 flip + cube 8 frame、受け入れ値は `DRM_FLIP_ITERS=1000 DRM_CUBE_FRAMES=8`。1000/1000 event で type/length/crtc_id、64-bit user_data、sequence 1→1000、timestamp 単調性を検証し、poll/epoll を交互使用、最終 magenta screendump green。実測は 17.775 秒 / 56.258 fps。cube は llvmpipe で 8/8 event、17.391 fps、画面中心 8x8 が frame 1 の赤面 `#ff0000` から最終 frame の緑面 `#00ff00` へ変化した。drm-restart 20/20、QEMU 17/17 (再実行0)、mesa-inventory、kernel test、filed VFS、termd pgrp、service ABI layout、pack Go test は全 green。clang cold は guest 5 秒 / host 9 秒で20秒未満を維持した。M3.4 では同じ logical-handle lifetime を PRIME/dma-buf file ownership に適用する必要がある。M3.6 に向けて raw fenced KMS が約56 fpsなのに cube が約17 fpsであるため、event/IRQ より Mesa/llvmpipe draw・swap/copy 側が次の性能計測対象と分かった。

**M3.4 PRIME/dma-buf + GBM modifier path**
- PRIME_HANDLE_TO_FD / FD_TO_HANDLE、dma-buf lifetime/rights、GBM modifier negotiation を実装。まず userland drmd/LPR 境界で設計し、kernel ABI 変更は既存 VMO/fd 転送で不可能と証明された場合だけ別途理由と許可を求める。
- 受け入れ: GBM BO export/import、cross-process lifetime、ADDFB2 modifier、close/error path を実 buffer で検証。

**結果 (2026-07-11, working tree / commit なし)**: 受け入れ達成。drmd の GEM handle を backing buffer と分離し、backing は handle ref / PRIME export ref / framebuffer ref のいずれかが残る限り生存する。PRIME export は backing ごとの token と共有 VMO FD を返し、LPR の dma-buf open-file object が token を所有する。同一 process の dup は open-file refcount を共有して最後の close だけが RELEASE、fork は child 分を ACQUIRE、exec は fd table ABI v4 / image ABI v7 の dma-buf descriptor と既存 native VMO FD を保持する。このため元 GEM handle と GBM BO を破棄した後も export FD から再importでき、M3.3 の logical-handle lifetime を file ownership へ拡張できた。VMO rights は MAP_READ/WRITE、TRANSFER、DUP、SET_FLAGS、CLOSE を生成元から転送の全段で明示的に保持する。

外部 memfd は Filed の既存 shared-file VMO を LPR→drmd IPC で渡し、drmd が backing/token 化する。Mesa llvmpipe が要求する `/dev/udmabuf` は LPR pseudo device として同じ import/export を使い、F_SEAL_SHRINK の追加・縮小拒否、dma-buf mmap/dup/CLOEXEC を実装した。DRM cap は PRIME import/export と ADDFB2 modifiers を返し、modifier は現 scanout 実装で正しい LINEAR のみを受理する。Linux ABI の `DRM_CLOEXEC=O_CLOEXEC` をそのまま解釈し、kms-swrast が `DRM_CLOEXEC` だけで作る export も read/write dma-buf として扱う。

新規 smoke は GBM `create_with_modifiers2(LINEAR)`→export、fork→exec した別 process の `GBM_BO_IMPORT_FD_MODIFIER`→再export→cyan 書込み、親で元 handle close後の再import、closed FD の EBADF、ADDFB2_WITH_MODIFIERS→SETCRTC、最終 pixel `#00ffff` を一つの実 1024x768 buffer で検証し host 12.1 秒で green。Mesa 25.1.9/current main の kms-swrast は FD_MODIFIER import 自体を受理する一方、displaytarget-backed llvmpipe resource の `gbm_bo_get_modifier()` を INVALID と返すため、その getter 値を OS shim で偽装していない。LINEAR 指定 import の成功、1 plane、LINEAR ADDFB2 と表示実体を oracle とした。kernel syscall/ABI/_kobox 変更はない。既存 VMO fd transfer、fork fd-table clone、shared mmap で export/import/write/display が成立したことが、kernel 追加不要の実証である。

**M3.5 Mesa loader metadata + tolerated syscall / cold-path hardening**
- loader warning の要求元を trace して sysfs/device metadata を実データ化。faccessat2/sched_getaffinity/sysinfo/fallocate/prctl は使用箇所と fallback 影響を測定して優先度順に実装し、cold 20秒以下を維持する。
- 受け入れ: loader warning 0、fallback ENOSYS 0、llvmpipe renderer/pixel/LP_NUM_THREADS=2 green、cold e 20秒以下。

**結果 (2026-07-11, working tree / commit なし)**: loader warning 0 と優先 syscall 2件を達成。libdrm 2.4.124 の要求実体は char dev 226:0 に対する `/sys/dev/char/226:0/device/drm` の存在、virtio subsystem と親 PCI subsystem の readlink、device realpath、PCI `uevent` の `PCI_SLOT_NAME`、vendor/device/subsystem/revision だった。QEMU/QMP で実測した `0000:00:04.0`, `1af4:1050`, subsystem `1af4:1100`, revision 1 を rootfs sysfs hierarchy に載せ、`drmGetDevice2(DRM_DEVICE_GET_PCI_REVISION)` が同値を返す fixture oracle を追加した。empty directory が pack manifest に入らないため実在する `drm/card0/dev=226:0` も収録した。LPR は `/dev/dri/card0` の char-device stat を返し、directory `newfstatat` は O_DIRECTORY retry、read-only directory open は過剰な READ/CREATE/REMOVE/RENAME を要求せず STAT/LOOKUP/GETDENTS に限定した。一時計測 trace は除去済み。

ENOSYS は二つの inventory run 合計で 204=`sched_getaffinity` 0、439=`faccessat2` 0。sched_getaffinity は CPUID topology と PachaOS/QEMU の上限4 CPUから 8-byte mask `0x0f` を返し、tid 0/current/LPR thread table を検証する。`sysconf(_SC_NPROCESSORS_ONLN/CONF)=4/4` を oracle 化し、従来 ENOSYS 時に musl が初期 mask `{1}` のまま1 worker相当になる影響を除いた。faccessat2 は観測 flags=AT_EACCESS を、現状 real/effective ID が同一という実条件で既存 faccessat pathへ正規に接続した。

非実装は 99=`sysinfo` 4 trace lines=実 call 2回、285=`fallocate` 4 lines=2回、324=`membarrier` 4 lines=registration 2回、157=`prctl` 16 lines=8回。sysinfo は musl sysconf fallback後も online/configured=4、fallocate は Mesa disk-cache preallocation fallback後も cache/draw成功、prctl は PR_SET_NAME と PR_SET_MM 系で thread naming/proc titleだけが欠け、membarrier は registration ENOSYS 後も llvmpipe draw/barrier/pixelが正しい。したがってこの4件は green の renderer=`llvmpipe (LLVM 20.1.8, 128 bits)`、triangle center `128,63,64,255`、cyan screendump、default/LP_NUM_THREADS=2 を根拠に実装価値を低いと判定した。mesa-inventory は host 15.8 秒、loader warning 0。なお 324 は mlock2 ではなく membarrier である。

指定回帰は PRIME、drm-card0、drm-restart 20、drm-page-flip既定20/cube8、kms-modeset、mesa-inventory、service ABI layout が green。restart は udmabuf open により logical handle 番号が iteration ごとに2進むため、旧「末尾handle=iterations」依存を外し、clean state 20回とserial最終closeの `handles=0 fb=0 dumb=0 eventq=0 events=0 master=0` を維持した。フル回帰は新方針どおりオーケストレーター実施待ち。

**M3.6 Mesa 実行速度の調査と大幅改善 (Phase M3 の締め)**
- 現状は棚卸し fixture の全段 (a〜e) が cold 20 秒超 / warm 約 11 秒で、常設スモークに入れるにも遅く、将来 WM (Sway) のフレームループとしては使い物にならない水準。第一章の clang 219 秒→4 秒と同じ方式で、推測せず計測から入る: cold/warm それぞれについて内訳 (Mesa/LLVM DSO の file-backed paging、llvmpipe の JIT compile、filed VMO/page cache、drmd 転送、KMS submit) を数字で確定してから、支配的要因だけを最小 diff で潰す。
- 想定候補 (計測で確定するまで仮説扱い): DSO cold paging (clang で実績のある filed file-VMO cache の適用範囲)、llvmpipe shader JIT の初回コスト、LP_NUM_THREADS と実コア数の整合、drmd 経由の buffer 転送コピー回数、dumb buffer mmap の書き込み経路。
- 受け入れ: 計測レポート (内訳と改善前後の数字) + warm の描画ループ (draw→swap→flip 1 フレーム) が interactive 水準に近づくこと + mesa inventory 相当スモークが常設バッテリーに入れられる実行時間 (目安: warm 全段数秒台) になること。cold も clang 同様の大幅短縮を狙う。既存回帰全 green 維持。

**結果 (2026-07-12, working tree / commit なし)**: 一時計測で同一 cold/warm run を比較した。inventory e は cold 24.11→21.51秒 (-2.60秒, -10.8%)、warm 7.65→5.29秒 (-2.36秒, -30.8%) となり、warm 数秒台を達成した。LPR は各 process で file mapping 140回 / 453,419,008B、process-local cache hit 54 / miss 86。従来 hit 36回は cached VMO を shared source として map し、別の anonymous VMO へ 143,671,296B memcpy しており、cold 2.553秒 / warm 2.518秒を消費していた。cached VMO を Linux `MAP_PRIVATE` 相当の既存 COW mapping として直接 map し、実行 segment だけ既存 patch 後に最終 protection へ戻すことで、この copy を 0回 / 0B / 0秒にした。mapping 全体は cold 4.181→1.839秒、warm 3.360→0.752秒。filed の cached VMO は private writable/executable mapping に必要な native map rights を LPR へ transfer するが、cache 本体は COW で不変、Linux fd や ABI/wire layout の変更はない。kernel 変更もない。

filed cold paging は Mesa/LLVM load 区間で file-VMO miss/store 59、backend pread 104回、約230.0MiB、backend read 約0.889秒で、warm は file-VMO hit のみで大 read 0。file-VMO RPC は cold 約1.56秒 / warm 約0.79秒、native mmap 176回は 12〜28ms、VMO create は0〜2ms、mprotect は1〜10msだったため、I/O や native syscall は支配項ではなかった。修正後の cold 残差は `eglCreateContext` 10.243秒、shader compile/link 2.283秒、first draw/finish 4.236秒、warm は各0.857 / 0.335 / 0.501秒であり、LLVM/llvmpipe 初回 context/JIT が支配する。cold 10秒未満には届かず21.51秒だが、残りを file cache や sleep 除去で短縮できる証拠はなく、JIT cache/precompile方式は M4/M5 で実 workload を測ってから設計する。

20 frame の層別計測では、明示 `glFinish` ありは draw 32.25ms/frame (うち finish 20.90ms)、EGL swap 0、KMS submit IPC 1.70ms、event wait 22.50ms。冗長な `glFinish` を除くと draw 6.45ms、EGL swap 23.15ms、KMS submit IPC 1.35ms、event wait 22.05msとなり、draw+swap は32.25→29.60ms (-8.2%)、計測 build の fps は14.378→15.396 (+7.1%)。drmd IPC がコピーするのは ioctl/event の小さい control payload だけで、framebuffer pixels は transferred VMO/shared mapping、pixel payload copy は0回。したがって displaytarget→scanout copy は EGL swap 内の約23.15msが残る。最終 raw 1000 flip は58.948 fps、cube 8 frame は M3.3 の17.391→19.559 fps (+12.5%)、赤面→緑面 screendump と1000/1000 eventが green。cube は描画/device close後かつprocess exit前の Mesa/libc cleanup区間で停止したため、成功条件とdrmd clean closeを完了してから fixtureを `_Exit` させ、runner DONEまで成立させた。

M3.2 の separate cold exec は、従来 DSO private mapping用 anonymous VMOを先に約143.7MiB実体化して物理 allocatorを断片化し、その後の3MiB DMA連続割当が9〜11回目で `PACHA_ERR_INVALID` になっていた。direct COW化後は別 process cold restart 20/20 (問題区間を含む) が成功し、INVALID 0、各回 `handles=0 fb=0 dumb=0 eventq=0 events=0 master=0` へ収束したため、kernel allocatorを変更せず解消した。

clang cold は guest 2秒 / host 6秒で20秒未満。endurance は最終 affinity 有効の2 runが guest 78 / 75秒 (host 125 / 126秒) で、単発97秒は再現しなかった。過去64〜65秒との差を切り分けるため、M3.5の唯一のclang到達候補 `sched_getaffinity` だけを一時ENOSYSへ戻したA/Bも guest 78秒 / host 126秒で差なし。M3.4 PRIME/dma-buf分岐はclangのfd種別では非到達であり、M3.4/M3.5起因の回帰ではない。全3 runは各 iteration 7〜8秒、open/live `[4,1]` 前後一致、kobox used=256 / eviction=6で収束した。計時はguest wall clockにもhost vCPU schedulingを含むため残る64→75〜78秒差はホスト実行条件の残差として記録し、97秒を製品回帰とは判定しない。

最終恒久実装差分は filed cached VMO map rights、LPR cached mappingのprivate COW化、cubeの重複`glFinish`除去と検証後`_Exit`だけ。一時計測、filed累積log、cold専用restart mode、affinity A/Bはすべて除去した。指定回帰は mesa-inventory、raw1000+cube8、drm-restart 20、kms-modeset、clang-cold、clang-endurance 2回が green。M4/M5へ残す性能リスクは cold llvmpipe context/JIT 約16.8秒、displaytarget→scanout copy 約23ms/frame、vblank待ち約22ms/frame、wlroots実 workloadでのmulti-surface/resize時copy増幅、4 worker時のgraceful Mesa cleanup停止である。

**2巡目結果 (M3.6b, 2026-07-12, working tree / commit なし)**: 同一 host、QEMU q35/KVM、2GiB、4 vCPU、`virtio-gpu-pci`、1024x768、同じ fixture ELF と PachaOS rootfs の Mesa 25.1.9 / LLVM 20.1.8 DSO を Alpine virt 3.22.5 guest から read-only mount して Linux 基準値を取得した。Alpine の最新再実行は inventory e 1.534秒（fixture固定sleep 1秒を含む）、`gbm_create_device` 0.255秒、`eglCreateContext` 0.162秒、shader compile/link 0.034秒、first draw/finish 0.056秒、raw flip 1000回 2353.327fps、cube 100 frame 287.791fpsだった。cube内訳は draw 0.36、EGL swap 1.49、KMS submit 0.02、event wait 1.10ms/frame。直前runも total 1.523秒、context 0.156秒、raw 2294.065fps、cube 328.724fpsで同じ桁だった。LinuxがPachaOSより大幅に速いため、残差を「llvmpipe/QEMU固有」とする仮説は棄却した。Linuxのpage-flip event sequenceは0で即時完了する一方、PachaOS drmdは各flipで全1024x768の`transfer_2d`→`set_scanout`→`flush`を実fence付きで行い、fence完了後にeventを生成する差がある。

Mesa shader cache は追加実装なしで成立済みだった。cache-empty ext4でのPachaOS bootは inventory e 10.243秒（固定sleep 1秒込み）、`gbm_create_device` 0.285秒、`eglCreateContext` 6.318秒、shader 0.637秒、first draw/finish 1.134秒。終了後に`/home/.cache/mesa_shader_cache_db/{index,marker,part0..49/{mesa_cache.db,mesa_cache.idx}}`が生成され、rootfs再syncを挟まない次bootにも残った。cache温まり後bootは total 6.944秒、context 2.963秒、shader 0.646秒、first draw/finish 1.184秒で、目標の10秒未満を達成した。`fallocate` ENOSYS fallbackはcache生成・再利用を阻害しておらず、cache directory、xattr、永続化の追加修正は不要。開発testの`sync rootfs --force`またはtimeout後のdisk再作成はunmanaged cacheを消すので、実利用のboot間cache評価では同じdiskを再起動する必要がある。初回の残差はcontext初期化/JIT 6.318秒が支配し、shader cacheが効く2回目もcontext 2.963秒が最大だった。

frame計測は1巡目の「EGL swap 23.15ms = displaytarget→scanout memcpy」という分類を修正する。Mesa 25.1.9の`drisw_present_texture`→`swrast_put_image2`にはGallium flush/frontbuffer完了とrow memcpyの両方が含まれる。`SWRAST_NO_PRESENT=1`でrow copyを止めても swap 22.283ms/frame、通常22.050ms/frameで変わらず、23msの大半は単純memcpyではない。3MiBを64回コピーした独立計測は anonymous copy 1.78ms/回、既map dumb copy 1.23ms/回、毎回mmap+copy+munmap 8.92ms/回。そこでMesa dumb BOのmapをpresent間で保持する試作も行ったが、swap 20.78→20.15ms/frame、cube 25.773→25.531fpsで改善なしだったためrevertした。x86_64専用memcpy試作もswap差なしでrevertした。kms-swrast displaytargetをGBM dumb BOへ直接統合するにはMesa winsys/EGL surface ownershipの変更が必要で最小diffではなく、PRIMEだけではdrisw frontendのpresent copyを消せない。

event待ちはdrmdのservice loopが各周回でnon-blocking IRQ処理を行い、10ms poll周期を持たない。LPRのDRM poll/epoll sleepだけ10→1msにした候補は一度25.773fpsを示したが、同じQEMU条件の対照再測定では1ms時 wait 22.33ms/frame / 15.715fps、10ms時22.38ms/frame / 15.885fpsで差が再現せず、QEMUのraw cadence自体もrun間59〜75fpsで変動したため恒久化しなかった。したがって約22msはevent配送量子ではなく、drmdが発行したvirtio transfer/flush fenceの実完了待ちである。前flip中に次frameをdraw/swapする3-buffer試作も、通常の draw/swap/submit/wait=6.10/21.32/1.80/22.38ms、15.885fpsに対し9.25/24.40/1.32/18.20ms、15.337fpsとなった。GBM BO lock/renderer同期が待ちを内包してoverlapできずrevertした。damage付きEGL swapも15.715fpsで効果なしだった。

2巡目の恒久コード差分は0で、一時計測、poll量子、Mesa persistent map、memcpy、damage、pipeline試作はすべてrevertした。Linux baselineだけは`.artifacts/m3.6b-linux-baseline/`にISO、同じDSOを載せたPachaOS diskをread-only利用するtimed fixture、console log、`run_baseline.py`を残した。再実行はrepo rootで`python3 .artifacts/m3.6b-linux-baseline/run_baseline.py`。scriptは先頭で指定どおりQEMU kill + 1秒待ちを行う。最終指定回帰は mesa-inventory、raw1000+cube8、kms-modeset、drm-restart 20、clang-coldがgreenで、rawは58.806fps、cubeは21.857fps、clang coldはguest 2秒 / host 6秒。raw+cube初回は全frame/event/pixel成功後の既知exit停止でDONEだけtimeoutしたため、規定どおりdisk削除→再sync→単体再実行しgreenを確認した。kernel treeは無変更で、現buildに存在しない旧`efi` stepは失敗、正式な`zig build kernel`は成功した。結論は、cache温まり後coldは目標達成、cube 30fpsは未達。残る上限はLinux llvmpipeではなくPachaOSの全画面virtio `transfer_2d`/flush fenceとGallium frontbuffer flushであり、正しい次段はM4/M5のcompositor damage情報を標準dirtyfb/atomic damageへ接続して転送矩形を縮めるか、host-visible resource/scanout BOへ統合して二重転送を除くこと。graceful Mesa cleanup停止は従来どおりM5送りである。

**3巡目結果 (M3.6c, 2026-07-12, working tree / commit なし)**: 起動側だけを再診断し、frame fence経路は変更しなかった。Linux baseline の無計測再実行は inventory e 1.517秒（fixture固定sleep 1秒込み）、`eglCreateContext` 0.159秒、shader 0.034秒、first draw/finish 0.056秒、raw 2332.818fps、cube 289.473fpsで2巡目を再現した。Linux は同じ timed ELF の target a/b を Alpine `strace -f -c` で差分化した。strace overhead 下では context 0.355秒、syscall内訳差は合計約0.168秒で、`futex` 31回 / 0.135秒が最大、`mmap` +993回 / 約0.005秒、`munmap` +642回 / 約0.006秒、`lseek` +1462回 / 約0.006秒、`clone` 9回 / 0.0005秒だった。`clock_gettime` はvDSOなのでstrace上0 syscall。CPU topology/sysfsはcontext前に読み終わり、context区間のretry loopではなかった。

PachaOS は既存LPR syscall metricを一時有効化し、同一bootの `target a1 -> b -> a2` で後段a2をcontrolにした。diagnostic tableへ候補を一時追加した採取では、B 2.873G cycles、A2 2.032G cycles、差0.841G cycles（同じ計測buildのfixture 1秒sleepから約2.97GHz、約0.283秒）だった。増分は `mmap` +862、`munmap` +517、`open` +19、`mkdir` +4、`clone` +8、`mprotect` +14、`clock_gettime` +2。B平均で見た概算は mmap約0.205秒、fixture markerのwritev約0.057秒、mkdir約0.048秒、munmap約0.047秒、open約0.027秒、clone約0.006秒だが、共通stage A callのrun間差が相殺するためaggregate差0.283秒を採用する。Linux ABI `futex(202)` は0回で、LPR clone内部native futex待ち込みでもclone全体は約0.006秒。ENOSYSはcontext増分で `prctl` 4、`membarrier` 1、`fallocate` 1だが各約74〜111 cyclesで、retry支配ではない。`LP_NUM_THREADS=0/1/2/default` のcontextは776/790/793/805ms、default再測807msで、worker数も支配項ではなかった。一時計測のclass mask、metric table、Pacha musl試作は全revertし、kernel/ABI変更はない。

2巡目の2.963秒は新規source修正なしに再現しなくなった。normal LPR/muslをclean rebuildし、新規diskでcache-empty eを正常完走したrunは context 9.116秒、shader 2.035秒、draw 3.846秒、total 17.150秒。その同一diskの次boot以降は default context 0.805 / 0.807秒に収束した。旧diskは規定のtimeout復旧で破棄済みなので、旧値がpartial DB状態かstale runtime artifactかはこれ以上分離できず、source修正の効果とは主張しない。ただし新規diskからの再現手順で目標1秒未満を2回達成し、Linuxとの差は20倍から約5.1倍へ縮んだ。0.805秒のうちLPR metricで説明できるsyscall差は概ね0.283秒、残り約0.522秒がLLVM/llvmpipeのuser CPU処理・未計測同期残差である。これ以上詰めるにはperf相当のguest sampling profilerまたはnative syscall/thread scheduler spanが必要で、現状のsyscall高速化だけではLinuxの0.159秒には届かない。

温後 e は total 3.752秒、stage A 0.418、stage B 0.923（context 0.805 + 他0.118）、stage C 0.031、stage D 0.964（make current 0.065、shader 0.306、draw/finish 0.474、他0.119）、stage E 1.416（fixture固定sleep 1.000 + KMS/2nd draw/flip等0.416）。従来の「その他約2.1秒」は、固定sleep 1.000、stage A 0.418、stage E実処理0.416、stage B/C/Dの小残差0.333、合計2.167秒と確定した。固定sleepとstage Eはinventoryのscreendump/KMS oracleで製品起動処理ではなく、削除して成功値を作らない。高頻度syscall側にも単独1秒級項目はないため恒久コード差分0とした。目標は context 2.963 -> 0.805秒、warm e 6.944 -> 3.752秒で両方達成。

最終回帰は normal fixture/runtimeへ復元したfresh diskで mesa-inventory（default e + LP_NUM_THREADS=2 d、cyan screendump）、clang-cold（guest 2秒 / host 6秒）、drm-page-flip既定20 + cube8（raw 60.975fps、cube 19.370fps、event/pixel全green）、kms-modeset（red -> cyan）と `zig build kernel` がgreen。フル回帰はオーケストレーター実施待ち。恒久diffはこの3巡目記録だけで、計測ログは `.artifacts/m36c-*`、Linux strace summaryは `.artifacts/m3.6b-linux-baseline/linux-baseline-console.log` に残した。

**4巡目結果 (M3.6d, 2026-07-12, working tree / commit なし)**: フレーム側だけを再診断した。Linux baseline再実行は raw 1000回 2314.869fps、cube 100回 298.902fps、cube内訳 draw/swap/submit/wait=0.35/1.42/0.02/1.03ms/frame。Linux 6.8 sourceでは通常のprimary dumb flipは full-frame `TRANSFER_TO_HOST_2D`（fenceなし）→ framebuffer/resourceが変わる時の`SET_SCANOUT`→`RESOURCE_FLUSH`（fenceなし）をqueueし、vblank未初期化のためatomic helperがsubmit直後にfake-vblank event（sequence=0）を送る。QEMU traceでもfixtureのresource 3/4は初期modesetを含めtransfer/set-scanout/flush=501/501/501回と500/500/500回、fence event=0回で、1000 flip各1回を確認した。したがって2-buffer rawでSET_SCANOUTを省ける仮説は棄却した。

PachaOS旧経路は同じ3 commandのうちflushだけにvirtio fenceを付け、controlq順序により先行transfer/set-scanoutも完了した後、`last_fence_id`到達をIRQ→deferred work→drmd event化していた。Linuxに合わせてevent flipのfence allocation/待ちとpending-fence stateを削除し、3 commandを維持したままnotify直後に既存形式のeventをqueueするよう変更した。単独変更は raw 58.858→58.837fps、cube 20.997→20.050fpsで効果がなく、原因はdrmd mainが各IPC reply後に必ずcompletionを処理し、次のPOLL/READ RPCを直列化していたためだった。device pumpを最大16 IPC dispatchごとの有界batch（endpoint idle時は即pump）にし、順序もIRQ dequeue→deferred workへ修正すると、同一timing buildでraw 70.057→212.811fps、1000回合計のfill/submit/wait/readは2405/3687/7101/1079ms→1922/1547/749/452msとなった。32 IPC batchは170.241fpsで改善せず、16へ戻した。

normal fixtureの同一1000/cube8手順は旧58.962/20.100fps→最終169.491/23.952fps（raw 2.88倍、cube 1.19倍）。eventは1000/1000、user_data、sequence 1→1000、timestamp単調、magenta/red→green screendumpがgreenで、TRANSFER/SET_SCANOUT/FLUSHは削っていない。8-frame timing buildでは旧draw/swap/submit/wait=15.13/20.13/3.50/8.38ms/frameに対し11.75/29.00/3.75/<0.13ms/frameで、flip event待ちは消えたがswapのrun間変動が支配した。毎frameのserial printを計測時だけ抑えた60-frame診断はdraw/swap/submit/wait=2.30/25.78/1.85/0.23ms/frame、24.439fpsで、恒久fixture変更は0。LP_NUM_THREADS=1/2も16.891/16.497fpsで改善せずrevertした。

目標判定はrawが58.8fps級から170fps級へ改善したが「最低でも数百fps」には安定到達せず、cube 60fpsも未達。残差はrawでmapped dumb fill約2.2ms、page-flip submit IPC約1.45ms、poll/read約1.05ms、その他約1.2ms（最終host変動込み約5.9ms/frame）、cubeはEGL swap約25.8msが最大で、KMS submit/event waitは約2.1ms。次設計はdrmd-LPR間のshared event ring + native wait capabilityでpoll/read RPCを除くこと、private control page/IPC allocationの再利用を測ること、llvmpipe worker完了をguest samplingまたはfutex/scheduler spanで分解してGBM dumbへのdirect present統合可否を判断すること。dumb fillのLinux差はmapping/cache属性をuserland/_koboxから先に測り、kernel仮説へ飛ばない。

最終回帰はdrm-page-flip既定20/cube8（raw 176.991fps、cube 25.806fps）と1000/cube8、kms-modeset、drm-restart 20/20、mesa-inventory、clang-cold（guest 2秒 / host 6秒）、userland sync build、`zig build kernel`がgreen。fresh diskの初回1000/cube8は全frame/event/pixel成功後の既知process-exit停止だったため、timeout前にorphan runnerを除去して同一disk単体再実行しgreen。旧`efi` stepは現build graphに存在しない。kernel、公開ABI、private wire layout、_koboxの変更はない。計測ログは`.artifacts/m36d-*`、Linux QEMU traceは`.artifacts/m3.6b-linux-baseline/qemu-raw-flip.trace`に残した。

**5巡目結果 (M3.6e, 2026-07-12, 中断 → M6.0 へ移管)**: 仮説を置かない内訳計測の過程で、`smp.startIdleAps()` が定義されているのに Limine boot 経路から呼ばれておらず、**PachaOS が QEMU の 4 vCPU 中 1 CPU しか起動していない**ことを発見した (commit `29ee24f` の Limine 移行で旧 UEFI 経路の SMP prepare/start/configure が失われていた)。SMP 有効化に着手し、Limine RSDP 受け渡し + MADT 解析、1MiB 未満の SIPI trampoline 確保、AP の GDT/TSS/kernel stack/CR0.WP/EFER.NXE 整備、CPUID APIC-ID による CPU slot fallback、default scheduler の AP park 経路接続などを経て、`boot: cpus ready count=4` と **AP 上での user thread 実行 (syscall/IPC/filed 処理) まで成立**した。しかし allocator 同期 (`global_free_list` が lock なしで syscall 経路と page-fault COW 経路の両方から操作される)、remote thread teardown、AP timer preemption の context 保存契約、BSP への wake IPI、TLB shootdown が未完成で、boot 後段の lpr_supervisor exec が ENOMEM になり regression-green に到達しなかった。単発タスクではなくフェーズ規模の kernel 作業と判明したため、ユーザー判断でここで中断し **M6.0 として専用フェーズ化**した。WIP は `smp-wip` branch (f9bdde6) に全て保全し、判明事実・残作業の設計・性能期待値は `pacha_docs/smp-handoff-m36e.md` に記録した。main には kernel 差分を入れず、M3.6d の drmd 差分のみ採用して Phase M3 を締める。

### Phase M4 — 入力と seat

**M4.1 virtio-input / evdev → libinput**
- virtio-input (keyboard/mouse) モジュールを kobox に導入し /dev/input/event* を配線。libinput が列挙・イベント取得できるところまで。seat 管理 (seatd) の導入もここ。
- 受け入れ: QEMU window へのキー/マウス入力が evdev イベントとして fixture に届く。

**M4 結果 (2026-07-12, working tree / commit なし)**: QEMU の恒久 device 順序を `virtio-gpu-pci` (00:03.0)、`virtio-keyboard-pci` (00:04.0)、`virtio-mouse-pci` (00:05.0)、net、console とし、Arch Linux 6.8 の `virtio_input.ko` を既存 kobox Linux module 資産と同じ取得経路へ追加した。kobox の Pacha device capsule / Linux PCI shim を複数 device 対応にし、既存の input core shim から device metadata、capability bitmap、absinfo、event ring を snapshot する。Linux input core は `input_register_device` 時に EV_SYN を付け、driver の `dev->open == NULL` は成功扱いするため、kobox 側も同じ契約へ揃えた。keyboard は event0 (`QEMU Virtio Keyboard`, 0006:0627:0001:0001)、mouse は event1 (product/version 0002) となる。

所有は新設 `inputd` 1 process に集約した。理由は、2個の device capsule、同一 virtio-input module instance、open handle/cursor、event queue/grab/clock state が単一の実状態であり、DRM 実状態だけを持つ drmd や VFS pathname/exec を持つ filed に混ぜると責務が交差するため。koboxd は引き続き `.ko` runtime の共通層で、device service ownership は持たせない。inputd endpoint は filed exec bootstrap と LPR fixed fd 244 へ伝播し、LPR image ABI は version 8、filed exec fd-table ABI は version 5 とした。INPUT kind を DRM と PIPE の間へ挿入して後続番号を一括更新し、旧番号互換 shim は置いていない。

evdev の最小面は libinput 1.28.1 / libevdev の実 trace で確定した。最初の context failure は syscall 283 `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)` の ENOSYS であり、LPR local-state に Linux 番号 283/286/287 の timerfd create/settime/gettime と read/poll/epoll deadline 評価を追加した。device open 後の inputd trace は `EVIOCGVERSION(0x80044501)`, `EVIOCGID(0x80084502)`, `EVIOCGNAME/PHYS/UNIQ(0x80ff4506/07/08)`, `EVIOCGPROP(0x80084509)`, `EVIOCGBIT` selector 0/1/2/3/4/5/0x11/0x12/0x15, `EVIOCGKEY/LED/SW(0x80604518, 0x80084519, 0x8008451b)`, `EVIOCGREP(0x80084503)`, `EVIOCSCLOCKID(0x400445a0)` で、全て status=0。raw fixture はさらに必要最小の EVIOCGABS と poll/read を検証する。未要求 ioctl の網羅実装はしていない。

sysfs は `/sys/dev/char/13:64,65` → `/sys/devices/pci0000:00/0000:00:04.0,05.0/virtio2,3/input/input0,1/event0,1`、`/sys/class/input`、id/name/phys/capabilities/uevent を実ファイル/実 symlink として生成した。libudev-zero の devnum lookup が最初は誤った `../../../devices` で `/devices` を指して `Invalid path`、修正後は open まで進み、次に udev rule engine がないため `not tagged as supported input device` となった。event uevent に実際に要求された `ID_INPUT=1` と keyboard/mouse tag を載せて path backend の列挙が成立した。`libinput_udev_create_context` は hotplug monitor 用 AF_NETLINK がない段階で context creation に失敗するため、計画が許容する path backend を採用した。path backend も devnum→libudev-zero→上記 sysfs metadata を通り、単なる pathname open への短絡ではない。

Alpine v3.22 の seatd、seatd-launch、libinput/libinput-libs/libinput-udev、libevdev、mtdev、libudev-zero を rootfs に導入した。Alpine libseat は seatd backend 選択時にも libelogind を直接 link し、PachaOS loader では libelogind の `statx` 未実装で起動不能だったため、同じ upstream seatd 0.9.1 の libseat を `SEATD_ENABLED=1`, logind disabled で再現 buildした。daemon は Alpine package binaryそのもの。seatd の実 trace から AF_UNIX socket/bind/listen/connect/accept/poll、SO_PEERCRED、sendmsg/recvmsg + SCM_RIGHTS が必要と確定したため、新 daemon は増やさず既存 socket owner の netd に pathname付き local stream brokerを追加し、bind node は filed に S_IFSOCK として作らせた。SCM_RIGHTS は M4/M5 に必要な input/DRM service handle を duplicateして転送する。PachaOS は Linux VT `/dev/tty0` を持たないため、seatd が正式提供する `SEATD_VTBOUND=0` で non-VT seat0 を使う。

headless input 注入は pacgo の repeatable option `--input-send-event 'MARKER@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up'` として固定した。pacgo は console の MARKER を待って QMP `input-send-event` を1 eventずつ送る。key/btn は down/up、rel/abs は signed integer を受ける。M5/M6 でも compositor/client の ready marker を左辺に置けば同じ方式を再利用できる。QEMU 前処理は comm 名に合わせ `pkill -9 qemu-system-x86; sleep 1` とする。

恒久 smoke は `tests/run-lpr-qemu-evdev-smoke.sh` と `tests/run-lpr-qemu-libinput-seatd-smoke.sh`。前者は raw metadata、KEY_A 30 press/release、REL_X=7、REL_Y=-4、BTN_LEFT=272 press/releaseを検証する。後者は fixture が Alpine seatd を起動し、libseat `seat0` + libinput path backend で2 deviceを追加、同じ key/motion/buttonを個別 event と aggregateで検証する。最終結果は両 smoke、service ABI layout、pacgo Go test、kernel `CAPOS_UNWRAPPED_CLANG=/usr/bin/clang zig build test`、drm-page-flip 20 + cube8、kms-modeset、drm-restart 20、mesa-inventory、clang-cold (guest 2秒 / host 6秒) が green。page-flip初回は全frame/event/pixel後の既知 wrapper DONE 停止でtimeoutし、diskを消さず単体再実行して完全 green。mesa初回はM4後の実BDF 03に対し旧expect 04だけが不一致で、expectを実配置へ更新後green。

bootfs 増大時の起動停止は serial の `loadUserElfIntoProcessPages: scratch alloc failed bytes=704512` で、16MiB boot scratch が init ELF→bootfs をLIFO保持したまま bootfs 16.45MiBをinit VMOへcopyした後も解放されていないことが原因だった。userlandではkernel boot scratch lifetimeを変更できないため、copy完了後に `bootfs_image` をLIFO freeしてからinit ELF stagingする最小kernel修正を入れた。stale limine/userland ELFも別に実在したため、limine rebuild→kernel build→bootfs sync と全CMake ELF削除→rootfs force syncを行い、以後の実機traceは更新済みartifactで採取した。

**M4 性能回帰修正 (2026-07-12, working tree / commit なし)**: フルバッテリーで shell-interaction が pre-M4 86秒からM4 103秒へ退行したため、同一host上に HEAD `1461723` / _kobox `8d98744` の隔離worktreeを作り、guest `EPOCHREALTIME` case markerと `fork` 直前から `waitpid` 復帰までのTSC fixtureでA/Bした。`busybox true` 10回の中央値はpre-M4約74msに対してM4約86ms、shell全体は85.99秒対103.12秒だった。filedの既存exec stage計測を一時的に1-exec summary化すると、遅延は特定のfd-table走査やimage byte copyへ集中せず、`process_create` / `load_main` / `start_plan`へ約30M cyclesのscheduler待ちとして交互に現れ、filed外のfork/waitにも同じ待ちがあった。input fixed-fd配線を一時的に外しても中央値は改善しなかったため、この候補は棄却した。

根本原因は、新設inputdがendpoint空受信時にも `kb_handle_any_irq()` / deferred work pumpを無制限に反復し、single-CPU scheduler上で常時runnableだったこと。空受信後に一度pumpしてからinput endpoint readableを `pacha_fd_wait_many(..., PACHA_FD_WAIT_FOREVER)` で待つよう変更した。consumerのpoll/read RPCが即wakeし、そのRPC後pumpでIRQ/event ringを進めるため入力経路は失わない。TSC中央値はbusy-loop約328M cycles→修正後約266M cycles (pre-M4約264M) へ戻り、一時計測コードを全撤去したtiming付きshell smokeは86.28秒、最終無計測再実行は86.77秒 (pre-M4 85.99秒、差0.9%)。QMP注入raw evdevとlibinput+seatdもkey 30、REL 7/-4、BTN 272までgreen。case start/end markerも区間内訳を採取後に撤去した。実測86秒に対して従来90秒は4秒しか余裕がないためdefault timeoutを120秒へ変更した。これは速度修正後の約40% marginであり、性能退行の代用ではない。

### Phase M5 — Sway + Wayland 

**M5.1 wlroots/Sway 導入と棚卸し** — 修正せず証拠付き棚卸し、タスク分解して追記。
**M5.2+ ギャップ実装**
**M5.3 完了基準**: QEMU window に Sway が起動し、Wayland ミニアプリ (まず wl_shm クライアント、次に GL クライアント) が表示され、キー入力が反映される。screendump 検証 + 起動反復のリーク耐久スモーク green。
M6.Xに向けてテスト時のinput注入方法もここで固定してほしい

### Phase M6 Sway + Waylandの実用化 (追加で)

**M6.0 SMP (マルチコア) 本格対応 — M6 の最初に実施**
- 背景: M3.6e で PachaOS が QEMU 4 vCPU 中 1 CPU しか起動していないことが判明 (詳細は M3.6 5巡目結果)。llvmpipe worker、Sway compositor とクライアントの並行実行、clang 系ワークロードの土台として、実用化の最初にここで完成させる。
- 再開点: `smp-wip` branch (f9bdde6) に 4 CPU boot + AP user 実行まで動く WIP がある。引き継ぎレポート `pacha_docs/smp-handoff-m36e.md` の残作業を依存順に実施する: ① 物理 page allocator 同期の一元化 (page-fault 経路との lock order に注意) ② thread 実行所有権の state machine 化と remote teardown (stop IPI + quiescence 後の slot 再利用) ③ AP timer preemption の context 保存契約の再設計 (GPR/FS/GS/FX/CR3 の所有時点を明文化) ④ BSP idle/wake の周期 timer 非依存化と kernel-mode interrupt return 契約の検証 ⑤ address space ごとの active CPU mask と TLB shootdown ⑥ kernel 全域の SMP 監査 (boot log、global tables、_kobox callback 区間) ⑦ 段階的回帰。
- 受け入れ: 4 CPU boot 20 回連続 green、既存フルバッテリー + 耐久スモーク green、cube / raw flip / clang cold / Sway 起動の before/after 計測。性能期待値は SMP 単独で cube 60fps を保証しない (Linux の LP_NUM_THREADS 比から 40〜45fps 推定、swap 待ちが single-CPU 直列化起因なら上振れ) ため、未達分は M6.1 で継続する。

**M6.1 Swayの高速化** Swayの起動時間、平均フレームレート、マウス、キーボード遅延などを調査し分析後、修正して速度の向上を狙う
**M6.2 Swayの実用化** 動いているがまだ実用には足りない部分を調査し、修正を行う。M6.1S Swayの高速化と一緒に行うことも検討する。
**M6.3 waylandアプリの追加** 軽量な既存Waylandアプリケーションを追加し、GUI体験を上げる gtkは少し重いかもしれないが今後を考えてgtk glibを使えるようにしたいためgtk-demoを対応行うのと、限界が気になるためfoot terminalを選びました。
また、そちらからも理由を含め2つ軽量なGUIアプリケーションを選び、実装を行ってください
- **選定 (2026-07-11, Claude)**: ① **bemenu** — Wayland ネイティブの dmenu 系ランチャー (C、GTK 不要、依存極小)。実用面で WM の使い勝手に直結 (foot からコマンドを打たずにアプリ起動できる) し、技術面で wlr-layer-shell プロトコル (バー/オーバーレイ用 wlroots 拡張) を通す最初のアプリになり将来のステータスバー等の土台検証を兼ねる。② **swayimg** — 画像ビューア (純 Wayland C 製、GTK 不要)。素の wayland-client + wl_shm + xdg-shell + キー/マウス入力という「普通の GUI アプリ」の基本経路を実コンテンツで検証できる (foot はターミナル特化、gtk-demo は GTK 抽象越しのため)。screendump した画像を guest 内で開けるようになり開発ループも楽になる。
