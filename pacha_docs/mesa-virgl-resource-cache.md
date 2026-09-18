# Mesa VirGL の render-target / sampler-view 資源再利用

## 対象と変更内容

対象は Mesa 25.1.9。正式に管理する差分は
[0001-virgl-cache-render-target-sampler-view.patch](../patches/mesa/0001-virgl-cache-render-target-sampler-view.patch)
の1ファイル・1行追加のみである。

`src/gallium/winsys/virgl/drm/virgl_drm_winsys.c` の `can_cache_resource()` に、
次の条件を追加する。

```c
bind == (VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW)
```

描画先にもサンプリング元にも使う資源を、既存の再利用キャッシュの対象にする。
完全一致の条件であり、任意の追加フラグを許可するビット判定ではない。
`SAMPLER_VIEW` 単独も新たには許可しない。

サイズ・format・sample count などの互換性判定、busy 判定、external 資源を
キャッシュへ戻さない処理は変更しない。解放時に毎回破棄する代わりに、
条件の合う再利用可能な資源を既存キャッシュから取得する。
新しいキャッシュ機構、submit のまとめ処理、完了確認の共有は追加しない。

これは PachaOS の未実装機能を迂回するための変更ではない。
通常の Linux virtio-gpu 経路でも同じ性能問題と改善を確認した、
Mesa VirGL の性能改善候補である。ただし、全用途で必要・安全と確認済みという意味ではない。
今回許可されたこの最小差分を管理するもので、他の OSS 改変を一般に許可するものではない。

## 測定結果（2026-09-17）

両環境とも KVM 有効、4 vCPU、RAM 2 GiB、ネットワーク有効、GPU の
`ioeventfd=on`、QEMU GTK/GL 表示を使用した。Xorg log の renderer は
`virgl (D3D12 (Intel(R) Graphics))` であり、LLVMPIPE ではない。
アプリは GTK 3.24.50 の `gtk3-demo --run=fishbowl`、対象は Button 1個。
FPS は画面上の表示値5点の中央値で、スクリーンショットの変化回数ではない。

| 環境 | パッチなし | 再利用対象追加のみ | 倍率 |
| --- | ---: | ---: | ---: |
| Linux / 標準 virtio-gpu | 13.84 FPS | 59.91 FPS | 約4.3倍 |
| PachaOS / LPR → gpud → kobox2 sandbox → virtio-gpu | 5.73 FPS | 12.58 FPS | 約2.2倍 |

Linux は各10秒、2秒ごとの5点。標準 GTK Inspector で自動個数調整を停止し、
Inspector を閉じてから測定した。PachaOS は各30秒、約6秒ごとの5点。
自動個数調整は変更せず、全10枚の測定画像で Button 1個であることを確認した。
各条件1回の冷起動 VM による比較で、統計的な信頼区間を求めた試験ではない。
環境間ではウィンドウ位置・準備操作が異なるため、倍率は各環境内の A/B 比較として扱う。

表示値の全サンプルは以下の通り。

| 環境・条件 | FPS（取得順） |
| --- | --- |
| Linux・なし | 14.079705, 13.585823, 13.843458, 13.800690, 13.954633 |
| Linux・追加のみ | 59.870509, 59.909195, 60.033790, 59.902017, 60.076039 |
| PachaOS・なし | 5.969986, 5.733269, 5.406236, 5.408728, 5.796732 |
| PachaOS・追加のみ | 14.470359, 13.677178, 12.243210, 11.402490, 12.584473 |

Linux の QEMU trace では、表示更新1回あたりの資源作成が約125.01回から
0.04回、submit が約11.94回から2.00回へ減った。表示更新は連続する
`virtio_gpu_cmd_res_flush` の完全な区間で数え、GTK のフレームと同一とは仮定しない。
submit 減少は観測結果であり、このパッチが明示的に submit をまとめているわけではない。
PachaOS の今回の trace には時刻がないため、同じ区間集計値は主張しない。

別の試作「完了確認の共有」を併用した Linux 測定は60.05 FPSだった。
この試作は本パッチに含めない。今回の Linux 負荷で60 FPSへ届くには再利用追加だけで
十分だったが、上限に達しているため、追加試作の CPU 負荷削減効果までは未判定である。
PachaOS では本パッチだけで60 FPSには届かず、残るボトルネックの原因は今回未調査。

## 副作用と検証範囲

- 既存キャッシュの期限は1秒だが、回収は次のキャッシュ操作時に行われる。
  タイマーによって必ず1秒後に解放される仕組みではなく、容量上限もこの変更では追加しない。
- Linux でアプリを終了した15秒後の未解放 resource ID 数は、なし101個、追加のみ1,967個。
  Xorg RSS はそれぞれ約98.6 / 98.9 MiBだった。resource ID 数はゲスト全体の値であり、
  VRAM のバイト数ではない。準備操作の履歴・時間も異なるため、差を本パッチだけの
  メモリ増加量と断定しない。物理 GPU メモリ量・メモリ圧迫時の挙動は未測定。
- 過去の Linux CTS 確認は「再利用追加＋完了確認の共有」の併用版が対象だった。
  GLES31 image-load/store buffer の125件と EGL color-clears の対応4件は通過したが、
  EGL の非対応12件は成功扱いにしない。本パッチ単独の CTS 通過実績に読み替えない。
- 単独版の回帰試験、長時間負荷、メモリ圧迫、複数 client・FD 共有・異常終了時の
  再検証は残る。過去の一般 Gate の成功も、この単独版での成功とは扱わない。

## 適用方法と現在の導入状態

Mesa 25.1.9 の管理されたソース作業ツリーで適用する。
パッチそのものの編集場所はこのリポジトリの `patches/mesa/` とし、
一時生成物置き場の `.artifacts/` をパッチの管理元にしない。
リポジトリのルートから、`mesa_source` を実際のソースルートに設定して実行する。

```sh
mesa_patch="$(pwd)/patches/mesa/0001-virgl-cache-render-target-sampler-view.patch"
mesa_source=/path/to/mesa-25.1.9
patch --dry-run --fuzz=0 --forward -p1 -d "$mesa_source" -i "$mesa_patch"
patch --fuzz=0 --forward -p1 -d "$mesa_source" -i "$mesa_patch"
```

二重適用や別バージョンへの強制適用はしない。更新時は対象条件・キャッシュの意味論を
再確認し、同じビルド条件のパッチなし版と比較する。
ABI、Linux kernel、PachaOS kernel、LPR、gpud の変更は本パッチの適用に含まれない。

通常ビルドにも自動適用する。`tools/build_wsl_alpine_mesa.sh` が Alpine の
配布済み APK を取得した後、`tools/build_mesa_virgl.sh` が upstream Mesa 25.1.9
を取得し、このパッチだけを適用して `libgallium-25.1.9.so` をビルドする。
EGL / GL / GBM のローダーは同じバージョンの Alpine 配布版を維持する。
VirGL 以外の Gallium ドライバを削った専用ライブラリにはしない。

リポジトリのルートから通常の対象ビルドを実行する（rootfs への反映も行う）。

```sh
.artifacts/bin/pacgo build userland alpine_mesa
```

ホストは Linux x86_64、`bwrap`（user namespace が利用可能）、`curl`、`tar`、
`patch`、`sha256sum`、`flock`、`readelf` を必要とする。初回はネットワーク経由で
Mesa ソースと専用の Alpine ビルド環境を取得する。ゲスト rootfs はコピーしない。
コンパイルの並列数は `MESA_BUILD_JOBS` で指定でき、既定値は8。

Mesa ソースと Alpine minirootfs は SHA-256 を固定し、対象 APK は
Alpine v3.22 x86_64 の Mesa 25.1.9-r0 に限定する。バージョンが変わった場合や
パッチ適用に失敗した場合は停止し、無修正版への暗黙のフォールバックはしない。
runtime の共通依存 APK は既存の `alpine-xfce-v3.22-x86_64.lock` に掲載された版と
SHA-256 を優先し、既存 clang overlay との同一パス・異なる内容の衝突検査も維持する。
ビルド用の依存 APK は v3.22 のリポジトリから取得し、全依存の版までは固定しない。
したがって、異なる時点の新規ビルドのビット単位の一致は保証しない。

ソース・ビルド出力は `.artifacts/build/mesa-25.1.9/` に生成する。
パッチ、ビルドスクリプト、インストール済みビルド環境のパッケージ情報から
キャッシュを識別し、再利用時はライブラリの SHA-256 を検証する。
pack の入力にもパッチとビルドスクリプトを登録しているため、変更時は再ビルド対象になる。
生成された `/usr/share/pacha/mesa-virgl-build.txt` でソース・パッチ・ライブラリの
識別情報を確認できる。一時生成ソースを手編集して変更を管理しない。

上記 A/B 試験時点ではライブラリを一時差し替え、試験後に配布版へ復元した。
今回の通常ビルドへの接続はその後の変更であり、以降はパッチ適用版が常用対象となる。
upstream PR は未提出。

通常ビルドへの接続後、`pacgo build userland alpine_mesa` によるビルド・rootfs 反映と、
変更なしの再実行で再ビルドが発生しないことを確認した。生成したライブラリと
ディスク内のライブラリの SHA-256 はともに
`917845a08d69770cc7979d075cfe3b20d224d99011716a01471a20f55cfccff0`。
既存の `qemu_xfce_interaction.py` を KVM / Intel D3D12 / ramfb 併用で1回実行し、
壁紙・アイコン、hover、Applications、端末、fishbowl の表示・更新を確認した。
Xorg の renderer は `virgl (D3D12 (Intel(R) Graphics))`。
Button 1個の表示 FPS は 14.962284, 14.272054, 13.531355, 13.587510, 15.935366
（中央値14.27）。これは通常ビルド生成物の動作確認であり、新たな A/B 比較ではない。
ログと画像は [通常ビルド確認](../.artifacts/mesa-normal-build.F5s4Ck/) に保存した。

測定に使った `libgallium-25.1.9.so` の SHA-256:

| 種類 | SHA-256 |
| --- | --- |
| 比較用・パッチなし | `c52fd211c93a26a803566ad4a82f48b3a834d455d6e71d1bda632770dc2c048c` |
| 比較用・再利用追加のみ | `58503d29f5e5f6127054d2b5704acb1e7852c8e38a4f114b0582f7899b444031` |
| PachaOS 試験後に復元した配布版 | `924d4c51a6f4bd3e2f703e23ab681c8efcdc46351ab8e853aec64a599530d83f` |

比較用2本は同一ビルド条件の保存済みバイナリを使用した。
ここでいう「パッチなし」は今回検討した2つの独自変更がないことを意味し、
Alpine の通常のパッケージ差分まで存在しないという意味ではない。

詳細なログ・画像・測定条件は以下のローカル生成物にある。
これらは一時生成物であり、主要な結果とバイナリ識別情報は本書にも残す。

- [Linux の3条件比較](../.artifacts/linux-mesa-split.bsHG5a/comparison.json)
- [PachaOS の A/B 比較](../.artifacts/pacha-mesa-reuse.QVnPKh/comparison.json)
- [併用版の CTS 確認](../.artifacts/deqp-regression.KnmRb9/comparison.json)
