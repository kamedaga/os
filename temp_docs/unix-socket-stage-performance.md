# UNIX socket 処理別計測と高速化

**完了**: 主要24条件とLPR/unixd処理別内訳を実測し、支配的だったserver制御ページmap/unmapを再利用した。最終通常版でQEMU全回帰、20プロセスの退去・再登録、XFCE/Writer操作を確認。kernel変更なし。最新結果は末尾。途中の未完了・失敗記録は履歴として残す。

開始点: `4a20ceb`。目的は主要経路を処理単位で実測し、その表で支配的な処理を選んで高速化すること。計測前に高速化対象を決めない。kernel変更は必要性をサブエージェントに審査させ、許可を得た場合のみ。

## 計測範囲

| 経路 | 分解して測る処理 |
| --- | --- |
| STREAM / SEQPACKET 送受信 | context・mapping準備、共有キュー予約、本文コピー、公開/消費確定、通知、待機登録、arm/recheck/待機、終了処理 |
| DGRAM 送受信 | 上記に加え、ROUTE/HEAD RPC、世代付きmapping再利用、COMMIT/CONSUME RPC |
| 制御RPC | ページ取得、要求直列化、IPC_CALL、返信待ち、応答検証、reply FD close、ページ返却 |
| 名前付き接続 | socket生成、bind、listen、connect、accept、close。pathnameとabstractを別測定 |
| 通知/付随データ | poll/epoll ready検査、別threadとのblocking往復、SCM_RIGHTS付き送受信 |

`lpr_unix_stage_bench`で3種類を個別プロセスとして測定。64 Bと16 KiB、反復・複数試行。本文に反復番号を入れて照合し、SCM_RIGHTSは受信FDの内容も確認する。内訳は`LPR_UNIX_PROFILE=1`専用ビルドだけで集計し、終了時にserialへ出力する。通常版で同じベンチを実行して計測負荷と実際の改善を区別する。

TSCはlfence付きで採取し、各プロセスでCLOCK_MONOTONICの200ms区間と較正する。TSC経過時間にはdescheduleと待機時間が含まれ、CPU実行時間とは呼ばない。集計時計の量子化と計測コードの負荷もある。内訳は呼出回数と平均時間を示し、入れ子のRPC/送受信総時間を子項目に加算しない。通常版の試行中央値を最終的な速度比較に使う。

## 進捗

- 計測専用ビルド成功。Linuxで7モード×3種類の内容照合まで成功。
- 通常版と計測版のQEMU 24条件、表に基づく高速化・再計測・最終GUI確認まで完了。

## 最適化前の実測

4 vCPU / KVM、同じkernel、各3試行。通常版は`unix-stages-before/`、計測版は`unix-stages-profile-before/`（いずれも`.artifacts/test-results/`内）。生ログ・ハッシュ・全行の`table.md`を保存。再生成は`python3 tests/unix_stage_report.py <directory>`。

| 通常版、64 B、µs/反復の中央値 | STREAM | SEQPACKET | DGRAM |
| --- | ---: | ---: | ---: |
| ready：同一threadの送信＋受信 | 10.982 | 11.091 | 433.611 |
| rtt：別threadとの要求＋応答 | 85.614 | 84.252 | 1925.539 |
| poll readyを含む送信＋受信 | 16.946 | 16.995 | 547.395 |
| epoll readyを含む送信＋受信 | 17.025 | 17.351 | 557.468 |
| SCM_RIGHTS（FD内容確認・closeを含む） | 1227.792 | 1225.105 | 1446.322 |
| pathname接続・送受信・close | 2492.599 | 7539.028 | 2219.410 |
| abstract接続・送受信・close | 1718.953 | 4506.802 | 1520.840 |
| ready：16 KiB送信＋受信 | 15.884 | 16.026 | 466.843 |

## 64 B readyの処理別内訳（計測版、µs/call）

各send/recvは768回。通常版の試行中央値とは異なり、この表は計測版の全呼出平均。総時間には計測コード負荷も含む。RPC列は下位処理を含むため二重加算しない。

| 処理 | STREAM送信 | STREAM受信 | SEQ送信 | SEQ受信 | DGRAM送信 | DGRAM受信 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LPR I/O全体 | 1.753 | 1.912 | 1.754 | 1.745 | 232.579 | 237.241 |
| context等の準備 | 1.410 | 1.564 | 1.408 | 1.380 | 1.548 | 1.525 |
| ROUTE / HEAD | — | — | — | — | 115.471 | 115.013 |
| route mapping再利用 | — | — | — | — | 0.195 | 0.143 |
| 共有キュー予約 | 0.037 | 0.032 | 0.036 | 0.030 | 0.076 | 0.066 |
| 本文コピー | 0.028 | 0.031 | 0.027 | 0.029 | 0.070 | 0.072 |
| 公開/消費確定（DGRAMはRPC） | 0.029 | 0.030 | 0.028 | 0.029 | 114.720 | 119.953 |
| 通知 | 0.075 | 0.090 | 0.087 | 0.085 | 0.132 | 0.136 |
| 終了処理 | 0.014 | 0.014 | 0.014 | 0.014 | 0.054 | 0.059 |

空のTSC計時は約0.012µs/回。nanosecond級の行はこの負荷を無視できない。Linux syscall境界のsend/recvはSTREAMで5.554/5.788µsであり、上記I/O本体だけでfrontend全体を説明したことにはしない。

| DGRAM RPC内訳、µs/call、各768回 | ROUTE | COMMIT | HEAD | CONSUME |
| --- | ---: | ---: | ---: | ---: |
| 全体 | 115.314 | 114.581 | 114.895 | 119.754 |
| ページ取得 | 0.033 | 0.033 | 0.068 | 0.033 |
| 要求直列化 | 0.065 | 0.055 | 0.066 | 0.077 |
| IPC_CALL | 6.154 | 6.040 | 6.134 | 6.280 |
| 返信待ち | 107.033 | 106.468 | 106.621 | 111.371 |
| 応答検証 | 0.121 | 0.134 | 0.147 | 0.133 |
| reply FD close | 1.461 | 1.469 | 1.497 | 1.481 |
| ページ返却 | 0.124 | 0.068 | 0.069 | 0.069 |

STREAM blocking RTTでは、送信の通知が平均30.225µs（1536回）、受信waitが60.347µs（769回）。待機なし表にこの時間を混ぜない。

### 表から選ぶ改善対象

DGRAM・SCM_RIGHTS・名前付き接続に共通するRPC待ちを優先する。返信待ちにはサーバ処理とスケジューリングが混在するため、まずunixdの受信・検査/map・dispatch・unmap・reply・closeを追加計測する。STREAM/SEQの準備とLinux frontendも次点であり、既に微小なコピーや世代付きmapping再利用を先に作り直す根拠はない。

## unixd側の追加実測と採用する変更

`unix-stages-server-before/`。通常unixdから計測専用unixdへ切り替えて同じ24条件が成功。以下はDGRAM ready 64 B、各768回のµs/call。

| server処理 | ROUTE | COMMIT | HEAD | CONSUME |
| --- | ---: | ---: | ---: | ---: |
| IPC受信 | 5.824 | 5.724 | 5.719 | 5.642 |
| 検査・map・snapshot | 13.429 | 14.023 | 13.490 | 13.498 |
| dispatch・応答書込 | 0.502 | 0.567 | 0.311 | 0.493 |
| unmap | 25.366 | 26.080 | 26.736 | 26.534 |
| IPC_REPLY経過時間 | 56.486 | 61.873 | 56.359 | 61.769 |
| 受信FD close | 3.180 | 3.145 | 3.136 | 3.182 |

`kernel_review`の読取審査により、IPC_REPLYはclientへ直接実行をhandoffするため、上記時間にclient実行とunixd再開待ちが含まれ得ることを確認。kernel本体のCPU処理時間ではなく、client側とserver側の時間を足すこともできない。kernelの既存metric有効化も現段階では不要との結論。kernelは編集しない。

まず約39µs/RPCの検査/map/unmap部分を対象に、**server側制御ページmappingを再利用**する。

- 初回の通常RPCにVMOを添付してserver発行tokenを受け取る。別の登録RPCは増やさない。以降は同じsession内のtokenだけで呼ぶ。
- unixd全体で最大16 mapping。受信FDは毎回閉じ、mappingによるVMO参照だけを保持。session死亡/execで回収する。
- tokenはserver内で非再利用。別sessionからは解決不可。退去後はdispatchせず専用miss返信とし、clientは要求IDを変えずVMO付きで一度だけ再試行。一般の操作エラーや輸送エラーでは再試行しない。
- MOVE付き要求は初回からページを添付する。miss後には元のdonor FDが残らないため、再試行が必要な経路へ入れない。この点も補助検査済み。
- LPRはページの再利用先session/endpointが変わればtokenを捨てる。既存の排他lease、forkのmapping掃除、PRIVATE|CLOEXEC、失敗RPCのページ破棄を維持。
- 内部wireを8→9へ変更。理由はVMO添付数と制御ページtoken/miss返信の契約変更。opcodeは増やさず、Linux/native syscall ABIは変更しない。LPR/unixd/supervisor/seed0rootを揃えて配置する。

上記の実装と補助検査（cap省略、missでの二重実行防止、不正missの再試行拒否、16枠退去、別session拒否、munmap失敗時の追跡維持）は完了。変更後QEMU・速度比較・GUI検証は未完了。

## 変更後チェックポイント（未完了）

- `unix-stages-after/`: 通常版wire 9で全socket probe（16 fork、40 execを含む）と24計測条件が成功。DGRAM ready 64 Bは433.611→269.897µs、16 KiBは466.843→275.364µs。SCM_RIGHTS付きSTREAMは1227.792→869.349µs。いずれも全操作反復の中央値。通常STREAM readyは10.982→10.934µsでほぼ同じ。
- `unix-stages-profile-after/`: LPR+unixd計測版でも24条件は完走。ROUTEのserver検査/map/snapshotは13.429→2.654µs、unmapは25.366→0.014µsとなり狙った処理の削減を確認。
- その後の追加検査`--eviction-test`は20 thread同時利用のsocketpair作成でENOMEM。全体markerはstatus=1。成功条件待ちを打ち切ってQEMUを停止したためharnessもexit 1。この起動全体をPASSとは扱わない。通常probe成功と追加負荷検査失敗を分ける。
- 同じwire 9でLPR側のserver cache利用だけを一時的に無効化した比較ビルドを作り、ソースを復元済み。この時点では切り分けとGUI確認が未完了だった。結果は以下。


## 最終通常版の比較表（完了）

`.artifacts/test-results/unix-stages-final-checked/`。変更前と同じ起動直後の順序で24条件を先に測定し、その後に退去検査・全probe・従来benchを実行。各3試行の中央値。µsは小さいほど速い。before/afterは経過時間の比。

| mode | type | bytes | before µs | after µs | before/after |
| --- | --- | ---: | ---: | ---: | ---: |
| abstract | STREAM | 64 | 1718.953 | 1288.380 | 1.334 |
| abstract | DGRAM | 64 | 1520.840 | 1122.750 | 1.355 |
| abstract | SEQPACKET | 64 | 4506.802 | 1255.528 | 3.590 |
| epoll | STREAM | 64 | 17.025 | 17.644 | 0.965 |
| epoll | DGRAM | 64 | 557.468 | 340.392 | 1.638 |
| epoll | SEQPACKET | 64 | 17.351 | 18.043 | 0.962 |
| named | STREAM | 64 | 2492.599 | 1979.348 | 1.259 |
| named | DGRAM | 64 | 2219.410 | 1713.411 | 1.295 |
| named | SEQPACKET | 64 | 7539.028 | 2062.264 | 3.656 |
| poll | STREAM | 64 | 16.946 | 17.417 | 0.973 |
| poll | DGRAM | 64 | 547.395 | 344.031 | 1.591 |
| poll | SEQPACKET | 64 | 16.995 | 17.280 | 0.984 |
| ready | STREAM | 64 | 10.982 | 10.936 | 1.004 |
| ready | STREAM | 16384 | 15.884 | 16.659 | 0.953 |
| ready | DGRAM | 64 | 433.611 | 270.174 | 1.605 |
| ready | DGRAM | 16384 | 466.843 | 274.062 | 1.703 |
| ready | SEQPACKET | 64 | 11.091 | 11.068 | 1.002 |
| ready | SEQPACKET | 16384 | 16.026 | 16.955 | 0.945 |
| rights | STREAM | 64 | 1227.792 | 874.520 | 1.404 |
| rights | DGRAM | 64 | 1446.322 | 1048.009 | 1.380 |
| rights | SEQPACKET | 64 | 1225.105 | 872.761 | 1.404 |
| rtt | STREAM | 64 | 85.614 | 83.493 | 1.025 |
| rtt | DGRAM | 64 | 1925.539 | 1537.088 | 1.253 |
| rtt | SEQPACKET | 64 | 84.252 | 84.185 | 1.001 |

DGRAM readyは約1.6〜1.7倍、SCM_RIGHTS付きは約1.4倍。STREAM/SEQの64 B直接転送はほぼ同じ。poll/epollや16 KiB直接転送には約2〜6%遅い測定もあり、全面的な高速化や変動解消は主張しない。SEQの名前付き接続は変更前の試行間変動が大きく、3.6倍という比を一般化しない。

従来benchも内容照合まで完走。DGRAM 16 MiB/16 KiBの中央値56.537 MiB/s、1 MiB/64 Bは0.224 MiB/s。従来64 B STREAM RTTはsocketpair 125.976 µs、pathname 126.953 µs。こちらは1024往復×5試行で、上表のblocking RTTとは別ワークロード。

## 最終検証・残存事項

- `unix-stages-final-checked/`: `UNIX_STAGE_EVICTION=OK processes=20 rounds=12`、`UNIXD_DGRAM_CACHE_FORK=OK cycles=16 routes=12`、`UNIXD_CACHE_EXEC=OK cycles=40`、全socket probeと全bench、最後の`STAGE_CHECKED_EXIT status=0`を確認。harnessは終了marker待ちなので、ゲストのstatus=0も別途確認した。
- 20プロセスは独立したLPRページcacheを保持し、serverの16枠を超えて順番に再使用する。threadの共有4ページpoolを回すだけの検査ではない。初回のプロセス版検査（`unix-stages-final/`）は子終了のSIGCHLDに伴うpipe readのEINTRを検査側が再試行せず中断した。pipeとwaitpidのEINTR処理を直し、上記最終版で全巡回・全子のexit 0を確認した。本体変更によって通したものではない。
- 初回20 thread同時負荷のENOMEMは、同じwire 9でserver cache利用を無効にした`unix-stages-eviction-off/`でも再現（recvmsg: Out of memory、ゲストstatus=1）。今回の再利用が必要条件ではないことまで確認。資源不足の直接原因は未特定・未解消であり、20 thread同時確保の成功は主張しない。再現用`--thread-pressure-test`を残した。比較用ソース変更は除去し、通常LPR/unixdを再配置済み。
- 補助検査: LPR client・native client・server wait/cache・server rights・既存LPR cache・service ABI layoutが成功。ABI検査の古い期待値もclangの実layout出力と照合し、wire 9のsize=1352、diagnostic offset=1184へ更新。cap省略/退去再登録/不正miss/二重実行防止/MOVE/異なるsession/回収失敗の保持を検査。
- `unix-stages-xfce/`: 最終通常版と7バイナリのhash一致。XFCE ready 16.364秒、5アプリ表示PASS。Writerで`Unix stages verified`を入力し`/root/unix-stages.odt`へ保存、終了して新プロセスで再オープン、`Reopened and saved`を追記・再保存・終了。ODTだけをconsole経由で回収しZIP全entryと本文2行の一致を確認した。
- Thunar /usr→/root移動、保存文書表示、Applicationsメニューをマウス操作で確認。スクリーンショットとODTを保存し、確認後にmanual-doneを記録。harness成功終了（287.4秒、手動操作込み）、classification=ready、evidence_complete=true。input_check=not-runは自動入力検査を使わなかった値であり、手動証拠と区別する。
- GUI console/serialにPAGE FAULT・GENERAL PROTECTION・XFCE_SESSION_EXITなし。既存のsystem bus、LibreOffice page-in/Java警告を解消したとは扱わない。DPMS無効化は検証セッション内だけ。アカウント・OS権限機能・kernel・pacha_docsは変更していない。
