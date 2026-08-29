# kobox2 の設計思想

> **状態:** 設計検討中。実装は未着手。仮称 `kobox2`。
> 全体の判断基準は [PachaOS の設計思想](./architecture.md) を参照すること。
> 本文書は既存の [filed VFS design](./filed-vfs-design.md) の記述と一部衝突する。
> §14 に整理した。
>
> 最初の実装先は PachaOS とする。sandbox を第一級に扱う別のマイクロカーネルの
> ほうが最終的な配置には適しているが、現時点では未完成である。ただし PachaOS
> を kobox2 の前提にはせず、将来は host port の追加だけで対応できる構造にする。

## 1. これは何か

kobox2 は、**Linux の binary `.ko` を sandbox userland で動かすための、
小さなモジュールローダと隔離境界**です。最初の host port は PachaOS です。

kobox2 の**非 GPL controller 側**は Linux のことを知りません。Linux の
構造体も、意味論も、バージョンも持ちません。それらと `.ko` の loader は、
別リポジトリ・GPLv2 で管理される **Linux sandbox 側**にあります。
controller が持つのは、sandbox process の生成と監督、起動 manifest の検証、
そして意図的に狭められた channel の確立だけです。

### 何でないか

kobox2 は Linux 互換レイヤではありません。syscall ABI を提供しません。
LKL は最も近い比較対象ですが、Linux core 全体を library 化し、`CONFIG_SMP=n`
であるため、役割別 sandbox と初期からの SMP を要求する kobox2 の代替では
ありません (§10)。

kobox2 が正当化されるのは、次の三点です。

1. **ソースの無いモジュールを動かせる。** LKL は kernel source をコンパイル
   するため、構造上これができません。
2. **監査できないコードをハードウェア境界の内側に閉じ込められる。** sandbox
   process + 専用 IOMMU domain。
3. **SMP を最初から提供する。** LKL の `CONFIG_SMP=n` では満たせない Linux
   module の並行実行を、host OS の native thread 上に構築する。

この三点に効かず、Linux core 全体を載せる方向へ進む機能追加は、kobox2 の
切断位置を失っている合図です (§10)。

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

## 3. 設計思想

### 3.1 非 GPL 側は小さい controller である

kobox2 の非 GPL controller の責務は次に限定します。

- module image の hash / size、opaque profile、capability set の検証と起動指示
- sandbox process の生成・監督・再起動
- ring buffer channel の確立と権限付与
- resource admission policy (どの capability と上限を渡すか) の保持

subsystem `.so` の ELF load / dynamic symbol 解決と、`.ko` の ELF parse、再配置、
依存解決、Linux symbol 解決、`init_module` / `cleanup_module` 呼び出しは
**Linux sandbox が自己ロードとして行います**。
controller が別 process の address space を書き換える cross-process loader は
作りません。loader は Linux module ABI と強く結合するため、GPL 側に置くのが
責務とライセンスの両面で自然です。

controller が**持たないもの**: Linux の構造体定義、Linux の意味論、
filesystem/block/net の知識、カーネルバージョン依存の値、再配置処理。
Linux feature の `refused` / `unimplemented` manifest は GPL sandbox 側が所有し、
controller はその profile ID と hash を opaque な値として扱います。

これは方針ではなく機構で保証します。非 GPL controller は Linux header を
include できません (§3.3)。

### 3.2 stub を書ける場所を無くす

v1 は「重要な機能を無効化する stub」をある程度許容していました。kobox2 は
これを禁止します。ただし禁じるのは **silent stub** であって、未実装そのもの
ではありません。区別は次のとおりです。

`refused` にできるのは、mount 前などに完全拒否できる独立した optional feature
だけです。性能や複数 subsystem の正しさに波及する基盤機能を `refused` にして
先へ進むことは禁止します。

| 状態 | 意味 | 実装 |
|---|---|---|
| `implemented` | 本物 | 手で書く |
| `refused` | 意図的に非対応 | **manifest から生成**。mount 時に feature bit を見て拒否 |
| `unimplemented` | 未着手 | **manifest から生成**。呼ばれたら log + abort |

**`refused` と `unimplemented` のコードは手で書きません。** manifest から
生成します。したがって「うっかり 0 を返す no-op を書く」ことが**構造的に
不可能**になります。規律ではなく、書く場所が存在しないという形で保証します。

CI が検査すること:

1. profile がロードする core / subsystem `.so` と依存 `.ko` の export closure が、対象
   `.ko` の未定義シンボル集合と完全一致すること
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

### 3.4 上流は参考資料ではなく、実装本体かつ品質基準

Linux sandbox リポジトリを GPLv2 にする決定は、ライセンス衛生のためだけでは
ありません。**上流 Linux のコードをそのまま実装として使えるようになる**
ことが、v1 からの最大の変更です (著作権表示と license 保持は必須)。

実装対象は 3 つに割れます。

| 種別 | 扱い | 例 |
|---|---|---|
| 土台にほぼ依存しない | **そのまま持ち込む** | `lib/xarray.c`, `lib/rbtree.c`, `lib/idr.c`, `lib/sort.c`, string/bitmap 系 |
| 論理は上流のまま、土台呼び出しのみ差し替え | **移植** | `fs/buffer.c`, `mm/filemap.c`, `fs/libfs.c` |
| 土台そのもの | **書く** | thread, lock, allocator, time, RCU, per-CPU |

さらに、Linux sandbox リポジトリは pin した upstream Linux の point release
を基点にした fork とし、**上流のディレクトリ構成をそのまま使います**。
`mm/filemap.c` を使うなら配置先も `mm/filemap.c`、`lib/xarray.c` なら
`lib/xarray.c` です。選んだファイルを `src/linux_compat/` のような独自分類へ
コピーし直しません。

```text
kobox-linux/                 # GPLv2、pin した Linux source tree が基準
  COPYING
  Kconfig / Makefile
  block/ crypto/ drivers/ fs/ include/ kernel/ lib/ mm/ ...
  kobox/                     # upstream に無い kobox 固有部分だけ
    host/
      host.h                 # OS 非依存の host contract
      test/                  # CI / host test port
    loader/                  # sandbox 内の .so / .ko self-loader
    manifest/                # source file → subsystem .so → target .ko の対応
```

上流ファイルを変更する必要がある場合も同じ path で変更し、基準 tag に対する
patch として管理します。`kobox/` は OS 境界や loader のように上流 Linux に
対応する置き場所がないコードだけに使います。これで 4 つ得られます。

- 何をそのまま流用し、何を変更したかが upstream tag との差分で機械的に出る
- source / config / header / `.ko` の組を一つの tree で固定できる
- ファイルが肥大しない (上流の当該ファイルが事実上の行数上限になる)
- 派生物を派生物として、GPL リポジトリに、同じ構造で置くという正直な姿勢

ここで Linux の tree 構造を使うのは **source の配置と provenance のため**であり、
tree 全体を一つの runtime image にするという意味ではありません。Linux core 部分は
一つの shared object、非 driver subsystem は責務ごとの shared object として build
します。

```text
Linux core/primitive .so   kernel/・mm/・lib/ 由来の共通機構
                          allocator / page cache / writeback / kthread /
                          lock / RCU / per-CPU / xarray / ...
device-pci subsystem .so   device model / PCI core / DMA API / IRQ glue / ...
blk subsystem .so          bio / request / blk-mq / block device core / ...
fs subsystem .so           VFS / buffer_head / filesystem core / ...
net subsystem .so          net_device / skb / NAPI / network core / ...
drm subsystem .so          DRM / GEM / TTM / GPU scheduler / display core / ...
```

core を `mm.so` / `sched.so` / `rcu.so` のように機能ごとへ分割しません。Linux の
core state と相互依存を一つの `core/primitive .so` に閉じ、その外側だけを Linux の
subsystem 境界に沿って `.so` 化します。そのため netd-kobox が使わない core path を
一部含むことは許容します。core 内部へ人工的な ABI 境界を増やすより、この単位を
保つことを優先します。

target profile は必要な `.so` と `.ko` だけを manifest に列挙し、sandbox はそれだけを
ロードします。filed-kobox の NVMe 段階なら core/primitive / device-pci / blk 系 `.so` と
`nvme-core.ko` / `nvme.ko`、ext4 まで載せる段階で初めて fs `.so` と `jbd2.ko` /
`mbcache.ko` / `crc16.ko` / `ext4.ko` を追加します。netd-kobox なら
core/primitive / device-pci / net 系 `.so` と対象 NIC driver `.ko` だけです。
gpud-kobox なら core/primitive / device-pci / drm 系 `.so` と AMDGPU の dependency
closure だけをロードします。

Linux core `.so` は全 Linux kernel ではありません。選択した module 群が要求する
core symbol の実装だけを含み、syscall ABI、全 driver、未選択 subsystem は載せません。
LKL のように Linux core 全体を一つの library として持ち込む構成にはしません。

**「力仕事の品質はレビューでは守れない」** — v1 も善意で書かれ、`fs.c` は
15,620 行まで堆積しました。守れるのは、交渉できない外部基準 (上流のファイル)
と機械的なオラクル (§8) だけです。

### 3.5 SMP は有効

v1 の失敗は UP だったことではなく、**UP から出られない形の UP** だった
ことです (setjmp コルーチン)。

kobox2 は、データ構造とロックを最初から SMP 前提で設計します
(per-CPU、本物の spinlock 意味論、RCU)。Linux では高速化や遅延実行により、
通常の操作も別 CPU の workqueue / RCU / writeback と関係します。そのため初期の
checkpoint から SMP を有効にし、OS の native thread 上に構築します。

**retrofit だけは行いません。** LKL が `CONFIG_SMP=n` を選んだのは
アーキテクチャ上の決断であり、v1 が UP なのは事故です。両者は別物です。

### 3.6 PachaOS は最初の port であり、依存先ではない

kobox2 は最初に PachaOS で動かします。しかし分離単位は「PachaOS で今ある
API」ではなく、「sandbox を実行する host が満たす能力」です。

```text
OS 非依存                      OS port
---------------------------   --------------------------------
controller state machine   -> sandbox create / stop / resource transfer
wire protocol              -> channel / shared-memory / notification
Linux sandbox runtime      -> thread / VM / time / wait / log
device adapter             -> MMIO / DMA / IRQ / reset / revoke
```

境界は二つに分けます。

1. **host contract** — sandbox process 内の C ABI。native thread、memory、time、
   wait/wake、log と、必要な場合だけ device capability を facet ごとに渡す。
2. **wire protocol** — controller、filed2、sandbox 間の process 境界。固定幅整数と
   opaque ID だけを使い、pointer、PachaOS FD 番号、kernel object layout を
   wire に載せない。capability の実転送方法は port が持つ。

`host contract` は Linux API の再定義ではありません。Linux の `kmalloc`、
`kthread`、RCU、workqueue、page cache などの意味論は GPL 側が所有し、その
最下層が必要とする OS mechanism だけを host contract へ落とします。

依存規則は CI で固定します。

- `kobox` と `kobox-linux` に PachaOS header / syscall wrapper を置かない
- PachaOS の syscall number / FD / VMO を書けるのは PachaOS リポジトリ内の
  kobox port / role 実装だけ
- `test` host port でも同じ loader と core / subsystem `.so` 群を build / test する
- protocol と host contract は別に version を持ち、PachaOS ABI version と
  同一視しない

PachaOS port はまず既存の process / thread / VMO / IPC / capsule capability で
実装します。既存 mechanism で満たせないことが実測で確定するまでは kernel を
変更しません。PachaOS port の source と role packaging は PachaOS リポジトリが
所有します。将来のマイクロカーネルも自身のリポジトリに port を実装し、Linux
source と controller state machine を変更せず通ることを移植完了条件にします。

### 3.7 role ごとに kobox instance を作る

`kobox` と `kobox-linux` は mechanism、protocol、再利用可能な `.so` / loader を
提供します。`filed` や `netd` という PachaOS service の知識は持ちません。

PachaOS リポジトリが role ごとの構成を所有します。

- **filed-kobox** — storage device capability、filed protocol、core / device-pci /
  blk / fs `.so`、NVMe / ext4 stack の `.ko`
- **netd-kobox** — network device capability、netd protocol、core / device-pci /
  net `.so`、対象 NIC driver `.ko`

各 role は独立した controller instance、sandbox process、resource manifest、ring、
generation、restart domain を持ちます。`.so` の build artifact は再利用できますが、
runtime state、FD/capability、DMA domain、Linux object は role 間で共有しません。
万能な中央 kobox daemon に全 driver を集約しないことが、隔離と最小ロード集合の
両方に必要です。

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

以下は **PachaOS の filed-kobox 配置**です。service、role controller、sandbox
という関係は共通ですが、process 作成や capability 移送の方法は host port が
持ちます。

```text
filed2 プロセス
  namespace / vnode / OFD / path policy / exec policy
        │
        │  ring buffer  (データパス。kobox2 は介在しない)
        │
        ├──────────────────────────┐
        │                          │
   filed-kobox controller プロセス  │
     監督 / role manifest 検証 / resource policy
     (control plane のみ)          │
        │  ring buffer             │
        │  (control)               │
        ↓                          ↓
   filed-kobox sandbox プロセス  (専用 IOMMU domain)
     - .so / .ko self-loader
     - Linux core/primitive .so (kernel/mm/lib の共通機構)
     - device-pci subsystem .so (device model, PCI, DMA API, IRQ glue, ...)
     - blk subsystem .so       → nvme-core.ko / nvme.ko
     - fs subsystem .so        → jbd2.ko / mbcache.ko / crc16.ko / ext4.ko
```

これは最終的な storage profile の図です。Phase 4 の NVMe profile は fs subsystem
`.so` と ext4 stack をロードせず、Phase 5 で初めて下段を追加します。

netd-kobox も同じ型ですが、別 instance です。

```text
netd ───────────────→ netd-kobox sandbox
  └─ control ──────→ netd-kobox controller

netd-kobox sandbox:
  Linux core/primitive .so + device-pci .so + net .so + NIC driver .ko
```

**controller はデータパスに入りません。** `filed2 → controller → sandbox` の
直列構成では、I/O 1 往復あたり ring を 4 回横断します。controller は channel を
確立したら退き、filed2 と sandbox が直接 ring を共有します。

これは性能のためだけではありません。controller を control plane に限定する
ことが、§3.1 の境界を最も強く担保します。データを見ない
component は、データの意味を知る必要がないからです。

### 4.4 所有と authority を分ける

§4.3 の「controller はデータパスに入らない」は、このままでは **規約** です。
守るのは実装者であり、一度でも「ここだけ kobox2 が中継すれば早い」を通せば
崩れます。規約は性能圧の下で必ず破れます。

別 OS (形式検証マイクロカーネル) の DMA モデルが、これを不変条件へ変える形
を示しています。

> Device / MMIO / IRQ / DMA mapping の実所有者は worker sandbox。
> controller は生成・停止・再割当てを要求する管理 authority だけを持つ。

kobox2 controller に写すと、**sandbox の生死を制御する権限は持つが、sandbox が触る
メモリやデバイスへの参照は保持できない**。データパスに入らないのではなく、
**入れない**。capability の型がそれを禁じます。

PachaOS 上での形:

- device FD / DMA mapping FD / ring VMO の所有者は sandbox。controller は
  それらを自分の FD 表に持たない
- controller が持つのは「sandbox を作る / 止める / 再生成する」権限のみ
- filed2 ↔ sandbox の ring は両者が直接共有する。controller は確立を仲介した
  のち、自分側の参照を落とす

**確立時に一時的に参照を持つのは避けられません。** 条件は「確立完了時点で
controller 側から到達不能になっていること」であり、これは FD 表を見れば検証
できます。§3.1 の「小さい controller」を、意図ではなく構造で担保する形です。

## 5. ring buffer

### 5.1 lock を避ける形

単一の lock-free MPMC ring は、正しく書くのが難しく、書けてもキャッシュ
ラインで競合します。kobox2 は **スレッド / キューごとの SPSC ペア**
(submission + completion) を採ります。

これは NVMe の SQ/CQ そのものであり、`nvme.ko` と blk-mq が前提とする
per-CPU hardware queue の形とも一致します。lock-free が自明になり、SMP で
そのままスケールし、抽象化の継ぎ目が自然になります。

### 5.2 意図的に狭める

二つの interface を混同しません。

- sandbox **内**の host contract は、facet ごとの関数ポインタ構造体に
  閉じ込め、それを超える OS 依存がコンパイルエラーになる形にする
- process **間**の ring は pointer や関数ポインタを一切含まない wire format
  とし、operation、request ID、generation、offset、length、flags だけを持つ

LKL の `lkl_host_operations` (~30 コールバック) は前者の形が成立することの
実証ですが、後者を同じ vtable に押し込む根拠にはしません。wire operation は
filed / block / control の意味ごとに分け、万能な `request(void *)` を作りません。

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

この節の既存値は ext4 profile の測定であり、実装順を意味しません。Phase 0 では
先に同じ source / config から `nvme-core.ko` / `nvme.ko` の undefined symbol closure
を再生成し、core/primitive / device-pci / blk subsystem `.so` へ割り当てます。その
profile が fs subsystem `.so` を要求しないことを最初の境界検査にします。
ext4 の以下の測定は Phase 5 で使用します。

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
| string / lib / bitmap / printk / time / crypto | ~74 | Linux core `.so` へ持ち込む |
| VFS core | 84 | fs subsystem `.so` へ移植 (filed2 が唯一の消費者なので薄くできる) |
| page cache / folio / writeback | 42 | Linux core/primitive `.so` で**本実装** (§7) |
| buffer_head | 33 | fs subsystem `.so` で**本実装** (§7) |
| bio / block layer | 14 | blk subsystem `.so` へ移植 |
| mm alloc / sched / wq / kthread / lock / RCU / per-CPU | 85 | Linux core/primitive `.so` として**書く** |
| その他 (proc/sysfs, security/cred, misc) | ~90 | 個別判断 |

**当初「どこまで流用しどこから再実装するか」が最難関と見ていたが、
実測では 193 個 (A+B+C) の判断が自明**でした。真に設計判断を要するのは
§7 の一点です。

> **注意:** この分類は heuristic であり、元データ `.artifacts/ext4-u.names` は
> 過去の調査 artifact です。基準版として採用する v6.18.48 の source + kobox2
> config から再生成するまで、symbol 数や導入 version の推定を境界仕様に使わないこと。

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

**結論:** page cache は Linux core/primitive `.so` で、shim ではなく本実装すべき
層です。
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
一致するかは機械的に判定できます。**部品は v1 に既にあります** (§13)。

**機構 5 の内容:**
- ファイルスコープの固定長配列を禁止 (`KB_FS_FILE_MAX = 256` が生まれた経路)
- 数値のオフセット定数を禁止 (機構 1 により、そもそも不要になる)

## 9. ライセンスとリポジトリ

| リポジトリ | ライセンス | 中身 |
|---|---|---|
| kobox | 現行のまま | controller、OS 非依存 protocol、startup manifest schema、controller port interface |
| kobox-linux (新規・完全分離) | **GPLv2** | pin した Linux source fork、Linux core `.so`、device-pci / blk / fs / net 等の非 driver subsystem `.so`、Linux feature manifest、`.so` / `.ko` self-loader、OS 非依存 sandbox host contract |
| PachaOS (本リポジトリ) | 現行のまま | PachaOS controller/sandbox ports、filed-kobox、netd-kobox 等の role manifest / service bridge / packaging |

- **commit #1 から** GPLv2 + SPDX ヘッダを入れること。後から遡って付けるのは、
  他者のコミットが入った瞬間に困難になります。
- **ワイヤプロトコル仕様は非 GPL 側**に置き、両者がそれを実装する形にします。
- Linux source の license / copyright / `COPYING` は upstream のまま保持し、
  基準 tag と kobox patch の対応を機械的に出せるようにします。
- `filed-kobox` / `netd-kobox` の source と設定を `kobox` / `kobox-linux` へ
  逆流させない。これらは PachaOS の service / capability 構成だからです。
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
`CONFIG_FSNOTIFY` で変わります。したがって **`.ko` と、それが使う core /
subsystem `.so` は同じ source + config ツリーから一緒にビルドされねば
なりません。**

将来のベンダドライバについては、その config に合わせる必要が生じます。
source shim + blob 形式 (NVIDIA 等) なら現地コンパイルで緩和されますが、
純バイナリ配布は厳しい。**長期目標の射程を早めに見積もること** (§11)。

### 9.2 Linux 基準版は v6.18.48

kobox2 の最初の基準 tree は、upstream stable の
[**Linux v6.18.48**](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tag/?h=v6.18.48)
(`5bbb9c9f8f808710e2123f2b30f0d61d7d698f52`) に固定します。distro kernel や
`linux-lts-dev` package は header/config の比較資料には使えますが、source の
基準にはしません。

6.12 ではなく 6.18 を選ぶ理由は、将来 target に **RX 9060 XT (Navi 44) の
AMDGPU driver** を含めるためです。Navi 44 の初期対応だけを満たす最古版ではなく、
製品投入後の AMDGPU 修正を含む新しい LTS を基準にします。6.12 と 6.18 の
[upstream projected EOL](https://www.kernel.org/releases.html) はどちらも
2028 年 12 月であり、6.12 を選んでも保守期間上の利点はありません。

固定単位は `6.18.y` という系列だけではなく、**tag の peeled commit + config hash +
toolchain + firmware revision** です。stable point release は自動追従しません。
更新 commit ごとに core/subsystem `.so` と全 `.ko` を同時 rebuild し、symbol/layout
manifest を再生成し、role profile の回帰試験を通してから pin を進めます。

AMDGPU の成立条件は kernel source だけではありません。対応する `linux-firmware`
revision を PachaOS の GPU role manifest で別途 pin し、描画まで対象にする段階では
Mesa/libdrm 側も独立に version 固定します。これらを Linux tag に暗黙追従させません。

## 10. LKL との比較

LKL (Linux Kernel Library) が最も近い先行例です。差は全て**どの高さで切るか**
から出ています。

|  | LKL | kobox2 |
|---|---|---|
| 切る位置 | kernel の**下** (`arch/lkl` として arch port) | kernel の**中** (`.ko` がリンクするコアを供給) |
| Linux module 面 | Linux core 内部なので境界ではない | 対象 `.ko` が要求する **~350** symbol |
| OS host 面 | `lkl_host_operations` **~30** | §3.6 の小さい facet 群。正確な数は未決 |
| 得られるもの | Linux 全部 | ロードしたモジュールだけ |
| runtime 構成 | Linux core 全体を一つの library として載せる | 選択した core `.so` + role profile の subsystem `.so` / `.ko` |
| 入力 | kernel **source** | **binary `.ko`** + 同じ source/config の core / subsystem `.so` |
| 隔離 | 1 アドレス空間 | sandbox process + IOMMU domain |
| SMP | `CONFIG_SMP=n` | SMP 前提で設計 (§3.5) |
| バージョン追従 | out-of-tree rebase が慢性的な痛み | 固定が内在的 |
| syscall ABI | `lkl_syscall()` で提供 | 提供しない |

LKL は上流 Linux source を userland へ移す方法と host operation の設計を学ぶ
先行例ですが、kobox2 の代替ではありません。特に `CONFIG_SMP=n` なので、ext4
だけを対象にしても §3.5 の SMP 条件を満たしません。

### 判断の北極星

設計で迷ったときの基準:

> その選択は「ソースの無いモジュール」と「隔離」に奉仕しているか。
> native-thread SMP と role ごとの最小ロード集合を保っているか。
> それとも Linux core 全体を一つの sandbox に戻しているか。

汎用性や syscall 網羅に手を伸ばし始めたら、切断位置を失った合図です。逆に
§1 の三点に効く投資 (sandbox 境界、IOMMU domain、ローダの厳密さ、SMP、role
profile の制限性) が kobox2 固有の領域です。

### 最初のターゲット

最終 target は `filed2 + nvme + ext4` ですが、実装順は **NVMe → ext4** です。
Linux ext4 が使う block device は Linux の NVMe driver / block layer の上にあるため、
先に `nvme-core.ko` / `nvme.ko` と blk subsystem `.so` を成立させます。NVMe の
ほうが page cache、buffer_head、writeback、jbd2 を要求しないため、最初の実 module
target としても小さい。そこで得た block device の上に、fs subsystem `.so` と
ext4 stack を追加します。

ext4 は source があり、native Linux を正しさの oracle にできるため、kobox2 の
`.so` / `.ko` 分割、SMP、隔離を検証する最初の filesystem 題材に向いています。
その先の具体的な driver target の一つを AMDGPU とし、手元の RX 9060 XT を
実機 oracle にします。ただし filed-kobox の完了条件へ GPU を混ぜず、別 role
profile として依存 closure、firmware、DMA/IRQ/reset 境界を測定します。

## 11. 未決事項

kernel バージョンは §9.2 のとおり **v6.18.48 に決定済み**です。旧 6.8 distro
module と `.artifacts/ext4-u.names` は移行前の調査資料に留め、kobox2 の ABI
根拠にはしません。

| # | 項目 | なぜ先に決めるべきか |
|---|---|---|
| 2 | **ring の wire format** | §5.3。virtio の queue layout を流用するか、用途別の小さい SPSC format を定義するか。pointer-free、generation 付き、IPC ring と device queue を別型にする条件は確定済み |
| 3 | **host contract の最小 operation 集合** | thread / VM / wait / time / log と device facet の正確な切り方。v1 の巨大 backend API を名前だけ変えて持ち込まないため、最初の module と PachaOS 既存 capability の両方から operation を導く必要がある |
| 4 | **sandbox 再起動時の handle 復元 (と device 世代)** | §4.1。filed2 が vnode/OFD を inode 番号から再解決する設計が必要。加えて再起動前の DMA・completion・IRQ を新世代が受理してはならない。まず PachaOS の reset / revoke / IOMMU teardown を userland から実測し、既存 mechanism で保証できないと証明された場合だけ kernel ABI 案を作る。kernel 編集は別途許可を得るまで行わない |
| 5 | **ベンダ binary `.ko` の現実的射程** | §9.1 の config 制約。長期目標が実際にどこまで届くかの見積もり |

core / subsystem `.so` と `.ko` をロードする主体は未決事項ではありません。§3.1 の
とおり、GPL sandbox 側の self-loader に確定します。

## 12. 実装計画

phase は機能数ではなく、**境界を一つずつ実証する順序**で進めます。後続 phase
の都合で前段の境界を崩した場合は、機能が動いていても完了とはしません。

### Phase 0 — 基準 tree と測定値を固定する

- upstream stable の v6.18.48
  (`5bbb9c9f8f808710e2123f2b30f0d61d7d698f52`) と kobox2 最小 config を
  build manifest に固定する
- その tag を基点に `kobox-linux` GPLv2 repository を作り、Linux tree の
  directory 構成と履歴を保持する
- 同じ source + config から `nvme-core.ko` / `nvme.ko` と、ext4 / jbd2 / mbcache /
  crc16 を build する
- Linux core `.so` と device-pci / blk / fs subsystem `.so` ごとに source manifest を
  `kobox-linux` 側で作る
- PachaOS 側に filed-kobox の `nvme` profile と `nvme+ext4` profile を置き、それぞれの
  undefined symbol / dependency closure を再生成する
- v1 の probe image、symbol scanner、`e2fsck` oracle を変更せず再実行できる
  test data として取り込む

**gate:** source commit、config hash、toolchain、module hash、symbol manifest が
一つの build manifest から再現でき、各 source file と出力 core / subsystem `.so`
の対応が追跡できること。全 subsystem を結合した runtime artifact は作らない。
ここでは PachaOS/kernel を編集しない。

### Phase 1 — OS 境界を先に作る

- controller state machine、wire schema、sandbox host contract を header と文書で
  固定する
- `kobox-linux` の `test` host port と、PachaOS リポジトリの controller / sandbox
  port skeleton を同時に build する
- PachaOS include が `kobox` / `kobox-linux` へ逆流しない fence、wire の pointer/FD
  混入検査、upstream tag との差分 report を CI に入れる
- role manifest と service bridge は PachaOS 側だけに置き、filed/netd 名を generic
  repository に入れない
- startup resource は path で探さず、manifest に列挙した opaque resource slot と
  capability transfer だけで受け取る

**gate:** controller、loader、core/subsystem `.so` の generic source に PachaOS header
がなく、同じ protocol test vector が `test` port と PachaOS port の両方を通ること。

### Phase 2 — filed-kobox bootstrap と self-loader

- PachaOS の既存 process / thread / VMO / IPC を使い、PachaOS リポジトリの
  filed-kobox controller から filed-kobox sandbox を起動する
- core / subsystem `.so`、module image、resource capability を渡し、sandbox 内
  loader が `.so` と `.ko` の relocation / symbol closure / init order を解決する
- 最小 Linux core `.so` + fixture `.ko` で load / init / unload / crash / restart を
  固定する
- 最初から 2 本以上の native execution thread で起動し、host contract の
  lock、wait/wake、timer、thread-local CPU ID の concurrency test を通す

**gate:** cross-process memory write、固定 FD 番号、controller 内 Linux symbol table
なしで、manifest に列挙した `.so` と fixture `.ko` だけが反復起動できること。
この phase では raw device / DMA を渡さない。

### Phase 3 — subsystem `.so` の単位を成立させる

- `kernel/` / `mm/` / `lib/` から選んだ core 実装を、一つの Linux core/primitive
  `.so` として build する。mm / scheduler / RCU 等を別 `.so` へ分けない
- device model / PCI core / DMA API / IRQ glue を device-pci subsystem `.so` として
  別 build する
- bio / request / blk-mq / block device core を blk subsystem `.so` として別 build
  し、Linux core `.so` への依存を manifest に明記する
- VFS / buffer_head / filesystem core は fs subsystem `.so` に置く。xarray / page
  cache / writeback 等の `mm/` / `lib/` core 実装は Linux core `.so` に置く
- host 接続は `kobox/host/` に置き、各 subsystem `.so` が OS API を直接呼ばない
- Linux の spinlock、workqueue、kthread、RCU を 2 CPU 以上で最初から有効にする
- `implemented` / `refused` / `unimplemented` manifest と生成 stub を symbol 単位で
  CI 検査する

**gate:** core `.so` だけ、NVMe 用の core + device-pci + blk `.so`、ext4 用の core +
device-pci + blk + fs `.so` を別々に link/load 検査でき、NVMe profile が fs
subsystem `.so` をロードしないこと。上流無変更 file、patch file、kobox 固有 file を
区別でき、silent stub が 0、SMP stress が安定して通ること。

### Phase 4 — filed-kobox で NVMe driver を先に動かす

- Linux core / device-pci / blk subsystem `.so` と `nvme-core.ko` / `nvme.ko` だけを
  target profile に入れる。fs subsystem `.so` と ext4 stack は載せない
- MMIO / DMA / IRQ / reset / revoke を device host facet として追加し、device と DMA
  mapping は sandbox に直接所有させる
- Linux NVMe driver が登録した block device に対して、scratch namespace の identify、
  read、write、flush、timeout、reset、unload / reload を検証する
- sandbox generation を DMA mapping、IRQ、completion、block device handle に伝播し、
  restart 前の event を拒否する

**gate:** blk profile が fs subsystem をロードせずに実 NVMe I/O を完了し、IOMMU
domain の外へ DMA できず、controller が device / mapping を保持せず、reset 中の
delayed completion を新世代が受理しないこと。既存の PachaOS mechanism で満たせない
場合は、再現結果と最小 ABI 案を提示して許可を得るまで kernel へ進まない。

### Phase 5 — filed-kobox の NVMe 上に ext4 を載せる

- Phase 4 の profile に fs subsystem `.so` と `jbd2.ko` / `mbcache.ko` / `crc16.ko` /
  `ext4.ko` を追加し、Linux NVMe driver が登録した block device へ接続する
- PachaOS block service で NVMe を迂回する経路は integration の成功条件にしない。
  image-backed provider は fs subsystem 単体 test にだけ使う
- read-only mount、read、write、fsync、unmount、journal replay の順に通す
- filed2 の vnode/OFD と sandbox inode handle の対応、sandbox restart 後の再解決を
  設計・検証する
- 同一操作列を native Linux と kobox2 に与え、operation result、正規化した
  metadata / block 差分、`dumpe2fs`、`e2fsck`、crash point ごとの replay 結果を
  比較する。決定的な fixture では image hash も比較する

**gate:** Linux core `.so` の page cache / writeback と、fs subsystem `.so` の
buffer_head / VFS、`jbd2.ko` を省略せず、電源断相当の全 checkpoint で NVMe 上の
image が整合し、sandbox 再起動後に stale filed / inode / block handle を受理しない
こと。

### Phase 6 — PachaOS 内で role 分離を実証する

- filed-kobox の構成から PachaOS 固有の role template を抽出する。ただし generic
  kobox repository へ filed/netd の知識を移さない
- netd-kobox を PachaOS リポジトリに追加し、Linux core `.so`、device-pci / net
  subsystem `.so`、一つの NIC driver `.ko` だけをロードする
- filed-kobox と netd-kobox が別 controller / sandbox / capability / DMA domain /
  generation / restart domain を持つことを検査する

**gate:** netd-kobox が blk / fs `.so` や storage `.ko` をロードせず、filed-kobox が
net `.so` や NIC `.ko` をロードしないこと。一方の crash / restart / device reset が
他方の state と data path に影響しないこと。

### Phase 7 — gpud-kobox で RX 9060 XT の AMDGPU driver を動かす

- PachaOS リポジトリに gpud-kobox の controller / sandbox port / role manifest を
  追加し、Linux core `.so`、device-pci / drm subsystem `.so` と AMDGPU の `.ko`
  dependency closure だけをロードする
- v6.18.48 と組にする `linux-firmware` commit、必要 firmware file の hash、license を
  manifest に固定し、path lookup ではなく起動 resource として sandbox へ渡す
- 最初に PCI probe、IP discovery、firmware load、VRAM/GTT allocation、ring/fence、
  interrupt、GPU reset を通し、その後に KMS と render client 境界を追加する
- BAR / doorbell / VRAM / GTT / DMA mapping / IRQ / reset capability と generation を
  gpud-kobox 専用にし、filed-kobox / netd-kobox と共有しない
- 描画を始める段階で Mesa / libdrm も個別の source revision と build manifest に固定し、
  kernel tag や firmware revision への暗黙依存を禁止する

**gate:** 手元の RX 9060 XT で probe から GPU ring test、IRQ、reset、sandbox
restart まで反復でき、stale fence / IRQ / DMA completion を新世代が受理しないこと。
gpud-kobox が blk / fs / net subsystem `.so` をロードせず、他 role の crash / reset が
GPU state に影響せず、GPU reset が他 role の device domain に影響しないこと。

### Phase 8 — 第二のマイクロカーネル port

- 将来のマイクロカーネルが必要な process / thread / VM / channel / device capability
  を提供できる段階で、controller port と sandbox host port を追加する
- Linux source、self-loader、Linux feature manifest、host contract は変更しない。
  新しい OS の role protocol / packaging は、その OS のリポジトリが所有する。移植で
  不足が出た場合は、まず port の問題か host contract の漏れかを分類する

**gate:** OS 固有変更が新しい OS 側 repository の port と role packaging に閉じ、
Phase 2〜5 で使った loader / SMP / `.so` / `.ko` の共通 test vector が通ること。
共通 loader / subsystem source の変更を要した場合は、PachaOS 固有概念が漏れていた
設計不具合として記録する。

## 13. v1 から継ぐもの・捨てるもの

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

## 14. 既存文書との衝突

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
