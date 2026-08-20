# kobox2 の設計思想

> **状態:** 設計検討中。実装は未着手。仮称 `kobox2`。
> 全体の判断基準は [PachaOS の設計思想](./architecture.md) を参照すること。
> 本文書は既存の [filed VFS design](./filed-vfs-design.md) の記述と一部衝突する。
> §12 に整理した。

## 1. これは何か

kobox2 は、**Linux の binary `.ko` を PachaOS の userland で動かすための、
小さなモジュールローダと隔離境界**です。

kobox2 自身は Linux のことを知りません。Linux の構造体も、意味論も、
バージョンも持ちません。それらは全て、別リポジトリ・GPLv2 で管理される
**subsystem 側**にあります。kobox2 が持つのは、モジュールをロードする機構、
sandbox process の生成と監督、そして意図的に狭められた ring buffer
interface だけです。

### 何でないか

kobox2 は Linux 互換レイヤではありません。syscall ABI を提供しません。
「Linux を userland で動かす」ことが目的なら、後述するとおり **LKL のほうが
正しい答え**です (§10)。

kobox2 が正当化されるのは、次の二点においてのみです。

1. **ソースの無いモジュールを動かせる。** LKL は kernel source をコンパイル
   するため、構造上これができません。
2. **監査できないコードをハードウェア境界の内側に閉じ込められる。** sandbox
   process + 専用 IOMMU domain。

この二点に効かない機能追加は、LKL の劣化再実装に向かっている合図です (§10)。

## 2. なぜ v1 を作り直すのか

以下は kobox v1 (`_kobox/`) の実測値です。設計上の各規則は、ここに挙げた
どれかから導かれています。

| 観測 | 実測 | 出典 |
|---|---|---|
| Linux 構造体オフセットの直値 | ツリー全体で **648 個** (`fs.c` だけで 229) | `src/linux_subsystem/fs/fs.c:109-165` ほか |
| `task_struct` の直値 | `0x638`, `0xbd8` | `src/linux_personality/linux_stubs.c:21,24` |
| page cache | **2048 スロットの静的配列**、索引なし、毎回線形走査 | `fs.c:39`, `fs.c:4365` |
| buffer cache | **512 エントリ固定** | `fs.c:57` |
| `filemap_xarray_load` | xarray ではなく同じ配列の線形走査 | `fs.c:713` |
| kthread | `setjmp`/`longjmp` の協調コルーチン (7 箇所) | `linux_stubs.c:44,62` |
| 単一ファイルの規模 | `fs.c` = **15,620 行 / 532 KB** | — |
| stub されているシンボル | **203 個** (うち 179 個が `ext4.ko` の要求と重なる) | `.artifacts/fs-stub.names` |
| ライセンス宣言 | **ゼロ** (SPDX なし、LICENSE/COPYING なし) | — |

これらは品質の問題である前に、**依存の向きの問題**です。v1 の buffer_head は
33 シンボル中 29 個が実装されており一見資産に見えますが、その実装が乗って
いる土台 — 2048 スロット配列、512 エントリ cache、setjmp コルーチン、
648 個のオフセット — は kobox2 が置き換える対象そのものです。土台を
入れ替えれば実装は残りません。

**したがって v1 のコードは継承しません。** 継承するのは測定装置です (§11)。

### 2.1 v1 の page cache は現在の性能問題の容疑者

`KB_FS_FILEMAP_FOLIO_CACHE_MAX = 2048` は約 8 MiB です。working set が
これを超えると、`filemap_get_folio` は 2048 要素を線形走査した上で shrinker
を叩いて evict します。

これは Sway 起動退行 (10 秒 → 350 秒) と、検証ゲートで観測される **flake の
双峰性**の両方と整合します。「じわじわ遅い」ではなく「ある working set を
境に桁で遅くなる崖」だからです。

**未検証の仮説**ですが、切り分けは非常に安く済みます。二つの仮説が同じ
一行の変更に対して逆の予測をするからです。

| `CACHE_MAX` を 2048 → 32768 | 予測 |
|---|---|
| 容量の崖が支配項なら | 速くなる |
| O(n) 走査が支配項なら | 遅くなる |

kobox2 とは独立に、先に測る価値があります。

## 3. 設計思想

### 3.1 kobox2 は小さいローダである

kobox2 の責務は次に限定します。

- ELF module のロードと再配置の**指示**、依存解決、シンボル解決の**方針**
- sandbox process の生成・監督・再起動
- ring buffer channel の確立と権限付与
- 拒否ポリシー (どの feature を受け付けないか) の保持

kobox2 が**持たないもの**: Linux の構造体定義、Linux の意味論、
filesystem/block/net の知識、カーネルバージョン依存の値。

これは方針ではなく機構で保証します。kobox2 は Linux header を include
できません (§3.3)。

### 3.2 stub を書ける場所を無くす

v1 は「重要な機能を無効化する stub」をある程度許容していました。kobox2 は
これを禁止します。ただし禁じるのは **silent stub** であって、未実装そのもの
ではありません。区別は次のとおりです。

| 状態 | 意味 | 実装 |
|---|---|---|
| `implemented` | 本物 | 手で書く |
| `refused` | 意図的に非対応 | **manifest から生成**。mount 時に feature bit を見て拒否 |
| `unimplemented` | 未着手 | **manifest から生成**。呼ばれたら log + abort |

**`refused` と `unimplemented` のコードは手で書きません。** manifest から
生成します。したがって「うっかり 0 を返す no-op を書く」ことが**構造的に
不可能**になります。規律ではなく、書く場所が存在しないという形で保証します。

CI が検査すること:

1. subsystem `.so` のエクスポート集合が、対象 `.ko` の未定義シンボル集合と
   完全一致すること
2. `unimplemented` がクリティカルパス上にゼロであること
3. `unimplemented` の総数が単調非増加であること

### 3.3 Linux の構造体を知るのは GPL 側だけ

`KB_FS_INODE_SIZE_OFFSET = 0x50` のような定数を 648 個抱えたのは、
`struct inode` が不透明なまま手で合成したからです。オフセットが 1 つずれても
コンパイルは通り、実行時にメモリを壊します。

kobox2 の解は「気をつける」ではありません。

> **Linux の構造体は、本物の kernel header に対してコンパイルする。
> オフセットは常にコンパイラが計算する。**

これを行う薄い shim は GPL 側リポジトリにのみ存在し、kobox2 は不透明ハンドル
しか見ません。数値オフセット定数は grep で落とします (§8)。

**この境界は、正しさの境界であると同時にライセンスの境界でもあります。**
二つが一致するのは、切り方が正しいことの兆候だと考えます。

### 3.4 上流は参考資料ではなく、部品置き場かつ品質基準

subsystem リポジトリを GPLv2 にする決定は、ライセンス衛生のためだけでは
ありません。**上流 Linux のコードをそのまま持ち込めるようになる**ことが、
時間と品質の両方に効く最大のレバーです (著作権表示と license 保持は必須)。

実装対象は 3 つに割れます。

| 種別 | 扱い | 例 |
|---|---|---|
| 土台にほぼ依存しない | **そのまま持ち込む** | `lib/xarray.c`, `lib/rbtree.c`, `lib/idr.c`, `lib/sort.c`, string/bitmap 系 |
| 論理は上流のまま、土台呼び出しのみ差し替え | **移植** | `fs/buffer.c`, `mm/filemap.c`, `fs/libfs.c` |
| 土台そのもの | **書く** | thread, lock, allocator, time, RCU, per-CPU |

さらに、**上流のファイル構成をそのまま鏡にします**。`mm/filemap.c` を
再実装するならファイル名は `mm/filemap.c` です。これで 3 つ得られます。

- 何を実装していないかが `diff` で機械的に出る
- ファイルが肥大しない (上流の当該ファイルが事実上の行数上限になる)
- 派生物を派生物として、GPL リポジトリに、同じ構造で置くという正直な姿勢

**「力仕事の品質はレビューでは守れない」** — v1 も善意で書かれ、`fs.c` は
15,620 行まで堆積しました。守れるのは、交渉できない外部基準 (上流のファイル)
と機械的なオラクル (§8) だけです。

### 3.5 SMP は後から有効化する。後から設計はしない

v1 の失敗は UP だったことではなく、**UP から出られない形の UP** だった
ことです (setjmp コルーチン)。

kobox2 は、データ構造とロックを最初から SMP 前提で設計します
(per-CPU、本物の spinlock 意味論、RCU)。有効化は後で構いません。初期は
1 スレッドで走らせてよい。

**retrofit だけは行いません。** LKL が `CONFIG_SMP=n` を選んだのは
アーキテクチャ上の決断であり、v1 が UP なのは事故です。両者は別物です。

## 4. process 配置

### 4.1 architecture.md の基準に対する回答

[architecture.md](./architecture.md) は process 境界の基準を二点に定め、
storage 経路については明示的に融合を選んでいます。

> handle の復元や transaction recovery を持たずに process だけを分けても、
> 障害耐性は増えず、IPC と同期だけが増えます。

kobox2 はこの基準に対して次のように答えます。

**基準 1 — 状態と capability の所有者が異なるか。** 異なります。sandbox は
GPL kernel code と DMA 可能メモリを、**専用 IOMMU domain の下で**所有します。
filed2 は namespace、vnode、open-file-description、path policy を所有します。
これは v1 より強い分離です。v1 では両者が同一アドレス空間にあり、
capability 境界がソフトウェア規約でしかありませんでした。

**基準 2 — 独立に停止・再起動・存続させることに意味があるか。** あります。
そして **transaction recovery は jbd2 が既に持っています**。sandbox が死んだ
場合、filed2 はそれを再起動し、jbd2 が journal を replay して filesystem を
整合状態に戻します。architecture.md が process 分離の前提として要求している
機構が、ロードするモジュール自身に内蔵されている、という構図です。

handle の復元については設計課題が残ります。filed2 は vnode/OFD を保持し、
sandbox 再起動後に inode 番号から再解決する必要があります。**これは未解決
項目です** (§11)。

### 4.2 基準の拡張提案

上記二基準は、**信頼**の軸を持ちません。architecture.md が書かれた時点では
必要なかったためです。kobox2 は第三の基準を提案します。

> 3. そのコードを監査できるか。監査できないコードは、監査できるコードと
>    同じアドレス空間に置かない。

ベンダの binary `.ko` は filed 自身のコードと質的に異なります。この基準は
architecture.md への追記を要するため、**本文書の一存では確定させません**。

### 4.3 配置

```text
filed2 プロセス
  namespace / vnode / OFD / path policy / exec policy
        │
        │  ring buffer  (データパス。kobox2 は介在しない)
        │
        ├──────────────────────────┐
        │                          │
   kobox2 プロセス                  │
     module loader / 監督 / 拒否ポリシー
     (control plane のみ)          │
        │  ring buffer             │
        │  (control)               │
        ↓                          ↓
   kobox sandbox プロセス  (専用 IOMMU domain)
     - linux primitive の .so  (kmalloc, kthread, lock, RCU, ...)
     - fs subsystem の .so     → jbd2.ko / ext4.ko / mbcache.ko / crc16.ko
     - blk subsystem の .so    → nvme.ko / nvme-core.ko
```

**kobox2 はデータパスに入りません。** `filed2 → kobox2 → sandbox` の直列
構成では、I/O 1 往復あたり ring を 4 回横断します。kobox2 は channel を
確立したら退き、filed2 と sandbox が直接 ring を共有します。

これは性能のためだけではありません。kobox2 を control plane に限定すること
が、§3.1 の「kobox2 を小さく保つ」を最も強く担保します。データを見ない
component は、データの意味を知る必要がないからです。

### 4.4 所有と authority を分ける

§4.3 の「kobox2 はデータパスに入らない」は、このままでは **規約** です。
守るのは実装者であり、一度でも「ここだけ kobox2 が中継すれば早い」を通せば
崩れます。規約は性能圧の下で必ず破れます。

別 OS (形式検証マイクロカーネル) の DMA モデルが、これを不変条件へ変える形
を示しています。

> Device / MMIO / IRQ / DMA mapping の実所有者は worker sandbox。
> controller は生成・停止・再割当てを要求する管理 authority だけを持つ。

kobox2 に写すと、**sandbox の生死を制御する権限は持つが、sandbox が触る
メモリやデバイスへの参照は保持できない**。データパスに入らないのではなく、
**入れない**。capability の型がそれを禁じます。

PachaOS 上での形:

- device FD / DMA mapping FD / ring VMO の所有者は sandbox。kobox2 は
  それらを自分の FD 表に持たない
- kobox2 が持つのは「sandbox を作る / 止める / 再生成する」権限のみ
- filed2 ↔ sandbox の ring は両者が直接共有する。kobox2 は確立を仲介した
  のち、自分側の参照を落とす

**確立時に一時的に参照を持つのは避けられません。** 条件は「確立完了時点で
kobox2 側から到達不能になっていること」であり、これは FD 表を見れば検証
できます。§3.1 の「小さいローダ」を、意図ではなく構造で担保する形です。

## 5. ring buffer

### 5.1 lock を避ける形

単一の lock-free MPMC ring は、正しく書くのが難しく、書けてもキャッシュ
ラインで競合します。kobox2 は **スレッド / キューごとの SPSC ペア**
(submission + completion) を採ります。

これは NVMe の SQ/CQ そのものであり、`nvme.ko` と blk-mq が前提とする
per-CPU hardware queue の形とも一致します。lock-free が自明になり、SMP で
そのままスケールし、抽象化の継ぎ目が自然になります。

### 5.2 意図的に狭める

interface は関数ポインタ 1 構造体に閉じ込め、それを超える依存が
**コンパイルエラーになる**形にします。LKL の `lkl_host_operations`
(~30 コールバック) が、この形が成立することの実証です。

### 5.3 共有メモリ基盤は既にある

「ring buffer 文化が PachaOS にない」は、見かけより安く済むはずです。

- 先行実装: `userland/drmd/src/virtio_gpu_unref_bridge.c` と kvm 層の vring
  処理。**virtqueue は lock-free ring そのもの**です。
- 基盤: DMA 修理 Phase 1 で導入した「作成時から連続な VMO + `mmap(SHARED)`
  の eager PTE install」が、プロセス間 ring が必要とするものの正体です。

virtio の descriptor / avail / used 分割は、仕様が公開され意味論が枯れて
います。**独自 ring を設計する前に、これを土台にできないか検討すること。**

### 5.4 ring と device queue を混同しない

「ring は DMA 対象にしない」という規則は、**sandbox 間 IPC の ring にしか
適用できません**。

blk subsystem に `nvme.ko` を載せた時点で、NVMe の SQ/CQ は本質的に DMA
対象になります (コントローラが SQ を読み、CQ を書く)。実 virtio デバイスの
descriptor / avail / used ring も同じです。

| | 用途 | DMA | 保護 |
|---|---|---|---|
| **IPC ring** | filed2 ↔ sandbox、kobox2 ↔ sandbox | させない | 共有 VMO のみ。device から到達不能 |
| **device queue** | sandbox ↔ NVMe / virtio HW | 必須 | IOMMU domain + device 世代検査 (§11-4) |

同じ「ring」という語で両方を指すと、規則が静かに破れます。**本文書および
実装では、device queue を ring と呼ばないこと。**

## 6. 境界の実測

`ext4.ko` の未定義シンボルが、境界の定義そのものです。**615 個**。

| クラス | 数 | 中身 | コスト |
|---|---|---|---|
| **A. 流用** | 63 | `jbd2_*` 53、`mb_cache_*` 10 | 無料 (`.ko` を載せるだけ) |
| **B. 機能ごと拒否** | ~97 | fscrypt/fsverity 49、quota 34、iomap 8、DAX ~6 | 無料 (mount 時に feature bit を拒否) |
| **C. ビルド設定由来** | 33 | `__SCT__*`、`__fentry__`、`__x86_*_thunk`、`bpf_trace_run1..6`、`stackleak_track_stack`、`latent_entropy` | 無料 (自前ビルドの config で消える) |
| **D. 実装が要る** | ~422 | 下記 | 実作業 |

クラス D の内訳:

| 層 | 数 | 方針 (§3.4) |
|---|---|---|
| string / lib / bitmap / printk / time / crypto | ~74 | 持ち込む |
| VFS core | 84 | 移植 (filed2 が唯一の消費者なので薄くできる) |
| page cache / folio / writeback | 42 | **本実装** (§7) |
| buffer_head | 33 | **本実装** (§7) |
| bio / block layer | 14 | 移植 |
| mm alloc / sched / wq / kthread / lock / RCU / per-CPU | 85 | **書く (substrate)** |
| その他 (proc/sysfs, security/cred, misc) | ~90 | 個別判断 |

**当初「どこまで流用しどこから再実装するか」が最難関と見ていたが、
実測では 193 個 (A+B+C) の判断が自明**でした。真に設計判断を要するのは
§7 の一点です。

> **注意:** この分類は heuristic であり、元データ `.artifacts/ext4-u.names`
> の出所カーネルバージョンが未確定です (§11-1)。バージョン確定後に
> 再生成すること。

## 7. page cache — 唯一の設計課題

`ext4.ko` が page cache に触る経路は 3 本あり、性質が全く異なります。

**① folio ライフサイクル** — get / lock / unlock / put、refcount、
dirty/writeback 状態ビット。状態機械が明確なので素直に書けます。

**② buffer_head over folio — 構造上、切れない。**
ext4 のメタデータも、**jbd2 も、folio ではなく buffer_head を journal
します**。`bh->b_private` に `journal_head` がぶら下がる契約です。jbd2.ko を
流用する限り、buffer_head は per-buffer 状態ビットと `b_private` を含めて
本物でなければなりません。薄い shim にはできません。

**③ writeback / dirty tagging — 実はここが一番厄介。**
`tag_pages_for_writeback` / `filemap_get_folios_tag` / `->writepages` は、
mapping に対する **タグ付き範囲検索** (`PAGECACHE_TAG_DIRTY` / `TOWRITE`) を
要求します。配列では成立しません。

ただし **`lib/xarray.c` がまさにこれで、GPLv2 なので持ち込めます** (§3.4)。
設計問題は消え、移植問題になります。

**結論:** page cache は kobox2 で唯一、shim ではなく本実装すべき層です。
`address_space` ごとのタグ付きインデックス、本物の folio refcount / lock /
writeback 状態、`folio->private` の buffer_head。ここが本物なら、残りは
薄くできます。

## 8. 品質機構

「既知難易度の力仕事」でこそ品質は崩れます。目視レビューでは守れません。

| # | 機構 | 塞ぐ失敗 |
|---|---|---|
| 1 | 本物の header に対してコンパイル | 648 個のオフセット直値 |
| 2 | 上流のファイル構成を鏡にする | 15,620 行のファイル、暗黙の未実装 |
| 3 | `refused` / `unimplemented` を manifest から生成 | silent no-op |
| 4 | `e2fsck` + イメージ差分をオラクルにする | 目視で守れない正しさ |
| 5 | grep ルール 2 本 | 下記 |

**機構 4 の具体:** 同じ操作列を native Linux と kobox2 の両方で同じイメージに
対して実行し、`e2fsck` と `dumpe2fs` で突き合わせます。350 シンボルを目で
見て品質を守るのは不可能ですが、結果のディスクイメージが本物の Linux と
一致するかは機械的に判定できます。**部品は v1 に既にあります** (§11)。

**機構 5 の内容:**
- ファイルスコープの固定長配列を禁止 (`KB_FS_FILE_MAX = 256` が生まれた経路)
- 数値のオフセット定数を禁止 (機構 1 により、そもそも不要になる)

## 9. ライセンスとリポジトリ

| リポジトリ | ライセンス | 中身 |
|---|---|---|
| PachaOS (本リポジトリ) | 現行のまま | kobox2 (ローダ / 監督 / ring)、filed2 |
| subsystem (新規・完全分離) | **GPLv2** | primitive `.so`、subsystem `.so`、header shim、pin した kernel source |

- **commit #1 から** GPLv2 + SPDX ヘッダを入れること。後から遡って付けるのは、
  他者のコミットが入った瞬間に困難になります。
- **ワイヤプロトコル仕様は非 GPL 側**に置き、両者がそれを実装する形にします。
- プロセス分離と、文書化された IPC プロトコルという構造が、分離を主張する
  上で最も強い形です。

> これは法的助言ではありません。構造の話であり、最終判断は別途行うこと。

### 9.1 `.ko` は自前でビルドする

distro の `.ko` を使わず、**pin した upstream source を、面を最小化する
config で自前ビルド**します。GPL 側リポジトリなので source はどのみち置く
ことになります。

- クラス C の 33 シンボルが消える (`PREEMPT_DYNAMIC=n` で `__SCT__*`、
  `FTRACE=n` で `__fentry__`、`BPF_EVENTS=n` で `bpf_trace_run*` 等)
- distro 独自パッチを継承しない
- バージョンが再現可能に固定される
- 機構 1 (本物の header) が成立する

**制約:** binary `.ko` 互換は version だけでなく **config にも縛られます**。
`struct inode` のレイアウトは `CONFIG_FS_POSIX_ACL` / `CONFIG_SECURITY` /
`CONFIG_FSNOTIFY` で変わります。したがって **`.ko` と substrate は同じ
source + config ツリーから一緒にビルドされねばなりません。**

将来のベンダドライバについては、その config に合わせる必要が生じます。
source shim + blob 形式 (NVIDIA 等) なら現地コンパイルで緩和されますが、
純バイナリ配布は厳しい。**長期目標の射程を早めに見積もること** (§11)。

## 10. LKL との比較

LKL (Linux Kernel Library) が最も近い先行例です。差は全て**どの高さで切るか**
から出ています。

|  | LKL | kobox2 |
|---|---|---|
| 切る位置 | kernel の**下** (`arch/lkl` として arch port) | kernel の**中** (`.ko` がリンクするコアを供給) |
| ホスト面 | `lkl_host_operations` **~30** | **~350** シンボル |
| 得られるもの | Linux 全部 | ロードしたモジュールだけ |
| 入力 | kernel **source** | **binary `.ko`** |
| 隔離 | 1 アドレス空間 | sandbox process + IOMMU domain |
| SMP | `CONFIG_SMP=n` | SMP 前提で設計、後で有効化 (§3.5) |
| バージョン追従 | out-of-tree rebase が慢性的な痛み | 固定が内在的 |
| syscall ABI | `lkl_syscall()` で無料 | 1 本ずつ手で書く |

**「ext4 を userland で動かしたい」だけなら LKL が正解です。**
kobox2 が正当化されるのは §1 の二点のみ。

### 判断の北極星

設計で迷ったときの基準:

> その選択は「ソースの無いモジュール」と「隔離」に奉仕しているか。
> それとも LKL を劣化再実装しているか。

汎用性や syscall 網羅に手を伸ばし始めたら、負け戦に入った合図です。逆に
この二点に効く投資 (sandbox 境界、IOMMU domain、ローダの厳密さ、ring の
制限性) は、どれだけ払っても LKL には追いつけない領域です。

### 最初のターゲット

`filed2 + ext4 + nvme`。ext4 はソースがあるので**本来 LKL でも足りる** —
つまり「kobox2 が動くことを証明する題材」として安全であり、成功すれば nvme の
先にベンダドライバへの道が開きます。

## 11. 未決事項

| # | 項目 | なぜ先に決めるべきか |
|---|---|---|
| 1 | **kernel バージョンの確定** | `.artifacts/ext4-u.names` には `__kmalloc_noprof` (6.10+)、`__kmem_cache_create_args` (6.12+)、`__find_get_block_nonatomic` (6.15+) が含まれるが、`.artifacts/debs` は **6.8.0-117**。境界仕様自体がバージョン依存なので、確定しないと §6 以降が空振りする。LTS を選び point release まで pin すること |
| 2 | **モジュールを実際にロードするのは誰か** | kobox2 が別プロセスの sandbox にどうロードするか。(a) sandbox が自己ロードし kobox2 は指示と検証のみ / (b) kobox2 がクロスプロセス ELF ロード。(a) が簡単で kobox2 も小さく保てる。また `.ko` を primitive `.so` にリンクする再配置・シンボル解決コードは派生物寄りなので **GPL 側に置くほうが清潔** |
| 3 | **ring プロトコルは virtio か独自か** | §5.3。仕様が既にあることの価値と、「意図的に制限する」方針との緊張 |
| 4 | **sandbox 再起動時の handle 復元 (と device 世代)** | §4.1。architecture.md の基準を満たすための必須要件。filed2 が vnode/OFD を inode 番号から再解決する設計。**加えて device 側**: 再起動前の sandbox が仕込んだ DMA・completion・IRQ が、再起動後の sandbox に届いてはならない。teardown をどれだけ正しくしても、ハードウェアが既に保持しているキューエントリは止まらないため、**device generation を持ち、世代を跨いだ completion / IRQ / mapping を受理しない**機構が要る。PachaOS には現在これが無い — `kernel/src/state/types.zig` の `IrqObject` に generation フィールドが無く、`kernel/src/state/fd.zig` の generation は FD 番号再利用の ABA 対策であって device の世代ではない。**sandbox 再起動を設計条件にする以上、kobox2 はこれを前提にできない。PachaOS 側の追加が先** (`.temp-docs/dma-iommu-repair-plan.md` R11 として登録済み) |
| 5 | **ベンダ binary `.ko` の現実的射程** | §9.1 の config 制約。長期目標が実際にどこまで届くかの見積もり |
| 6 | **v1 の page cache 崖の検証** | §2.1。kobox2 とは独立に、今すぐ安く測れる |

## 12. v1 から継ぐもの・捨てるもの

**捨てる — 実装。** §2 の理由。

**継ぐ — 測定装置。** これは価値が高く、作り直す必要がありません。

- `_kobox/tools/run_ext4_real_module_probe.sh` — 本物の `.ko` を載せて叩く
  harness の形
- feature 別プローブイメージ (`ext4-smoke.img`, `inline_data.img`,
  `metadata_csum_seed.img`, `orphan_file.img`, `full-features.img`) — 実 mkfs
  で作った本物のフィクスチャ
- `.artifacts/ext4-u.names` (615) / `fs-stub.names` (203) — 境界仕様
- `e2fsck` をオラクルにしていた実績 (`.artifacts/ext4-probe/e2fsck.log`)
- **失敗記録**: v1 は `ext4.ko` + `jbd2.ko` + `mbcache.ko` + `crc16.ko` を
  実際にロードし、read / write / readback / native writeback まで通した上で
  **jbd2.ko の `section_offset=0x1724` で SIGILL** している
  (`.artifacts/ext4-feature-probe/orphan_file.log`)。jbd2 が何を要求するかの
  データ点として有効

## 13. 既存文書との衝突

[filed VFS design](./filed-vfs-design.md) は次のように記述しています。

> 現在、storage 用 Kobox runtime は `filed.elf` にリンクされている。
> (中略) process 分離を恒久的な設計条件とはしない。

kobox2 はこれを**反転させます** — process 分離を恒久的な設計条件とします。

[architecture.md](./architecture.md) は、その転換の条件を既に示しています。

> 将来、独立再起動や状態復元に意味を持たせられるなら、同じ interface を
> process 境界として使うこともできます。

§4.1 がこの条件への回答です。ただし未決事項 4 (handle 復元) が閉じるまで、
条件は完全には満たされていません。

**この二文書の更新は、kobox2 の設計確定後に行うこと。** 本文書が先行して
既存の記述を無効化しないよう、衝突を明示的に残しています。

---

## 関連文書

- [PachaOS の設計思想](./architecture.md)
- [filed VFS design](./filed-vfs-design.md)
- [Userland service ABI](./userland-service-abi.md)

by Claude Opus 5
