---

# Sakura Capability OS

## Phase 1: UEFI最小カーネル仕様

---

## 1. 目的

UEFI環境上で動作する最小カーネルを構築し、以下を実現する。

* メモリ領域の抽象化（Region）
* Capabilityテーブル管理
* 所有権（owner）の管理
* DMA開始／完了のロジック実装
* シリアルログによる状態確認

本段階では以下は実装しない：

* スケジューラ
* 仮想メモリ管理
* 割り込み処理
* virtioデバイス制御
* ユーザ空間分離

---

## 2. 実行環境

* QEMU x86_64
* UEFI (OVMF)
* ZigによるUEFIアプリ形式のカーネル

起動形式：

* EFIアプリ（BOOTX64.EFI）
* UEFIがlong modeおよびページングを提供

---

## 3. 基本データ構造

### 3.1 Principal

主体を識別するID。

```text
enum PrincipalId {
    Process0,
    Device0
}
```

本段階では単一プロセス、単一デバイス。

---

### 3.2 Region

メモリ領域の抽象単位。

```text
struct Region {
    id: u64
    owner: PrincipalId
}
```

物理アドレスはまだ扱わない。

---

### 3.3 Rights

```text
struct Rights {
    read: bool
    dma: bool
}
```

WriteはPhase1では省略可。

---

### 3.4 Capability

```text
struct Capability {
    region_id: u64
    rights: Rights
}
```

---

### 3.5 Capability Table (CNode)

```text
struct CNode {
    caps: []Capability
}
```

* Principalごとに1つ保持
* 動的拡張不要（固定配列でよい）

---

## 4. システム状態

カーネル内部に保持する状態：

```text
regions: []Region
cap_tables: map PrincipalId -> CNode
```

---

## 5. 初期化仕様

UEFIエントリで以下を行う。

1. シリアル出力初期化
2. Region配列初期化

   * Region0 を Process0 所有とする
3. Capabilityテーブル初期化

   * Process0 に

     * region0
     * rights = {read=true, dma=true}
   * Device0 は空

---

## 6. DMA仕様（Phase1）

### 6.1 start_dma(region_id)

前提条件：

* region.owner == Process0
* Process0 が当該regionのDMA権限を持つ

動作：

* region.owner = Device0
* Process0 の capability から dma 権限を削除
* Device0 に dma-only capability を追加

ログ出力：

```
DMA start: region X
owner -> Device0
```

---

### 6.2 complete_dma(region_id)

前提条件：

* region.owner == Device0
* Device0 が dma capability を保持

動作：

* region.owner = Process0
* Device0 の capability を削除
* Process0 の capability に dma 権限を戻す

ログ出力：

```
DMA complete: region X
owner -> Process0
```

---

## 7. 安全性条件（実装上の保証）

実装内で必ず満たすこと。

### 7.1 所有権一意性

* 各Regionは常に1つのPrincipalにのみ属する

### 7.2 DMA中のアクセス禁止

* owner == Device0 の場合

  * Process0 は read 不可

### 7.3 権限単調性

* DMA開始時に Process0 から DMA権限を削除
* 同時に両者が DMA権限を持たない

---

## 8. 実行フロー（デモ用）

UEFI起動後：

1. 初期状態ログ出力
2. start_dma(region0)
3. 状態ログ出力
4. complete_dma(region0)
5. 状態ログ出力
6. 無限ループ

---

## 9. Phase1の到達目標

* UEFI上で動作
* Capability構造体が動く
* owner遷移が確認できる
* DMA delegationの概念が実装に落ちている

---

## 10. 次段階（Phase2）

* 仮想メモリ導入
* 割り込みハンドラ導入
* virtio-net MMIO初期化
* DMA実メモリコピーとの接続

---