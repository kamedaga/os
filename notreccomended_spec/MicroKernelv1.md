# Capability OS
## Phase 1: UEFI最小カーネル仕様

## 1. 目的
UEFI 環境上で動作する最小カーネルを構築し、以下を実現する。

- メモリ領域の抽象化（Region）
- Capability テーブル管理
- capability による保持主体の管理
- DMA 開始/完了ロジック
- シリアルログによる状態確認

本段階で未実装とするもの:

- スケジューラ
- 仮想メモリ管理
- 割り込み処理
- virtio デバイス制御
- ユーザ空間分離

## 2. 実行環境
- QEMU x86_64
- UEFI（OVMF）
- Zig による UEFI アプリ形式のカーネル

起動形式:

- EFI アプリ（`BOOTX64.EFI`）
- UEFI が long mode とページングを提供

## 3. 基本データ構造
### 3.1 Principal
主体を識別する ID。Phase1 は単一プロセス/単一デバイスとする。

```text
enum PrincipalId {
    Process0,
    Device0
}
```

### 3.2 Region
メモリ領域の抽象単位。Phase1 では物理アドレスは扱わない。

```text
struct Region {
    id: u64
}
```

### 3.3 Rights
`write` は Phase1 では省略可能。

```text
struct Rights {
    read: bool
    dma: bool
}
```

### 3.4 Capability
```text
struct Capability {
    region_id: u64
    rights: Rights
}
```

### 3.5 Capability Table（CNode）
```text
struct CNode {
    caps: []Capability
}
```

- Principal ごとに 1 つ保持
- 動的拡張は不要（固定配列でよい）

## 4. システム状態
カーネル内部に保持する状態:

```text
regions: []Region
cap_tables: map PrincipalId -> CNode
```

補足:

- Region は `id` のみを持つ。
- 「誰が保持しているか」は `cap_tables` を走査して決定する（`scanCapTables(region_id)`）。

## 5. 初期化仕様
UEFI エントリで以下を実行する。

1. シリアル出力を初期化
2. メモリマップ情報を取得（descriptor 数と map key が取得できる状態）
3. `ExitBootServices` を実行（必要なら map key 再取得でリトライ）
4. `ExitBootServices` 成功後、System Table の以下ポインタを `null` 化し CRC32 を再計算
5. Region 配列を初期化（以降はベアメタル処理）
6. Capability テーブルを初期化

`null` 化対象:

- `console_in_handle`
- `con_in`
- `console_out_handle`
- `con_out`
- `standard_error_handle`
- `std_err`
- `boot_services`

初期状態:

- `Process0` に `region0` の capability を付与（`read=true, dma=true`）
- `Device0` は空
- `scanCapTables(region0) == Process0`

## 6. DMA仕様（Phase1）
### 6.1 `start_dma(region_id)`
前提条件:

- `Process0` が当該 region の DMA 権限を保持
- `Device0` は当該 region の capability を未保持

動作:

- `Process0` の capability を削除（DMA中アクセス禁止）
- `Device0` に DMA-only capability を追加
- `scanCapTables(region_id)` は `Device0` を返す

ログ:

```text
DMA start: region X
holder -> Device0
```

### 6.2 `complete_dma(region_id)`
前提条件:

- `Device0` が DMA capability を保持
- `Process0` は当該 region の capability を未保持

動作:

- `Device0` の capability を削除
- `Process0` の capability に DMA 権限を復帰
- `scanCapTables(region_id)` は `Process0` を返す

ログ:

```text
DMA complete: region X
holder -> Process0
```

## 7. 安全性条件（実装保証）
### 7.1 所有権一意性
- 各 Region は capability 走査結果として高々 1 Principal のみが保持者になる（`Shared` は禁止）

### 7.2 DMA中のアクセス禁止
- `scanCapTables(region_id) == Device0` の間、`Process0` は当該 region capability を保持しない

### 7.3 権限単調性
- DMA 開始時に `Process0` から DMA 権限を削除
- 同時に両者が DMA 権限を保持しない

## 8. 実行フロー（デモ）
UEFI 起動後のシーケンス:

1. シリアル初期化
2. メモリマップ取得ログ出力
3. `ExitBootServices` 成功
4. UEFI サービス完全終了（Boot Services 非参照）
5. ベアメタル状態で初期 Capability 状態ログ出力
6. `start_dma(region0)`
7. 状態ログ出力
8. `complete_dma(region0)`
9. 状態ログ出力
10. 無限ループ

## 9. Phase1到達目標
- UEFI 上で起動し `ExitBootServices` まで到達
- `ExitBootServices` 後に UEFI 非依存で継続動作
- Capability 構造体が動作
- capability 走査で保持者遷移が確認可能
- DMA delegation 概念が実装に反映済み

## 10. 次段階（Phase2）
- 仮想メモリ導入
- 割り込みハンドラ導入
- virtio-net MMIO 初期化
- DMA 実メモリコピーとの接続

## 11. Kernel化チェックリスト（現到達点）
- UEFI 起動
- メモリマップ取得
- `ExitBootServices` 成功
- UEFI サービス完全終了
- ベアメタル上で Capability/DMA 処理（`start_dma` / `complete_dma`）実行
- 最後に無限ループで停止

## 12. 自前ページテーブル仕様（Kernel化拡張）
目的:

- UEFI 提供ページテーブル依存を廃止し、カーネル自前ページテーブルへ移行

実装要件:

1. PML4 作成（4KB align）
2. PDP 作成（4KB align）
3. PD 作成（4KB align、2MB large page）
4. `0x00000000` 〜 `0x3FFFFFFF`（0〜1GiB）を identity map
5. `CR3` を新しい PML4 物理アドレスへ書き換え

マッピング詳細:

- `PML4[0] -> PDP`
- `PDP[0] -> PD`
- `PD[i] = (i * 2MiB) | Present | RW | PS`（`i = 0..511`）

実行順序:

1. `ExitBootServices` 成功
2. UEFI サービス終了処理
3. 自前ページテーブル構築
4. `CR3` 切替
5. ベアメタル Capability 処理継続

注意事項:

- 現段階は identity map を 0〜1GiB のみに限定
- ページテーブル実体も 1GiB 未満配置を前提
- 1GiB 超領域のマップは次段階で拡張

## 13. GDT 最小構成（ring3準備）
目的:

- ring3 遷移に必要な user セグメントを事前定義する

エントリ構成:

- `0x00`: null
- `0x08`: kernel code（DPL=0）
- `0x10`: kernel data（DPL=0）
- `0x18`: user code（DPL=3）
- `0x20`: user data（DPL=3）

実装要件:

1. 上記5エントリの GDT を定義する
2. `lgdt` でロードする
3. far return で `CS=0x08` に再ロードする
4. `DS/ES/SS=0x10` を設定する
5. IDT gate の selector は kernel code（`0x08`）を使う
