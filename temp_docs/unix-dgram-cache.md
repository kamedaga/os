# DGRAM経路・RPC制御ページ再利用（2026-09-09）

## 実装

- LPRプロセスごとにidle制御ページ4個・idle経路4個だけ保持する。呼び出し中のleaseは別に登録し、同じページや経路mappingを複数threadへ同時に貸し出さない。上限を超える同時呼び出しは通常の一時資源を使い、idleに戻す際に上限内へ退去させる。
- DGRAM_ROUTE / HEADは毎回unixdが所有権・宛先・pathname・配送先頭・資格情報を確認する。LPRのcached_generationが実際の世代と一致するときだけ2個のmapping FDを省略する。COMMIT / CONSUMEと宛先全体の容量・FIFO管理はそのまま。
- 同じ経路のTX/RX VMOを再利用する。RXに送信者が書けず、送信payloadに受信者が書けないRO/RW分離は維持。内部mapping FDはPRIVATE|CLOEXEC、DUP/TRANSFERなしに縮小。制御ページもPRIVATE|CLOEXECとし、forkだけでなくexecでもFDを残さない。
- RPC制御ページは同期replyまで排他的に使い、成功時だけ再利用する。エラー時は破棄し、遅れたserver処理が次のRPCのページを書き換えないようにする。
- socket最終close、threadのclient終了、cache退去でidle資源を解放。VMO確保失敗時はidle資源を解放して一度だけ再試行し、使用中資源は取り上げない。

## forkと契約

kernel_reviewの読取レビューで、PRIVATE FDはforkされないがそのshared mappingは継承されること、今回kernel追加は不要であることを確認した。

cache registry・mappingの生成破棄を専用lockで同期し、native cloneの間だけ同じlockを保持する。broker RPCやsocket待機の間には保持しない。子bootstrapの最初に、消滅した兄弟threadの使用中leaseもidle枠もすべてmunmapする。PRIVATE FD番号はcloseせず、共有本文や共有transport lockも初期化しない。munmap失敗なら子を終了し、不完全な掃除でRPCへ進まない。

内部wire version 7→8。理由はcached_generationの追加とDGRAM mapping FD契約の縮小。古いwireとは混在させず、LPR / unixd / supervisor / seed0rootをまとめて更新した。opcode追加はなく番号変更なし。Linux ABI・kernelは変更なし。

## 検証

- `.artifacts/test-results/unixd-dgram-cache-regression/`: QEMU 4 vCPU、全probe完走、`DGRAM_CACHE_EXIT status=0`。12経路churn、別threadのwarm DGRAM受信待ち中のforkを16回追加。再接続・資格情報・SCM_RIGHTS・epoll・SIGKILL後回収も完走。
- 補助host検査: 100 RPCで制御ページ生成1回、同一経路100回でTX/RX生成各1回、世代不一致、上限退去、使用中＋idle mappingのfork掃除、親mappingの存続、private FD番号をcloseしないこと、munmap失敗、失敗RPCのページ破棄。
- `lpr_unix_client` / mapping / context / unix client / service waits / service rightsのhost検査も通過。

## 速度

比較元は同条件・変更前バイナリの`.artifacts/test-results/unixd-perf-scoped/`で、開始時にハッシュ一致を確認した。変更後は`unixd-dgram-cache-perf/`。4 vCPU / 2 GiB / KVM、console-shell、CPU pinningなし、計測中ビルドなし。同じbench・総転送量・chunk・試行数。旧netd比ではない。

| 各試行の中央値 | 変更前 | cache版 |
| --- | ---: | ---: |
| DGRAM・16 MiB / 16 KiB chunk・3試行 | 23.188 MiB/s | 34.409 MiB/s |
| DGRAM・1 MiB / 64 B chunk・3試行 | 0.092 MiB/s | 0.134 MiB/s |
| STREAM・64 B RTT・1024往復×5試行 | 126.953 µs | 126.953 µs |
| pathname STREAM・同RTT | 125.976 µs | 127.929 µs |
| STREAM・16 KiB bulk | 205.128 MiB/s | 207.792 MiB/s |
| SEQPACKET・16 KiB bulk | 205.128 MiB/s | 205.128 MiB/s |

DGRAMはこの測定で約1.5倍。64 B bulkのSTREAM/SEQPACKETは大きく揺れ、cache版の初回3試行中央値は1.647 / 2.137 MiB/s、別起動10試行はSTREAM 0.894〜8.621、SEQPACKET 0.989〜8.696 MiB/sだった（`unixd-dgram-cache-repeat/`）。DGRAMの改善を全socket種別の改善と扱わない。保持だけを無効にした同wire v8の比較結果は以下に記載。

最初のGUI (`unixd-dgram-cache-xfce/`) は5アプリ表示、Writer保存・新processで再オープン・追記再保存、ODTの内容/ZIP確認、Thunar /usr→/root操作までは確認したが、CLOEXEC補正の追加調査中に手動期限を超えたためharnessはmanual-incomplete/exit 1。最終版の完了根拠にはしない。

## CLOEXEC補正後の最終版

`.artifacts/test-results/unixd-cache-final/`。全probe（12経路churn、16回の並行fork、warm DGRAM socketを保持した40回のexec、SCM/資格情報/epoll/強制終了）と全benchを完走し、`CACHE_FINAL_EXIT status=0`。LPR / unixd / supervisor / seed0root / fixture / kernelのハッシュを保存した。kernelは変更前のまま。

| 最終版の中央値 | 保持OFF・同wire v8 | 保持ON・最終版 |
| --- | ---: | ---: |
| DGRAM 16 KiB bulk | 22.956 MiB/s | 34.261 MiB/s |
| DGRAM 64 B bulk | 0.090 MiB/s | 0.134 MiB/s |

OFFは`.artifacts/test-results/unixd-cache-off/`。cache_endのkeepだけを一時的に0へ固定して別ファイルへビルドし、すぐにソースを復元。比較用LPRだけを一時配置し、比較後は通常LPRを再配置・rootfsへ反映して上記最終検査を実施した。比較用の恒久設定・分岐は残していない。測定時のビルド並行なし。両方とも同じkernel・wire v8・CLOEXECあり。

OFFの小容量STREAM/SEQPACKETにも大きな揺れが再現したため、保持機構がこの揺れの必要条件ではない。揺れ自体は未解決であり、小容量STREAMの高速化や変動解消は主張しない。最終版のSTREAM RTT中央値はsocketpair/pathnameとも126.953 µs。

## 最終版のXFCE / Writer確認（完了）

`.artifacts/test-results/unixd-cache-final-xfce/`。QEMU 4 vCPU、graphics 2d、keyboard-tabletで検証。harnessは成功終了、`classification=ready`、`evidence_complete=true`、XFCE ready検出は16.822秒。5アプリ表示も通過。最終console検査と同一の6バイナリのハッシュ一致を再確認した。

- Writerへ `Dgram final verified` を入力し、`/root/dgram-final.odt` に保存して終了。
- 新しいWriterプロセスで開き直し、元の本文を画面確認。Tipをマウスで閉じ、`Reopened and saved` を追記して再保存・終了。
- rootfsはコピーせず、このODTだけをconsole経由で取り出した。ZIP整合性と`content.xml`内の両方の文字列を確認済み。`dgram-final.odt`と保存・再オープン・再保存のスクリーンショットを保存。
- マウスでThunarの`/usr`→`/root`移動、保存文書の表示、Applicationsメニュー展開を確認。各画面を保存。

自動input checkは`not-run`で、入力確認の根拠は上記手動操作とスクリーンショット・ODT。手動確認後に`manual-done`を記録し、harnessは534.6秒で成功終了した。console/serialにPAGE FAULT・GENERAL PROTECTION・XFCE_SESSION_EXITはなし。DPMSとscreensaverの無効化は検証セッション内のみで、起動設定は変更していない。既存のLibreOffice page-in未実装・Java警告は残るが、今回の編集・保存・再オープンは完走した。
