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
