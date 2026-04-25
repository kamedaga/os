# virtio-blk Capability Demo

このページは、`virtio-blk` が本当に capability で挙動を変えていることを、
shell からそのまま試すためのメモです。

いまは `cap blk ...` で次のことができます。

- 状態を見る
- `read-only` に切り替える
- `iommu` `virtqueue` `command` を revoke する

## 先に結論

見たいポイントは 3 つです。

- `cap blk profile read-only`
  - 読み取りは通るのに、書き込みだけ止まる
- `cap blk revoke iommu`
  - その後の block read も止まるので、`exec` まで失敗する
- `cap blk revoke command`
  - queue や DMA が残っていても command 側で止まる

これが見えれば、
`virtio-blk` が単に動いているだけではなく、
`何をしてよいか` を capability ごとに分けていることがかなり分かりやすくなります。

## いま使えるコマンド

shell で使うのはこのあたりです。

```text
cap blk status
cap blk profile full
cap blk profile read-only
cap blk revoke iommu
cap blk revoke virtqueue
cap blk revoke command
```

補足です。

- `cap blk profile no-iommu`
- `cap blk profile no-virtqueue`

この 2 つの command surface はありますが、現状は `unsupported` を返します。
今のデモで本当に使うのは `full` と `read-only`、それから `revoke` です。

## build

repo root で次を実行します。

```powershell
pactl setup diff
```

`pactl setup` 系は少し遅くても userland と boot image までまとめて更新する方向にしてあります。

## 起動

```powershell
pactl run
```

起動したら shell でまずこれを打ちます。

```text
cap blk status
```

こんな感じの表示になれば OK です。

```text
blk process=...
profile=full
iommu=active
virtqueue=active
command=active
```

## デモ 1: read-only に切り替える

まずはこれがいちばん見やすいです。

```text
cap blk profile read-only
cap blk status
```

期待する見え方はこんな感じです。

```text
profile=read-only
iommu=active
virtqueue=active
command=active
```

ここで大事なのは、
`command` 自体が消えているわけではなく、
`write/flush` を含まない command subset に切り替わっていることです。

そのあとで、

- 読み取り系の操作
- 書き込み系の操作

を試します。

期待する結果:

- 読み取りは通る
- 書き込みは失敗する

つまり、
`DMA` も `queue` も残っているのに、
`write` だけ止められる、というのがこのデモの肝です。

元に戻すときはこれです。

```text
cap blk profile full
cap blk status
```

## デモ 2: iommu を revoke する

次は壊す側のデモです。

```text
cap blk revoke iommu
cap blk status
```

期待する見え方:

```text
iommu=revoked
```

この状態だと、その後の block request で DMA map を張れないので、
読み取りも止まります。

実際には `persistent_fs` が block read に依存しているので、
その先の `exec` まで失敗して見えることがあります。

これはバグというより、むしろ期待通りです。

- `virtio-blk` は `iommu capability` に本当に依存している
- その影響が filesystem や `exec` にまで伝わる

ということが見えています。

注意:

- `revoke` は戻し操作ではありません
- 元に戻したいときは VM を再起動するのがいちばん簡単です

## デモ 3: command を revoke する

```text
cap blk revoke command
cap blk status
```

期待する見え方:

```text
command=revoked
```

この場合は、
queue や DMA の準備が残っていても device command を発行できないので、
`virtio-blk` の request は成立しません。

`read-only profile` との違いはここです。

- `read-only`
  - command はあるが `read` だけ
- `revoke command`
  - command 自体を止める

## デモ 4: virtqueue を revoke する

```text
cap blk revoke virtqueue
cap blk status
```

期待する見え方:

```text
virtqueue=revoked
```

この場合は request を queue に載せられないので、
device まで届かなくなります。

見え方としては、

- command はある
- DMA の準備もできるかもしれない
- でも transport が止まる

という感じです。

## おすすめの順番

はじめて見るならこの順番がおすすめです。

1. `cap blk status`
2. `cap blk profile read-only`
3. 読み取りは通る / 書き込みは止まる、を確認
4. `cap blk profile full`
5. `cap blk revoke iommu`
6. その後の `exec` や file access が止まるのを確認

この流れだと、

- 細かい権限制御
- 強い revoke

の両方が見えます。

## いまの実装で分かること

現時点の `virtio-blk` では、少なくとも次が言えます。

- `command capability`
  - `read/write/flush` の意味側を分けられる
- `iommu capability`
  - DMA を張れるかを止められる
- `virtqueue capability`
  - ring を進める権限を止められる

なので、
driver 全体を丸ごと on/off するのではなく、
driver の authority を分解して扱えていることが見えてきます。

## まだ途中のところ

今のデモは十分面白いですが、まだ途中の部分もあります。

- `cap blk profile no-iommu`
- `cap blk profile no-virtqueue`

この 2 つは command 名はありますが、まだ `unsupported` です。
ここまで可逆にやるには、
`iommu/virtqueue` 側も profile 切り替え用の source token 管理をもう少し詰める必要があります。

## 関連文書

背景の設計を見たいときは、こちらもどうぞ。

- [virtio_blk_persistent_fs_plan_ja.md](../docs/virtio_blk_persistent_fs_plan_ja.md)
- [dma_iommu_vm_object_model_ja.md](../docs/dma_iommu_vm_object_model_ja.md)
- [kernel_capability_architecture_ja.md](../docs/kernel_capability_architecture_ja.md)
