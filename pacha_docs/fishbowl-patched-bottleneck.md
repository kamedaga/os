# 再利用パッチ適用後の fishbowl 調査

2026-09-17。目標30 FPSは未達。通常ビルドの確認は
[Mesa パッチの記録](mesa-virgl-resource-cache.md)を参照。
まず残る要求数を調べ、その後LPRのstream受信を集約した。
Mesaの追加改変、Xfceの機能削減は行っていない。
後述のIPC診断以外にカーネル変更はない。

## GPU 要求の差

KVM / 4 vCPU / Intel D3D12 / GTK表示 / ramfb併用 / ネットワーク有効。
PachaOSでは通常ビルド版Mesaを使用し、既存のXfce interaction試験で
Button 1個のfishbowlを実行した。QEMUの `virtio_gpu_cmd_*` を時刻付きで採取した。
以下の「表示更新」は連続する `res_flush` の区間であり、GTKフレームとの1対1対応は仮定しない。

| 表示更新1回あたり | Linux・既存の再利用単独測定 | PachaOS・今回 |
| --- | ---: | ---: |
| 資源作成 | 0.0414回 | 0.0146回 |
| submit | 2.0050回 | 15.7664回 |
| submitの合計bytes | 56,576 | 113,417 |

PachaOSの集計対象はepoch `1789650856.979041` から `1789650876.868828`
までの19.8898秒、274区間。submit 4,320回、資源作成4回。
表示更新間隔の中央値は70.51 msだった。計測終了直前の1秒を除いた最後の20秒から
完全な区間だけを採用しており、observerの30秒測定後の継続描画も含む。
Linux側は以前の10秒測定・604区間。
ライブラリのビルド時点、ウィンドウ位置、準備操作が異なるため、
submit差をそのままOS固有の損失や改善可能な倍率と断定しない。

資源再利用は働いている。一方、PachaOSでは小さなsubmitが多数残っているため、
1往復の微調整より先に、この分割の理由を確認する価値がある。

## CPU側の補助観測と次の確認

別実行で30秒のKVM統計差分を採取した。VM exit 3,852,146回、割込み注入765,320回。
639回のレジスタ観測はHLT 267、kernel 331、user 41。
通常カーネルと関数の機械語が一致した範囲で、KernelStateロック待ち、
APIC EOI・timer操作、VT-d invalidationなどが見えた。
QMP呼び出し平均3.84 msの介入とVM exit位置への偏りがあるため、
これをCPU時間の割合や処理のクリティカルパスとは扱わない。

集約前のコード上の調査候補:

- LPRの `lpr_unix/socket.c` は `unix_transport_read_begin()` を1回呼んで戻る。
  `unixd/src/transport.c` の同関数は内部レコード1個までを返す。
  後続の公開済みレコードを同じstream readへ集約する処理はない。
- 無編集のXorg 21.1.19 `glamor/glamor.c` はblock handlerで `glFlush()` を行う。
  受信・dispatchが細切れになるとsubmit増加につながる可能性がある。
- gpudとLPRのEXECBUFFER変換箇所も確認したが、今回読んだ経路には
  コマンド列を小分けにして複数submitするループは見つかっていない。

上記は仮説である。次は、同じ場面のread要求長・返却長・残存レコードと
DRM ioctlの種類・回数・待ち時間を対応させる。SCM_RIGHTSやcredentialsの境界を
壊す集約、EAGAINやbusyを成功に見せる変更、Mesa/Xfceの追加改変は行わない。

gpudの一時計測版は有効なカウンタ出力を得られず、所要時間の証拠には採用していない。
ソースは元へ戻し、rootfsも通常manifestから再同期した。ディスク内のgpudとMesaは
通常生成物とのSHA-256一致を確認した。カーネルソースの変更は行っていない。

ログ・画像:

- [KVM・実行位置](../.artifacts/fishbowl-patched-profile.0uG3bL/)
- [GPU trace](../.artifacts/fishbowl-patched-gpu-trace.okro9i/)
- [不採用のgpud計測と復元ログ](../.artifacts/fishbowl-patched-gpud.gGdDs1/)

## LPR stream受信の集約

一時計測で、要求容量に余裕があり、次の公開済みレコードもあるのに、
先頭レコードだけを返すreadが多数あることを確認した。
比較前の実行ではPID 44の約18.66秒で10,752 read、うち集約可能な呼出しは87.5%。
平均要求長12,514 bytesに対し、平均返却長は1,110 bytesだった。
プロセス名はこのプローブだけでは同定していない。

`unix_transport_read_next()` を追加し、LPRは同じread/readvの容量内で、
開始時点で公開済みだった通常streamレコードをまとめて返す。
reader lockとcommitは一度だけとし、FD/credentialsのticket境界を跨がず、
後から到着するデータは待たない。PEEKはまとめてcancelする。
共有wire ABIは変えず、packet/datagramのメッセージ境界も変更しない。

同じstream/DRM計測を入れた2回の比較結果:

| 観測 | 集約前 | 集約後 |
| --- | ---: | ---: |
| 表示FPS・5点中央値 | 12.533820 | 15.944695 |
| PID 44のstream read/秒 | 576.1 | 111.8 |
| GPU submit/表示更新 | 16.272 | 10.050 |
| WAIT ioctl・累計経過時間 | 7.535秒 / 19.699秒 | 8.340秒 / 19.642秒 |
| WAIT ioctl・平均経過時間 | 225.6 µs | 206.0 µs |
| DIRTYFB ioctl・平均経過時間 | 14.09 ms | 14.87 ms |

FPS全サンプルは集約前14.518936, 12.533820, 11.431119, 11.528441,
15.566469、集約後16.060295, 15.822624, 16.298818, 15.944695, 15.619335。
GPU traceは集約前19.866秒・261区間・4,247 submit、
集約後19.972秒・323区間・3,246 submit。上と同じ末尾20秒の完全区間を使った。
stream/DRMはゲストTSCの70〜90秒に含まれる累積カウンタの差分であり、
GPU traceの集計区間とは一致しない。壊れた/interleaveした計測行は除外した。

各1回の比較であり、FPS差約27%を再現性の保証された改善率とはしない。
別の初期測定ではホスト上の並行ビルドも観測した。
一方、read回数の大幅減少とsubmit分割の減少は狙いと一致するため、変更を残す。
集約後プローブの返却長カウンタは先頭レコードのみを数えるため、
集約後の実返却長としては使わない。

WAITは集約後でも40,485回、DIRTYFBは320回、EXECBUFFERは3,176回。
WAIT約126.5回/更新とDIRTYFBの経過時間が残り、30 FPSには未達。
ioctl経過時間には待ち・スケジューリングを含み、CPU占有時間ではない。
これだけで削減可能量を断定したり、busy判定を省略したりはしない。

transportのwrap、部分read、PEEK、途中の追加publish、ticket境界、
後続レコード破損のunitを追加。transport（ASan/UBSan）、service-rights、
notify、LPR mapping/context、service-waitの既存unitは通過した。
計測専用のLPRヘッダーとhookは削除し、通常版に残さない。

- [集約前・同一プローブ](../.artifacts/fishbowl-read-baseline.wdOhlr/)
- [集約後・同一プローブ](../.artifacts/fishbowl-read-batch.5Rbiby/)

### 通常版への反映と確認

計測hookを除いたLPRとunixdを通常manifestからビルドし、rootfsへ同期した。
ディスク内LPRと通常生成物のSHA-256はともに
`3d354b333693e2e9b2c179e46b52ca472ac2bf957190d6c8223358f7846409dd`。
Mesaは上記通常ビルド版のまま。kobox2作業ツリーに差分はない。

同じKVM / Intel D3D12 / GTK / ramfb構成のinteraction試験が通過した。
Xorgは `virgl (D3D12 (Intel(R) Graphics))` でglamorを初期化した。
計測hookなしのFPSは16.704132, 14.288762, 17.700346, 16.569538,
17.078836、中央値16.704132。単独の動作確認であり、これを新しいA/B改善率にはしない。
通常版でも30 FPSには未達。

- [通常版のログ・画像](../.artifacts/fishbowl-read-normal.7xADzJ/)

## WAITの配送経路と実処理の分離

通常版の16.70 FPSを基準に、native gpudとsandbox adapterだけに一時カウンタを
追加した。上流Linux、core.so、.ko、Mesa、Xfceには追加の変更をしていない。
全native診断hookは計測後に撤去し、通常gpud/sandbox生成物のSHA-256が
それぞれ `a1553dc26477ad3ec22493359c69221c98e875245a1612843d21852ac5e47340`、
`9261e4273e1a1196696a7069850166bc08ae97a0d347220085063b38f79ce11b`
へ戻ったことを確認した。

最初の診断は1回のログがnative上限256 bytesを超え、出力を得られなかったため不採用。
短い行で出す方式へ修正し、カウンタはTSC 70〜90秒内の最初/最後の正常行の差分とした。

gpudだけの計測（19秒前後）:

| 区間 | 回数 | 累計経過時間 | 平均 |
| --- | ---: | ---: | ---: |
| HANGUP監視（18.988秒） | 65,536 | 0.875秒 | 13.35 µs |
| 通信ページ検証・map（18.993秒） | 43,594 | 0.468秒 | 10.74 µs |
| 通信ページunmap（同上） | 43,594 | 1.588秒 | 36.44 µs |
| sandbox RPC（19.672秒） | 45,056 | 12.318秒 | 273.39 µs |
| RPC内の通知send（同上、上段に内包） | 45,056 | 1.817秒 | 40.32 µs |

HANGUP監視は約4.6%。仮に全てなくせても16.7 FPSを約17.5 FPSにする程度の上限で、
今回の主な変更対象にはしない。監視を省略してclient死亡回収を弱める変更も行わない。

sandbox内も加えた別実行では、ioctl種別を分離した:

| 区間 | 回数 | 集計区間 | 累計経過時間 | 平均 |
| --- | ---: | ---: | ---: | ---: |
| WAITのgpud→sandbox RPC | 37,387 | 18.647秒 | 6.3415秒 | 169.62 µs |
| WAITのquery execute本体 | 36,864 | 18.386秒 | 0.2168秒 | 5.88 µs |
| DIRTYFBのgpud→sandbox RPC | 296 | 18.647秒 | 4.2790秒 | 14.456 ms |
| DIRTYFBのquery execute本体 | 292 | 18.386秒 | 4.1610秒 | 14.250 ms |
| EXECBUFFERのquery execute本体 | 2,970 | 18.386秒 | 0.3728秒 | 125.52 µs |

query executeは `kobox_drm_query_execute_service()` を囲んだ値であり、
純粋なGPU実行時間ではない。集計境界と回数が違うため、行同士の厳密な差分は取らない。
それでもWAIT本体と配送経路の規模の違いは明瞭。
DIRTYFBは上流の `drm_atomic_helper_dirtyfb()` が同期commitする経路であり、
約14msをWAITと同じ配送遅延とは扱わない。

nativeログはCPU間で文字が混ざる場合がある。同時周期のRPC/lifecycle詳細行は
正常行が不足し、内訳の集計には不採用とした。上記service/queryの行は完全一致で検証した
正常行だけを使う。どの時間にもスケジュール待ちやhostによる停止が含まれ得る。

- [gpud区間計測](../.artifacts/fishbowl-gpud-cost-short.6Cxiu0/)
- [ioctl別・sandbox実処理計測](../.artifacts/fishbowl-gpud-cost-detail.O0Vij0/)

次はkernel内でしか分離できないIPCのcopyin/enqueue/wake/copyout/handoffを測る。
サブエージェントの事前・実装後監査を受け、`-Dipc-profile=true` のときだけ
CPU別の固定count/cycles配列をexportする診断へ変更した。
通常版では配列・export・計測コードを無効とし、共有ロック・ログ・新ABIは追加しない。
診断版/無効版双方のビルドと、無効版に `ipc_perf_counters` がないことを確認した。
QMPによるlive snapshotはcount/cyclesの同時性を保証しないため、長い区間の近似値として扱う。
send_wakeはrecv_messageとcopyoutを、recv_messageはcopyoutを内包する。
handoff計測はframe切替準備であり、senderが再実行されるまでの時間ではない。

### kernel IPC内訳の結果

30.0479秒の全native IPC集計。gpud専用の値ではない。

| 計測区間 | 回数 | 累計TSC換算秒 | 平均µs |
| --- | ---: | ---: | ---: |
| wait_register | 214,616 | 0.226991 | 1.058 |
| wait_repoll | 214,616 | 0.121613 | 0.567 |
| send_enqueue | 335,981 | 0.174021 | 0.518 |
| send_wake | 335,981 | 0.479533 | 1.427 |
| copyin | 336,814 | 0.232597 | 0.691 |
| copyout | 335,950 | 0.267585 | 0.797 |
| handoff | 327,910 | 0.241655 | 0.737 |
| recv_message | 715,014 | 0.673024 | 0.941 |

換算にはQMPのguest TSC周波数3,686,397,000 Hzを使用した。
recv_messageは空受信等の失敗も含み、enqueue/wakeは成功時のみ。
copyoutには受信ヘッダの読取りも含む。重複区間の単純加算は禁止。

userlandで観測した数十〜百数十µsに比べ、IPCのcopy/enqueue/wake本体は小さい。
したがって「u64単位コピーをまとめる」変更を性能改善の主対象にはしない。
次はIPC処理の外側にあるkernel共通ロック待ち、起床から実行までの遅延を調べる。

この診断版のFPSは16.536520, 17.123539, 16.850599, 16.827388, 16.497586、
中央値16.827388。通常版16.704132と大きな差は見られないが、各1回で無影響を保証はしない。
Gate終了後は元kernelを復元し、boot image内ファイルが保存済み通常版とSHA-256一致する
ことを確認した（`9d32543316fe9ecfe8ce3dcb456fde45f803da39f4f32a3c0ac4116b61cd2d41`）。

- [kernel IPC診断のログ・カウンタ・画像](../.artifacts/fishbowl-kernel-ipc.0xnUrA/)

### 既存SMPカウンタとの併用

さらに `-Dsmp-profile=true` を併用し、30.0358秒の差分を採取した。
共有ロック取得6,108,133回、競合1,498,502回、累計wait 9.699秒、hold 15.512秒。
scheduler lock取得31,097,624回、累計wait 1.409秒。
TLB shootdownは83,145回、送信0.652秒、ack待ち1.554秒。
munmapは83,139回・全成功、累計2.858秒、うちunmap区間2.350秒だった。
waitはCPU間で重複し、各区間も包含関係があるため、壁時間に対する割合として加算しない。

FPSは15.247073, 15.833558, 16.284732, 15.811205, 15.246393、中央値15.811205。
通常版16.704132より低いため、広いSMP計測の介入またはrun間変動を含む結果として扱う。
ただし毎秒約20万回のkernel共通ロック取得は、IPC要求だけでは説明できない規模である。
次はnative syscall別の回数を調べ、大量呼出しの発生元を特定する。

- [SMP＋IPC診断](../.artifacts/fishbowl-kernel-smp-ipc.Htkh4T/)

### native syscallの呼出し回数

SMP計測を外し、IPC診断とCPU別syscall番号カウンタで30.0424秒を測定した。
全nativeプロセス合計5,354,988回、約17.8万回/秒。起動中ではなく、fishbowlの
測定開始・終了snapshotの差分である。回数の割合は時間の割合ではない。

| syscall | 回数/秒 | 全体比 |
| --- | ---: | ---: |
| FUTEX_WAKE | 38,127 | 21.39% |
| FD_POLL | 24,735 | 13.88% |
| CLOCK_GETTIME | 19,396 | 10.88% |
| IPC_RECV | 15,067 | 8.45% |
| PROCESS_SIGNAL_CTL | 11,876 | 6.66% |
| GETTID | 11,515 | 6.46% |
| FUTEX_WAIT | 10,456 | 5.87% |
| FD_CLOSE | 7,874 | 4.42% |
| FD_WAIT_MANY | 6,195 | 3.48% |

THREAD_SET_GS_BASEは約387回/秒・0.22%であり、起動中の累計値から推測した
TLS切替の頻発は定常描画の主要因ではない。次の確認対象はsandbox adapterの
無条件mutex unlock wakeである。LPRのstate mutexは既に競合時だけwakeしている。

- [syscall回数の診断](../.artifacts/fishbowl-kernel-ipc-counts.0N1ZlX/)

## sandbox mutexの無競合wake削減

adapterの `ph_unlock()` は待ち手の有無によらずFUTEX_WAKEを発行していた。
CPU所有権、タイマー、IRQ、VM管理で使われるmutexであり、LPRのstate mutexとは別物。
まず一時カウンタだけを追加し、ゲストTSC 70.434〜97.950秒でunlock約26,199回/秒、
取得失敗約285回/秒を確認した。後者は待ち手の人数ではなく失敗したexchange回数で、
unlockの約1.09%だった。このrunのFPSは17.007381, 17.560694, 17.040565,
15.533774, 14.575924、中央値17.007381。

無競合unlockだけで毎秒約2.6万回のkernel entryが発生しているため、
mutexを0=空き、1=取得済み、2=待ち手がいる可能性あり、の3状態にした。
unlock時の旧値が2のときだけwakeする。低速経路で取得した所有者も状態2を維持し、
残る待ち手へwakeを引き継ぐ。wakeの対象数や通知イベント自体は変えない。
変更はnative adapterのみで、core.so、.ko、kernel、追加のMesa差分はない。
計測用カウンタとログは撤去した。

同じIPC/syscall診断kernelでの別run比較:

| 観測 | 変更前 | 変更後 |
| --- | ---: | ---: |
| FUTEX_WAKE/秒 | 38,127 | 19,037 |
| 全native syscall/秒 | 178,248 | 182,911 |
| FPS中央値 | 15.823965 | 16.872450 |
| QEMU CPU秒 / 約30秒 | 84.16 | 80.30 |

変更前FPSは11.020134, 17.726747, 15.823965, 16.483639, 12.631640、
変更後は16.459454, 16.872450, 17.470279, 17.848628, 16.802507。
run間変動があるため、FPS約6.6%増を確定した改善率とはしない。
FUTEX_WAKEは約半減した一方、進行量も変わるため全syscall/秒は減っていない。
回数21%という初期観測をそのまま速度21%改善に読み替えない。
この変更だけで30 FPSに届くという仮説は否定された。

実装を直接リンクするunitで、無競合時のsyscallゼロ、wait直前のunlock、
6 threadの競合を確認した（ASan/UBSan、10回反復）。変更前は無競合時の検査に失敗する。
既存のIRQ・lifecycle unitも通過。診断kernelは試験後に通常版へ復元しSHA-256一致を確認。

- [unlock発生元計測](../.artifacts/fishbowl-lock-attribution.BUsMEe/)
- [変更後のsyscall診断](../.artifacts/fishbowl-lock-contended.oq8k8n/)

通常kernel・計測hookなしのadapterでもinteraction Gateを通過した。
QEMU側ではGPU command traceを採取した。FPSは15.267059, 18.508774,
15.803709, 18.082622, 18.502806、中央値18.082622。
QEMU CPU時間は30.0409秒中82.27秒。前の通常版中央値16.704132からは増えているが、
各1回の比較であり改善率の確定には使わない。

GPU trace末尾の1秒を除く20秒の完全区間では、19.9410秒・360更新・3,222 submit、
8.95 submit/更新、平均85,397 bytes/更新、資源作成49回。
更新間隔の中央値54.69ms。Linuxの約2 submit/更新との差は残る。
次はこの分割と、多数のWAIT ioctlの配送がクリティカルパスに乗る理由を絞る。
mutexの呼出し削減は確認できたため残すが、30 FPS達成とは扱わない。

同じ生成物で既存Mesa multi-client Gateも通過した。
3 clientのrendererは全て `virgl (D3D12 (Intel(R) Graphics))`、dma-buf転送2回、
native fence転送8回、共有画素検証、SIGKILLとDRM file回収、
生存clientの描画継続・BO保持、第3clientの再openを確認した。
試験のconsole-shell切替はrunnerが終了時にstartxへ復元した。
通常sandbox生成物とディスク内ファイルのSHA-256:
`afa36e361668f50e7d20e9a75c7103d22d0b5568d6745b9854b98e5eaa80ebf9`。

- [通常kernelでの描画測定](../.artifacts/fishbowl-lock-normal.3Bhawz/)
- [共有・死亡回収の回帰](../.artifacts/fishbowl-lock-multi.4kUNO7/)

## IPI切替仮説の確認

監査許可を受けた一時カウンタで、共有vectorに到着したTLB maintenanceと
ユーザースレッド切替の共起を測定した。30.0429秒でring0到着656,376回、
ring3到着89,371回。TLB ackと同時だったring3到着は46,946回、
handoff成功6,526回、うちTLB ackと共起した成功は2,959回だった。
共起だけでは「不要な切替」を意味せず、通常のrunnable wakeとの重複もあり得る。

handoff処理の全CPU累計は0.01467秒、signal stagingは0.02111秒。
この直接費用のためにvectorを分離しても30 FPSへの大幅改善は見込めないため見送った。
maintenance全体は4.19977 CPU秒だが、複数CPUの和・割込みによる介入を含み、
そのままフレームの壁時間から引ける値ではない。一時IPIカウンタは撤去し、
測定差分はログディレクトリへ保存。通常boot kernelへの復元・ハッシュ一致を確認した。

gpudのRECV→WAIT→RECVを有限timeout付きRECV_WAITへ置換する案も、
現在のnative RECV_WAITが0と無期限しか扱えないため、そのままでは使えない。
gpudのtimeoutを捨てる変更は行わない。

- [IPI内訳と計測差分](../.artifacts/fishbowl-ipi-attribution.oA6Dgx/)

## 小さいioctlのVMO転送を省く（2026-09-18）

8 bytesのVIRTGPU_WAITにもLPRからVMOを転送し、gpudで毎回検証・map・unmap・close
していた。既測定のmap検証10.74 µs＋unmap36.44 µs、WAIT約125回/更新から、
直接区間の和は約5.9 ms/更新。ただしunmapは返信後であり、この和は削減可能な
クリティカルパスの保証ではない。

native IPCの4 wordsにmagic・DRM handle・ioctl番号・8 bytes以下の引数を載せる
LPR↔gpud内部形式を追加した。reply capabilityが呼出しを識別し、返信にはstatus、
ioctl番号、出力を載せる。一般のLinux ABI、kernel ABI、gpud↔sandbox protocolは不変。
追加FD・aux・ポインタ展開を伴う要求は従来のページ経路を維持する。
gpudのfile参照・generation・ioctl変換・実DRM呼出し・busy判定は同じ関数を通る。
完了結果のキャッシュやWAITの省略はしていない。

通常kernel・同じMesa/sandbox・QEMU GPU traceありの比較:

| 項目 | 変更前 | inline化後 |
| --- | ---: | ---: |
| 表示FPS中央値 | 18.082622 | 18.279507 |
| QEMU CPU秒 / 約30秒 | 82.27 | 71.59 |
| submit / 表示更新 | 8.950 | 8.557 |
| 更新間隔中央値 | 54.69 ms | 53.96 ms |

inline化後のFPSは16.863914, 18.485997, 18.279507, 18.087294, 18.934937。
各1回の比較なので、1%程度のFPS差は改善の証明としない。
初回の起動準備中にはホスト上で別ビルドも観測した。
GPU traceは両方とも末尾1秒を除く20秒の完全なflush間区間を集計。
変更後は19.9802秒・366更新・3,132 submit・83,767 bytes/更新。

同一の保存済みSMP診断kernelでも確認した（測定後は通常kernelへ復元）。
変更前30.0429秒、変更後30.0528秒の全native処理の差分:

| 項目 | 変更前 | inline化後 |
| --- | ---: | ---: |
| munmap / 秒 | 2,799.4 | 784.0 |
| munmap累計経過秒 | 3.126 | 0.953 |
| TLB shootdown / 秒 | 2,799.5 | 783.9 |
| 共通lock累計wait秒 | 9.799 | 7.674 |
| 共通lock累計hold秒 | 15.664 | 13.852 |

これらは全CPU累計で、包含・重複があるため足し合わせない。
変更後診断版FPSは16.466481, 17.154130, 16.781560, 18.425029, 17.230006。
不要なmappingとTLB負荷の削減は確認できたが、30 FPSを阻む待ちの解消にはなっていない。
unmapの経過時間の多くが他の進行・待ちと重複していた可能性が高い。
この変更はCPU負荷と不要なmappingを減らすものとして残す。

小さい要求の境界・WAIT/NOWAITの既存意味論をtranslation unit（ASan/UBSan）で確認。
通常interactionとMesa multi-client Gateを通過し、dma-buf/fence共有、SIGKILL回収、
生存clientの描画、第3clientの再openも確認した。
通常生成物とディスク内のハッシュは一致:

- gpud: `e041c3816758a149a4344b008446fe3bca7d071b1d67917e3dda7206cd0c3058`
- LPR: `5d35c4739f1150ec12b2af6cc8e6e4e5263605aa21359164013287f2b443d5ba`
- [通常測定](../.artifacts/fishbowl-inline-normal.WlqAhE/)
- [mapping/TLB診断](../.artifacts/fishbowl-inline-smp.YBPClT/)
- [共有・死亡回収Gate](../.artifacts/fishbowl-inline-multi.YbGmnp/)

## 現在のMesaを使ったLinux再比較

Alpine Linux 6.12.94-0-virtの**標準virtio-gpu**と比較した。Linux上のkobox2ではない。
4 vCPU/KVM、2 GiB、Intel D3D12、GTK、640×480、ioeventfd有効、ネットワーク有効。
PachaOS diskをread-only、ext4をro,noloadで読み、変更先はVM内RAM overlayだけ。
Mesaは通常ビルドと同一ハッシュ `917845a0…fcff0`、GTK/Xfceも同じディスクのバイナリ。
新しいOSSソース変更はない。

Button 1個、fishbowl client window 437×133、同じ画面位置で30秒測定した。
Linuxは標準GTK Inspectorでbenchmark=FALSE/count=1を設定し、Inspectorを隠してから
測定した。PachaOSは自動調整の設定を変えていないが、全測定画像がButton 1個だった。
したがって完全に同一の設定とはせず、固定負荷の描画経路比較として扱う。
LinuxのFPSは59.980806, 60.161749, 60.127126, 60.023238, 60.070626。

| 項目 | Linux標準virtio-gpu | PachaOS inline化後 |
| --- | ---: | ---: |
| FPS中央値 | 60.070626 | 18.279507 |
| submit / 表示更新 | 2.004 | 8.557 |
| submit bytes / 更新 | 56,976 | 83,767 |
| QEMU CPU秒 / 約30秒 | 17.75 | 71.59 |
| 定常更新経路 | PAGE_FLIP | DIRTYFB |

LinuxのGPU traceは30.0014秒・1,801完全更新区間。PachaOSの区間は上記20秒。
統計的な改善率の評価ではなく、要求数・表示方式の違いを見つけるための比較。

別の10秒でLinuxのsys_enter_ioctl/sys_exit_ioctl tracepointをXorg PIDに限定して記録。
157,404 events、欠落なし、入口/出口の未対応なし。kernelソースは変更していない。

| ioctl | 回数 | 平均経過時間 |
| --- | ---: | ---: |
| VIRTGPU_WAIT | 75,072 | 約0.85 µs |
| VIRTGPU_EXECBUFFER | 1,200 | 31.98 µs |
| MODE_PAGE_FLIP | 600 | 22.18 µs |

WAITは125.12回/flipで、PachaOSの約125回/更新と同規模。
回数そのものがPachaOSだけの異常という仮説は支持されない。
Linuxのtrace時刻表示は1 µs精度で、0 µsに丸められた標本も含む。
PachaOSの旧診断ではWAITのgpud→sandbox RPCが平均169.62 µsだったが、
時点も測定範囲も異なるため、厳密な倍率にはしない。

PachaOS Xorgにはplane resources取得のNot supportedも出ていたが、
標準Xorgの `drmmode_is_format_supported()` はnum_formats=0を許すため、
このエラーだけでflip不可と結論してはいけない。
より直接的な欠落はカーソルの接続である。gpud translatorとcore側DRM APIに
CURSOR/CURSOR2経路がなく、標準Xorgの `drmmode_set_cursor()` は失敗時に
software cursorへfallbackする。`ms_present_check_flip()` は
`sprites_visible > 0` でflipを拒否する。これはコードから確認した候補であり、
カーソル接続後の再測定で因果と改善量を検証する必要がある。
カーソル非表示を性能目標の達成条件にはしない。

次の実装候補はkobox所有のDRM橋渡し層への共通カーソルAPI追加と、gpudからの接続。
core.so側も変わるため個別許可を依頼し、ユーザーから
「linux-sandbox/kobox内なら許可」を得た。
Linux upstream、Mesa、Xfce、PachaOS kernelを変更する案ではない。

- [Linuxのログ・画像・ioctl trace・集計](../.artifacts/linux-fishbowl-current.UatAuK/)

## ハードウェアカーソルの接続

既存のcanonical CURSOR/CURSOR2をgpudからkoboxのtyped DRM APIへ接続した。
CURSORはLinux本体と同じくhotspot=0としてCURSOR2へ渡す。
移動の負座標、BO更新、handle=0による非表示、hotspotを保持し、
実DRM ioctlのmaster/lease・GEM所有権検査を経由する。
wire schema・Linux upstream・Mesa・Xfce・PachaOS kernelは変更していない。
Linux hostでも使用可能なAPIで、PachaOS専用の表示成功偽装ではない。

gpud GPU query単体テストで変換→prepare→typed dispatch→completion→replyを確認。
legacy移動、CURSOR2表示、非表示、backendのEACCES伝達、不正flags/sizeを含み、
既存translatorテストとともにASan/UBSanで通過した。

初回起動は配布用sandboxが旧版だったため停止・測定対象外とした。
更新後はdisk内gpud・sandbox・core.soのハッシュをビルド成果物と照合した。

- gpud: `922622f7be948b8510c6dfab06e8c28c753ef9a7e21dfc3383164b1ba00efde1`
- sandbox: `95375718ba7d436f4ffccdd776eaf97a1a29122c9f06342006a72c53a1b09838`
- core.so: `db10a0029ced4cf6589dd7599c72997e53be5dd773709b1cd81a98ee31a9078b`

通常kernel・KVM・Intel D3D12でカーソルupdate/moveがvirtio-gpuへ届くことを確認。
カーソルを非表示にはしていない。fishbowl Button=1の5標本は
19.059677 / 20.080897 / 19.223930 / 18.598966 / 18.837206 fps、中央値19.059677。
ただしこの回のQEMU traceはserialと同じ出力先になったため、旧測定との厳密な差分には使わない。
表示更新中のset_scanoutは依然なく、カーソル接続だけではPAGE_FLIP移行も30fpsも未達。
カーソル接続は機能欠落の修正として保持するが、大きな性能改善と主張しない。

- [ハードウェアカーソル確認ログ・画像](../.artifacts/fishbowl-hwcursor-normal.P3bgnu/)

別のcold起動では通常起動直後からxfwm4用の2個目のVirGL contextと
継続的なset_scanoutが現れ、flip経路で動作した。
QEMU traceは従来と同じ別ファイルへ出力し、カーソルupdateだけを追加した。
30秒測定の後に限って、未改変xfwm4を`G_MESSAGES_DEBUG=all LIBGL_DEBUG=verbose`
付きで`--replace`し、その標準ログでもDRI3、Intel VirGL、XPresent選択を確認した。
再起動後の画面は性能標本に含めない。

| 条件 | FPS中央値 | 約30秒のset_scanout | submit / flush |
| --- | ---: | ---: | ---: |
| 直前の通常版 | 18.279507 | 0 | 8.660 |
| カーソル接続、XPresent動作回 | 22.849682 | 717 | 8.884 |
| Linux標準virtio-gpu | 60.070626 | 1,801 | 2.004 |

今回のFPS標本は22.354647 / 23.256664 / 24.269320 / 22.849682 / 21.874316。
測定30.047秒、717 flush、6,370 submit、約84,932 bytes/flush、
flush間隔中央値41.169ms。QEMU CPU消費は約91.89 CPU秒と、
直前通常版の71.59 CPU秒より増えている（終了したthreadの末尾CPU時間は取得不能）。
GPUへの投入回数はLinuxの約2回/更新まで減っておらず、30fps未達。
Linux側はbenchmark=FALSE、PachaOSは自動調整有効のまま全標本Button=1という
測定条件差も従前どおり残り、厳密に同一負荷とは扱わない。

同じ配布バイナリでも先の起動はXorgのcontextしかなく、flipしなかった。
そのconsoleには`Another compositing manager is running`もあるが、
これだけで別compositorの存在や原因を断定しない。
まず起動ごとのDRI3/合成経路の不一致を解明し、その上でLinuxより多いsubmitと
残るWAIT往復のcritical pathを比較する。
途中でSIGTERM終了した`fishbowl-cursor-present.AdoG5j`は測定対象外。

- [XPresent動作回の測定・標準xfwm4診断](../.artifacts/fishbowl-cursor-present.BsClAo/)

## 起動経路差と配送遅延の再確認

console-shellから、通常と同じ`startx /root/.xinitrc`を
`G_MESSAGES_DEBUG=all LIBGL_DEBUG=verbose`だけ付けて起動した。
xfwm4はguest時刻15:55:12.231に`No vsync support in compositor`、
その3.39秒後に`Another compositing manager is running`を出した。
これはDRI3/合成方式を変更した実験ではなく、通常の分岐の診断である。
終了時にconsole-shell profileは通常startxへ復元済み。

未改変xfwm4のコードでは、settings初期化時にproperty-changedを先に接続し、
loadSettingsの終盤でvblank_modeを設定する。その間にuse_compositing通知が来ると
compositorActivateScreenへ進める。初期値VBLANK_OFFで先に合成を始める、という
順序は上のログと整合する。ただし実行時の呼出し順を追跡した証拠ではないため、
上流バグと確定せず、ソース修正・設定固定による回避も行っていない。

fishbowlの自動個数調整についても[上流GTKの実装](https://raw.githubusercontent.com/GNOME/gtk/gtk-3-24/demos/gtk-demo/gtkfishbowl.c)
を確認した。同じcountの再設定は即returnし、標準の調整周期は1秒。
「1個のまま毎フレーム作り直すから遅い」という仮説はこの実装では支持されない。

別の通常起動で、native sandboxのlifecycleだけを一時計測した。
同時に1要求だけを扱う既存状態遷移のrelease/acquire間で時刻を渡し、
16,384要求ごとの累積値を短い行で出力した。計測ソースは診断ELF作成直後に撤去し、
実行後は通常sandboxのハッシュ`95375718…`へ復元した。

TSC 61.676〜99.154秒、98,304要求の6区間での1要求あたり平均の範囲:

| 区間 | 平均の範囲 |
| --- | ---: |
| WORK_READY公開→dispatch開始 | 98.84〜104.79µs |
| service.dispatch本体 | 111.23〜116.29µs |
| dispatch終了→受信担当の再開 | 17.96〜19.19µs |
| service.complete | 21.93〜24.88µs |

全service要求の平均であり、WAIT限定ではない。dispatchにはDIRTYFBの同期待ちも含む。
返信担当への切替を丸ごと消しても、約125要求/更新なら約2.3msが上限。
ここだけを移動する案は主対象にしなかった。

開始側のcpu_switchは、owner_lock保持中に次のnative threadを起こしていた。
次threadがcpu_enterで同じlockを取り直すため、起こした直後に再び寝る余地がある。
引渡し公開・完了・unlockの後にwake permitを発行する順へ一時変更して比較した。
旧threadは既にlogical_cpu=-1、nextはpermit取得後にしかLinuxへ入れず、
間の通知はdomainに残る。PachaOS kernel・Linux upstreamの変更はない。

| 引渡し順序 | FPS中央値 | QEMU CPU秒 / 約30秒 |
| --- | ---: | ---: |
| unlock後にwakeする試行 | 19.423753 | 71.92 |
| 元の順序へ復元した比較 | 19.130021 | 73.39 |

双方とも計測hookなし、同じ別ファイルへのGPU traceあり。set_scanoutは起動時の3回だけで、
測定中は非flip経路だった。試行のFPSは19.708261 / 19.020680 / 19.620401 /
19.423753 / 18.523860、復元後は16.090992 / 19.538964 / 18.317894 /
19.130021 / 19.411526。XPresent動作回の22.85 FPSとは直接比較しない。

中央値差約1.5%はrun内の変動より小さく、この各1回では改善を立証できない。
元の実装でも、起こしたthreadが実行される前にunlockできれば再入眠は発生しない。
またwakeを後ろへ移すと実行開始そのものは遅れるため、開始側の約100µs全体を消せる変更ではない。
どちらが実際に支配的だったかは今回のFPSだけでは確定できないが、
狙った大幅な短縮は観測されず、この順序変更は撤去した。
CPU domain単体試験と双方のQEMU interactionは通過。単体試験は所有権状態遷移の検証であり、
native threadのwake競合そのものの検証とは区別する。

現在のsourceのmachine.c/lifecycle.cには今回の試行・計測差分がなく、
staged sandboxとディスク内sandboxはカーソル接続済み通常版`95375718…`へ復元済み。
Linux upstreamの差分はゼロ。カーソル接続は保持し、30 FPSは引き続き未達。

- [起動時の標準ログ](../.artifacts/fishbowl-startup-diagnostic.i77Abi/)
- [lifecycle配送区間・一時計測差分・通常版復元](../.artifacts/fishbowl-lifecycle-probe.OgfrLH/)
- [引渡し順序の試行](../.artifacts/fishbowl-handoff-order.wJiB4j/)
- [元の順序へ復元した比較](../.artifacts/fishbowl-handoff-baseline.c4fNr3/)

## 待機中CPUへの重複通知（2026-09-18）

native adapterだけの一時計測で、要求公開からdispatchまでを分解した。
TSC 60.398〜98.810秒の98,304要求中、全時刻が順序どおり揃った97,062要求を集計。
34要求は時刻欠落、1,208要求は並行した通知等で時刻順序が一致せず除外した。
従って例外・tailを含む全要求の平均ではない。各値は非重複区間の平均で、CPU占有時間ではない。

| 区間 | µs/要求 |
| --- | ---: |
| 公開→通知側owner_lock取得・domainへ通知登録 | 0.317 |
| 通知登録→native SIGNAL直前（futex wakeを含む） | 6.998 |
| SIGNAL直前→Linux CONTROL IRQ呼出し直前 | 28.106 |
| Linux CONTROL IRQ→処理担当へのwake直前 | 45.774 |
| wake直前→処理担当のcpu_enter開始 | 15.599 |
| cpu_enter開始→service dispatch開始 | 4.383 |

IRQからwakeの区間が最大。Linux scheduler内の時計・timer等のhost呼出しの内訳は未計測で、
この45.774µs全体を時計の費用とは断定しない。計測ソースは診断ELF作成後に撤去済み。

また、cpu_wait中はnative通知からのLinux進入を抑止しているのに、cpu_notifyは
futex wakeとSIGNALを両方発行していた。SIGNALのhandlerはこの場合deferredヒントを置くだけ。
owner_lockで待機を確認できる間だけ、その同じヒントを直接置いてsequenceを進め、
futexで起こす形へ変更した。実行中ownerへの非同期SIGNALは維持する。
busy-loop、時計精度低下、Linux IRQの省略、kernel/ABI変更はない。

実装のwait/notifyとnative mask復元を使う小さいunitで、wake-before-wait、
2通知の合流、native mask/logical IRQ mask保持、実行中ownerへのSIGNALを検証した。
`bash tests/run-kobox2-cpu-wait-unit.sh`（UBSan）通過。

通常版の非flip経路、各1回の比較:

| 条件 | FPS中央値 | QEMU CPU秒 / 約30秒 |
| --- | ---: | ---: |
| 直前基準 | 19.130021 | 73.39 |
| 待機中の重複SIGNAL削減 | 20.725910 | 70.71 |

変更後標本20.439448 / 21.115725 / 20.822891 / 20.725910 / 20.646958。
変動を含むため約8%増を確定した改善率とはせず、CPU負担減との方向の一致を確認した。
通常sandboxとディスク内のSHA-256は`fcba285507523ac4a0aa19474c9c8ad1a1209726661407c77e0786c7b3e6c50e`。

- [配送区間の一時計測・復元ログ](../.artifacts/fishbowl-delivery-stages.Lvg47z/)
- [重複通知削減後の通常版](../.artifacts/fishbowl-idle-notify.7aE2uw/)

同じバイナリ・ディスクのままVMを終了して新しく起動すると、起動直後から
2個目のVirGL contextと連続set_scanoutが現れた。xfwm4の手動replaceはしていない。
このXPresent/flip回の標本は27.117488 / 27.588381 / 28.646318 / 25.903236 /
24.839916、中央値27.117488 FPS。30.028秒中94.64 CPU秒。30 FPSは未達。
変更前のXPresent回22.849682 FPSとも差があるが、各1回なので再現性の保証はしない。

rootfsの通常manifestには/rootのxfwm4設定がなく、ext4同期はファイルシステムを再生成する。
一方、初回起動後のディスクにはvblank_mode=auto、use_compositing=trueが保存されていた。
設定を保持したcold再起動と、同期直後の初回起動は異なる条件である。
前述の初期化順序の仮説と整合するが、コールスタック追跡による原因確定はまだしていない。
初回起動の非flipを、再起動のflipの性能値で隠さない。Xfceの実装・設定は変更していない。

同じ変更で既存Mesa multi-client Gateも通過。3 clientともIntel D3D12のVirGL、
dma-buf転送2回、native fence転送8回、共有画素検証、SIGKILL/HANGUPによる
backend-close=1、生存clientの描画・BO保持、第3client再openを確認した。
試験後はrunnerがconsole-shellから通常startx profileへ復元した。

- [設定を保持したcold再起動・XPresent回](../.artifacts/fishbowl-idle-notify-reboot.89rlMw/)
- [共有・client死亡・再open回帰](../.artifacts/fishbowl-idle-notify-multi.oN4d44/)

## 初回ログインのXPresent選択

Xfconf 4.20.0の[xfconf_cache_set](https://raw.githubusercontent.com/xfce-mirror/xfconf/xfconf-4.20.0/xfconf/xfconf-cache.c)
は初回値の保存時にも、return前にproperty-changedを同期発行する。
[channel側](https://raw.githubusercontent.com/xfce-mirror/xfconf/xfconf-4.20.0/xfconf/xfconf-channel.c)もその場で通知を転送する。
Xfwm4 4.20.0はinitSettingsで先に通知を接続し、loadXfconfDataで未登録の値を書き込む。
use_compositingの通知はその場で合成を開始するが、vblank_modeの通知は無視され、
設定反映はloadSettingsの末尾。このため初回の合成開始が初期値VBLANK_OFFを使う経路がある。
単なるDBus配送速度の推測より強い、同期呼出し順序のコード上の根拠が得られた。

PachaOSの[システム既定値](../userland/fixtures/linux/xfwm4-defaults.xml)に
use_compositing=trueとvblank_mode=autoを明記し、packから
`/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml`へ配置する。
双方とも配布版`/usr/share/xfwm4/defaults`と同値。ユーザー設定ファイルには配置せず、
プロパティのlockも付けない。XPresent強制、合成OFF、遅延起動、xfwm4再起動は行わない。
これは初期設定による回避であり、上流の初期化実装自体を修正したものではない。

診断起動では、ユーザーのxfwm4.xmlが存在しないことをshellで確認してから、
通常のstartx/.xinitrcを標準debugログだけ付けて実行。
初回からIntel VirGLのDRI3と`Compositor using XPresent for vsync`を確認した。
Xfwm4のSHA-256は従来の配布版`ab98285cb6ba031e80d6989615c99486a683d540a7582107857ab877bd54e063`のまま。
時計区間の診断ELFは作成のみで未導入。ソースの診断差分も撤去し、sandboxは`fcba2855…`のまま。

診断起動はGate通過。XPresent選択ログは1回で、No-vsync/別compositor警告はともに0回。
その後、runnerの通常profile復元によるrootfs再生成を経て、debugfsでもユーザーの
xfwm4.xmlが存在しないことを確認し、debug環境変数なしの通常自動起動を実行した。
これも起動直後から追加VirGL contextと継続flipを確認し、interaction Gate通過。
ゲスト内Xfwm4/sandboxのSHA-256は上記と一致した。

| 初回ログイン | FPS中央値 | 全5標本 |
| --- | ---: | --- |
| 既定値なし・非flip（直前） | 20.725910 | 前節参照 |
| 既定値あり・標準debugログ | 25.738937 | 31.311015 / 25.592887 / 26.436493 / 24.674908 / 25.738937 |
| 既定値あり・通常自動起動 | 25.172431 | 25.085425 / 24.617419 / 25.240506 / 25.492274 / 25.172431 |

通常版は30.039秒中94.53 CPU秒。初回の経路不一致は解消したが、30 FPSは未達。
診断中の単発31.31 FPSだけを達成根拠にはしない。設定済み再起動の27.12 FPSとも
条件を混ぜず、以後はこの初回からXPresentが動く通常構成を基準にする。

- [設定空を確認した標準ログ診断](../.artifacts/fishbowl-first-login-defaults.txnShQ/)
- [設定空・通常自動起動の確認](../.artifacts/fishbowl-first-login-normal.60Aq5k/)

## 時計・タイマーsyscallの削減余地

初回からXPresentが動く同じ構成で、native adapterのCLOCK_GETTIMEと
TIMERFD_SETTIMEの直前・直後だけをTSC計測した。全sandbox threadを集計し、
16,384要求ごとに累積値を出力。Linux upstream、Mesa、Xfce、kernelは変更していない。
計測用ソースはELF作成直後に撤去し、試験後は通常sandboxへ復元した。

最初の2つのIRQ→service対応付けプローブは、callback終了後のidleからの
scheduleを取り逃す問題と、対象の大半が除外される偏りがあった。
これらの採用サンプルだけからIRQ内訳や改善率を推定しない。
代わりに全呼出しの合計を測り、この処理自体の直接削減余地を確認した。

描画中のTSC 75.406〜93.776秒、65,536要求、18.3703秒の累積差分:

| sandbox全体 | 回数 | 回数/要求 | 平均/呼出し | 合計時間 | 区間時間比 |
| --- | ---: | ---: | ---: | ---: | ---: |
| CLOCK_GETTIME | 298,233 | 4.551 | 3.972µs | 1.18469秒 | 6.449% |
| TIMERFD_SETTIME | 9,046 | 0.138 | 6.213µs | 0.05620秒 | 0.306% |

所要時間にはsyscall中の待機・割込み・descheduleと測定費用を含む。
並行thread間の合計であり、CPU使用率やIRQ→service区間の内訳ではない。
TSC周波数は当該QEMUの取得値3,686,397,000Hzを使用。

通常版25.172431 FPSの約39.73ms/フレームにこの比率を当てはめると、
全費用をゼロにして全てがcritical pathから消えるという楽観的な直接効果でも
約2.68ms/フレーム、約27.0 FPS相当。30 FPSまでの約6.39ms短縮には届かない。
別実行への一次近似であり、間接的なbatching変化まで制限する厳密な上限ではない。
timespecを2回に分けて検証・コピーする実装も確認したが、それはさらに費用の一部。
時計だけを主対象にしたkernel/ABI変更は、この結果では正当化しない。
また、以前の非flip条件で測ったIRQ→wake 45.774µsを現在の内訳として流用しない。

interaction Gateは通過。今回は計測用バイナリなので通常版のFPS改善とは扱わない。
復元後のstaged/disk内sandboxはともに`fcba285507523ac4a0aa19474c9c8ad1a1209726661407c77e0786c7b3e6c50e`。
lifecycle.cは差分なし、machine.cには実際のowner_waiting改善のみが残る。
Linux upstream差分ゼロとCPU wait unit通過も確認した。

- [全呼出しの計測・ソース差分・復元ログ](../.artifacts/fishbowl-clock-total.VHM5Qt/)
- [不採用: callbackまでの対応付け](../.artifacts/fishbowl-irq-cost-matched.wyC2tT/)
- [不採用: idle後までの対応付け](../.artifacts/fishbowl-irq-cost-idle.V2WcNn/)

## XPresent条件での要求別の往復

通常版fcba2855…を基に、一時的なnative lifecycle計測を行った。
WORK_READY/WORK_DONEのrelease/acquireで渡す同一要求の4区間を分離し、
DRM queryのcommand IDでWAIT・EXECBUFFER・PAGE_FLIP・その他を分類。
同時にCONTROL通知時のdomain ownerが処理担当か、cpu_wait中かを数えた。
一時計測はnative側のみで、計測ELF作成後にソース差分を撤去した。

TSC 76.815〜95.213秒、18.3982秒、65,536要求。時刻順序の不正は0件。

| 要求 | 件数 | 公開→dispatch | dispatch本体 | 終了→受信担当再開 | complete |
| --- | ---: | ---: | ---: | ---: | ---: |
| WAIT | 57,724 | 80.931µs | 6.216µs | 21.175µs | 26.896µs |
| EXECBUFFER | 3,881 | 85.490µs | 118.157µs | 26.287µs | 41.256µs |
| PAGE_FLIP | 473 | 74.167µs | 56.866µs | 19.760µs | 63.907µs |
| その他・DRM event | 3,458 | 100.085µs | 56.358µs | 27.742µs | 44.084µs |

WAITの内部DRM処理より、処理担当への引渡しが大きい。
Linux標準virtio-gpuのWAIT全syscall平均約0.85µsとは境界が違うため、
上の6.216µsをそのまま同範囲の比較値にはしない。

同区間の通知先は、処理担当がownerのもの11件、別ownerのcpu_wait中61,986件、
別ownerの非wait中またはownerなし3,539件。処理担当がまだ実行中だから
通知を省ける、という候補は対象が0.017%に過ぎず採用しない。
次に確認するのは、cpu_switchなどが待機者なしでも発行するowner_sequenceの
FUTEX_WAKE。新しい待機者との競合はowner_lockとsequenceの再検査で扱う必要がある。

interaction Gate通過、通常sandboxへの復元とハッシュ一致を確認。

- [要求別計測・通知先分類・復元ログ](../.artifacts/fishbowl-present-path.lNn1PK/)

## 空のowner待ち行列へのwake省略: 不採用

owner_lock下でcpu_enterの待機者数を数え、既存のowner_waitingと合わせて
wake_cpu_waitersの呼出しを分類した。測定時点ではwakeを省略していない。
TSC 74.855〜93.186秒、18.3306秒、65,536要求の差分:

| owner待機者 | wake回数 | 回数/要求 | 平均/回 | 合計経過時間 |
| --- | ---: | ---: | ---: | ---: |
| なし | 152,801 | 2.332 | 3.485µs | 0.53252秒 |
| あり | 132,224 | 2.018 | 6.647µs | 0.87888秒 |

空のwakeは8.126µs/要求。約125要求/更新なら約1.02msの直接費用に相当する。
それだけでは30 FPSに届かないが、小さい変更で除去できるため通常版で比較した。
候補はsequence更新を維持し、登録済み待機者がいる場合のみFUTEX_WAKEを発行する。
登録と判定をowner_lock下で行い、登録後・futex待機前のraceとcpu_enterの再検査を
追加unitで確認した。CPU waitと既存lifecycle unit、通常interaction Gateは通過。

| 通常バイナリ | FPS中央値 | QEMU CPU秒 / 約30秒 |
| --- | ---: | ---: |
| 以前の基準 | 25.172431 | 94.53 |
| 空wake省略 | 25.552388 | 94.43 |
| 直後の変更前バイナリ再測定 | 26.099196 | 94.33 |

候補の5標本は25.455610 / 25.552388 / 26.041618 / 25.117965 / 27.092037。
直後の変更前は25.280295 / 21.996773 / 27.851662 / 26.865987 / 26.099196。
単発の実行間で揺れがあり、FPSにもCPU時間にも利益を確認できなかった。
全CPU合計のwake経過時間には、既に次threadを起こした後の処理や待ちも含まれる。
その全量が描画の直列待ちから消えるとは限らず、約1msという小さい見積もりも
今回の変動幅を下回る。厳密に効果ゼロと証明したのではないが、今回の目標に対して
利益を確認できない分岐・待機者管理を残さず、候補と専用test差分を撤去した。

以前から残しているowner_waitingによる重複SIGNAL削減は維持。
通常sandboxはfcba2855…へ戻し、初回XPresentのシステム既定値も維持する。

- [空wakeの費用計測](../.artifacts/fishbowl-owner-wake-cost.jMnOQ1/)
- [空wake省略の通常版](../.artifacts/fishbowl-owner-wake-skip.114BQ6/)
- [直後の変更前比較と最終復元](../.artifacts/fishbowl-owner-wake-control.lff31z/)

## GTKフレーム全体のLinux / PachaOS比較

細かいIPC一件の費用から離れ、同じアプリのフレーム時計で大区間を比較した。
既存GTK/Mesa/Xfceは変更せず、外部の`GTK_MODULES`計測器
`tests/gtk_frame_observer.c`から公開frame-clock signalを観測する。
両方とも標準fishbowlのButton、`benchmark=false`、`count=1`。
後者はGTK Inspectorと同じ既存property設定であり、描画・スケジューラの改変ではない。
4 vCPU / 2 GiB / KVM / GTK+GL / Intel D3D12 / network有効。
LinuxはAlpine 6.12.94の標準DRMで、Linux版kobox2との比較ではない。
Linux VMには同じPacha rootfsをread-onlyで接続し、書き込みはRAM overlayだけに置く。

公開signalのbefore-paint→update→layout→paint→after-paint開始→終了→次before-paintを記録。
30秒間は固定メモリへ記録し、終了後にまとめて出力する。欠落は両方0。
表は同じ完全フレーム集合の平均経過時間で、区間は重複せず合計が全体になる。
GPU完了・画面提示時刻や各processのCPU時間を測った表ではない。

| 平均 ms / アプリframe | Linux（1799区間） | PachaOS（786区間） | 差 |
| --- | ---: | ---: | ---: |
| before-paint→update | 0.014 | 0.031 | +0.017 |
| update | 0.021 | 0.035 | +0.014 |
| layout | 0.024 | 0.047 | +0.024 |
| paint（通信・下流待ちを含む経過時間） | 0.760 | 19.979 | +19.220 |
| after-paint | 0.004 | 0.020 | +0.017 |
| after-paint終了→次before-paint | 15.845 | 18.032 | +2.188 |
| 全体 | 16.667 | 38.145 | +21.478 |

平均間隔の逆数は60.00対26.22 FPS。paint区間の差が全体差の約89.5%を占める。
Linuxは2回測定し、初回paint平均0.768 ms、再測定0.760 msで再現した。
表は再測定値。両OSのXorgログで`virgl (D3D12 (Intel(R) Graphics))`を確認済み。
PachaOSのpaint中央値21.833 ms、p95 31.404 ms、全体p95 51.409 ms。
更新・レイアウトの合計は約0.08 msで、ここを削っても目標には届かない。
次フレームまでの区間は、タイマー、イベント処理、下流処理などを含む未分解の区間であり、
全量をGPU待ちやsleepの無駄とは扱わない。

次に比較すべきなのはpaint内のCPU処理とX11送受信・応答待ち、および
その待ちの下流にあるXorg / gpud / sandboxの対応区間。
この結果だけからGTK自身の計算が遅い、またはGPUが約20 msかかるとは断定できない。
他区間不変という単純見積もりでは30 FPSまで約4.81 ms/frameの削減が必要。
paint全体の約24%、Linuxとの差の約25%であり、ここを切り分ける利益は
以前の空wake省略（全体約2.9%が上限）より大きい。
実際には起床・合成周期も変わるため、4.81 ms削減だけで30 FPSになる保証ではない。

- [Linux再測定・集計・Intel VirGL確認ログ](../.artifacts/fishbowl-frame-linux.ib8Z0a/)
- [Linux初回測定・集計](../.artifacts/fishbowl-frame-linux.HmyPQS/)
- [PachaOS生データ・集計・Intel VirGLのXorgログ](../.artifacts/fishbowl-frame-pacha.0XZfnj/)

計測器の準備で失敗したN4wxrF / oKxb1o / c9uaFbの各回は上表に含めない。
特に停止直後のdirtyなext4へdebugfsで直接追加する方法では、次回のjournal replayと
衝突し、計測器のパスにXorgログの内容が見えた。通常のrootfs同期で配置し直して解消した。
計測器のhost-musl由来の`NEEDED libc.so`も避け、読み込むAlpineアプリの公開symbolを使う。
Linux libcやnative libc、既存OSSの実装を変更したわけではない。
計測後はstage内の計測器を退避して通常rootfsを再同期し、guestからの不在を確認した。
sandboxのstage / disk両方は従来のfcba2855…のまま。計測器・解析コードはtestsに残し、
productionの処理変更、README変更、commit、pushは行っていない。

### paint内のlibc I/O待ち

同じ計測器を`LD_PRELOAD`でも読み込み、paint signalの実行threadでのみ
poll / writev / read / write / recv / recvmsg / sendmsg / pthread_cond_waitを観測した。
`tests/gtk_frame_io.c`は元のlibc関数を呼び、戻り値とerrnoを維持する。
再入呼び出しは重複加算しない。記録出力は測定窓終了後のみ。
GTK/Mesa/Xfce、Linux libc、kernel、gpudのproduction実装は変更していない。

| 平均 ms / frame | Linux（1799区間） | PachaOS（775区間） |
| --- | ---: | ---: |
| paint内poll | 0.0029 | 28.7105 |
| paint内writev | 0.0230 | 0.0767 |
| paint内recvmsg | 0.0031 | 0.0025 |
| paint内・上記I/O以外 | 0.6865 | 1.2125 |
| paint全体 | 0.7155 | 30.0023 |
| frame全体 | 16.6672 | 38.6948 |

paint内の残余も純粋なCPU時間とは限らず、未観測の待ちは含み得る。
記録欠落は双方0。PachaOSのpaint配分は前回19.98→30.00 ms、frame間は18.03→8.56 msに
動いたが、全体は38.15→38.69 msで近い。区間配分を固定費用とは扱わない。
Linuxは引き続き約60 FPSで、今回もXorgログにIntel D3D12 VirGLを確認した。

poll / writevの回数はLinux約1.03 / 1.02回、PachaOS約3.82 / 3.77回/frame。
PachaOSでpollの直後1 ms以内にwritevが来た2922回のpoll合計は約21.42秒であり、
送信先の消費を待つ経路が有力。上流[libxcb 1.17.0](https://xorg.freedesktop.org/archive/individual/lib/libxcb-1.17.0.tar.xz)
の`_xcb_conn_wait`も、送信時にPOLLIN|POLLOUTを待ち、POLLOUT後にwriteする構造。
ただし今回events/revents自体は記録していないので、各pollの理由を確定したとはしない。

送信総量は30秒でLinux約29.45 MB、PachaOS約26.19 MB。
frame当たり概算は16.37 KB対33.79 KBだが、総量が多いわけではない。
FPSが異なると移動描画の更新領域も変わり得るため、回数差の全量を不要呼び出しとはしない。
次はPOLLIN/POLLOUT、共有ringの空き・消費進捗とwake時刻を対応付け、
Xorgの消費が遅いのか、消費後のLPR待機解除が遅いのかを分ける。
バッファ拡大だけで待ちを先送りする変更は、この結果からは正当化しない。

- [PachaOS paint I/O生データ・集計](../.artifacts/fishbowl-paint-io-pacha.oBep8z/)
- [Linux paint I/O生データ・集計](../.artifacts/fishbowl-paint-io-linux.73CUx3/)

計測器はstageから退避し、通常rootfsを再同期した。30 FPS到達はまだ未確認。

### 送信可能待ち：消費前と消費後の分離

`fishbowl-unix-flow.DZTp7S`ではpollのevents/reventsを追加し、主な待ちが
POLLIN|POLLOUT要求→POLLOUT復帰であることを確認した。一方、native側の複数processが
同時にserialへdumpした記録は文字単位で混線し、相関集計には不採用とした。
kernelのloggerは変更せず、再測定ではprocessごとのファイルに保存した。

`fishbowl-unix-flow-file.KpRGBg`ではLPRの計測用ビルドで起動後80〜100秒だけ記録し、
115秒以降に`/root/lpr-flow-<PID>.bin`へ保存・fsyncした。
共有ringのwire layout・通知方式・pollの判定は変更していない。
GTKの公開signalとlibc I/Oの計測も併用し、画面上はButton 1個、Intel D3D12 VirGL。

送信側PID 94（アプリのREADYログと一致）の4234行と受信側PID 44の8470行は、
footerの件数・未確定件数から欠落0を確認した。sender identity 509 / generationを照合し、
送信可能の閾値64 bytesを下回ってblockし、復帰時には送信可能だった1267回を集計した。
復帰時も送信不可だった27回は別枠。対象外PID 68は上限16384行に対し45496件発生しており、
欠落があるため採用していない。全processの完全なトレースと主張するものではない。

| native block 1回あたり | 平均 ms | 中央値 ms | p95 ms |
| --- | ---: | ---: | ---: |
| block→復帰 | 12.9158 | 12.2201 | 19.2488 |
| block→受信側の消費確定観測 | 12.9011 | 12.2077 | 19.2356 |
| 消費確定観測→復帰 | 0.0147 | 0.0095 | 0.0216 |

合計16.364秒のうち消費後は18.60 ms、約0.114%。主な待ちは受信側がデータを読む前にある。
消費時刻はcommit直後の観測値であり、正確な命令実行時刻ではない。
このnative表はpaint外の送信待ちも含む20秒間の集計なので、30秒間のGTK frame表へ
加算したり、そのまま1 frameの費用と解釈したりしない。
LPRの通知後起床を主因として最適化する根拠は得られなかった。
次はXorgが次のreadへ進むまでの処理、特にDRM呼び出し・gpud / sandbox側との待ちを追う。

同時取得したアプリframeは平均36.066 ms（約27.73 FPS）、paint 23.509 ms、
うちPOLLOUTのみで復帰したpollは21.202 ms/frame。
以前の約26 FPSとは実行時期も変わっており、productionの高速化を適用した結果ではない。
30 FPS達成の証拠にはしない。

- [相関元のprocess別ファイル・集計・画面・Xorgログ](../.artifacts/fishbowl-unix-flow-file.KpRGBg/)
- [計測用LPR・通常版退避・配置／復元ログ](../.artifacts/lpr-flow-trace/)

終了時にLPRの一時計測hookを撤去し、元のLPRバイナリ5d35c473…へ戻した。
既存のsocket read集約変更など、今回より前の差分は維持する。

### アプリとXorgを含む大区間比較

細かな要求単価ではなく、完全なframe / PAGE_FLIP要求間隔を分母にした。
Linuxは今回改めて固定Button 1個でアプリframeとXorg syscallを同時採取した。
Linux標準DRMとPachaOSのgpud / kobox2経路の比較であり、Linux版kobox2ではない。
両方4 vCPU / 2 GiB / KVM / Intel D3D12 VirGL / GTK表示 / network有効。
Linuxは同じディスクをread-only + RAM overlayで使い、OSSソースは変更していない。

LinuxのXorg ftraceは156,214 eventsで欠落・入口出口未対応0。
PachaOSのLPR診断ではXorg PID 44が46,785行、アプリPID 94が3,229行で、
両方footer件数一致・未確定0。DRMは起動後80〜90秒、Unix read/writeは80〜100秒。
GTK記録も同じmonotonic時計の約10秒窓に絞り、完全なframeだけを集計した。
Linuxは約45.302〜55.297秒。両OSのGTK記録欠落も0。

| アプリframeの平均経過時間 ms | Linux（598区間） | PachaOS（251区間） |
| --- | ---: | ---: |
| update / layout / paint前後 | 0.057 | 0.145 |
| paint中のpoll | 0.003 | 21.946 |
| paintの残り（送受信を含む） | 0.698 | 1.128 |
| after-paint終了→次frame開始 | 15.909 | 16.400 |
| 合計 | 16.667 | 39.618 |

PachaOSのpollのうち20.832 ms/frameはPOLLIN|POLLOUT要求→POLLOUT復帰。
前節の消費前・消費後比較と合わせると、Xorgが読み進むまでの送信可能待ちが主候補。
残余やframe間を純粋なCPU時間・GPU時間・不要sleepと解釈しない。

Xorg側は同じthreadの連続PAGE_FLIP要求開始を境界にし、ioctl経過時間を
その区間にclipして重複なく加算した。成功通知・実画面提示の時刻ではない。

| Xorgの1更新要求間隔あたり ms | Linux（599区間） | PachaOS（252区間） |
| --- | ---: | ---: |
| BO状態確認（VIRTGPU_WAIT） | 0.072 | 22.366 |
| 描画投入（EXECBUFFER） | 7.995 | 4.766 |
| KMS（sequence / addfb / flip / rmfb） | 0.033 | 0.920 |
| resource create / map / close | 0.074 | 0.218 |
| 計測したioctlの外（未分解） | 8.493 | 11.325 |
| 合計 | 16.667 | 39.596 |
| 更新要求頻度 / 秒 | 60.00 | 25.26 |

アプリ表とXorg表は並行するprocessを別々に見たもので、両表の時間を足してはいけない。
Xorgのioctl外には通常処理・他のsyscall・スケジュール待ちなどが含まれる。
PachaOSのhookは通常DRM ioctl経路を対象とし、PRIMEなどの早期return経路は対象外。
GPU実行・fence完了・実画面提示の内訳は今回未計測。

PachaOSのBO状態確認は間隔の56.49%、描画投入は12.04%。
WAIT回数はLinux 124.12 / PachaOS 125.13回/更新と近い。
PachaOSの全31,584回はNOWAITであり、5.649秒はGPU完了をblockingで待った時間ではなく、
LPR→gpud→sandboxの往復・処理・スケジュール待ちを含む経過時間。
成功は31,082、EBUSYは502回。別ioctlを挟まず同じBOの成功確認を繰り返した例は0件。
他clientも状態を変え得るため、成功結果の単純キャッシュを正当化するデータではない。

同じアプリ接続のXorg read完了→次read開始754区間、合計9,932.83 msのうち、
同一threadのDRMと重なる部分は7,121.58 ms（71.70%）。これはアプリ表に加算しない。
EXECBUFFERはLinux 2.01 / PachaOS 8.48回/更新。LPR入口ですでに回数差がある。
ただし回数差の全量を無駄とせず、Xorgの処理単位・送受信・flushの発生条件を追う。

今回のLinux EXECBUFFERは1,205回中594回が1 ms超、p95 8.730 ms。
以前のLinux計測の累計約0.064 ms/更新とは大きく異なるが、更新頻度は今回も60 Hz。
待ち時間の配分が変わり得るので古い単価へ置き換えず今回の値を採用した。
この表だけでPachaOSのGPUやsubmit処理自体が速いと結論しない。

現在の39.596 msから30 Hzの33.333 msへは約6.26 msの削減が必要。
他の配分が不変と仮定すれば、状態確認22.366 msの約28%に相当する。
これは調査対象の規模を見積もる値で、実際のFPS改善を保証しない。
診断用hookの費用も含むため、採用する高速化は最後に通常版で検証する。

解析は `tests/compare-xorg-frame-cost.py`。threadを跨ぐ加算・重複spanを拒否し、
入力の欠落を検査する。GTK解析には時刻窓指定を追加した。
Linux runnerは測定後にftraceをgzip回収する。kjpUJnは非圧縮回収timeout、
ENrSe4は端末のコマンド折返しを完了markerと誤認し、DRMデータが不完全なため不採用。
markerは入力echoと一致しない形に修正した。
PachaOSの初回xUvBk6も一律5 MiB/processの計測bufferでメモリ不足となり不採用。
有効な回は必要なprocessだけ40 KiB単位で遅延確保し、通常処理を変えていない。

- [Linux同時計測・比較表の元データ](../.artifacts/fishbowl-pipeline-linux.oQ8zYs/)
- [PachaOS同時計測・DRM/read相関・画像](../.artifacts/fishbowl-lpr-drm-flow.fIzyaL/)
- [通常版復元ログ](../.artifacts/lpr-drm-flow-trace/restore.log)

計測後にLPRの一時hookを撤去し、通常LPR 5d35c473…へ復元した。
stage / diskのハッシュ一致と、guest内の計測moduleの撤去を確認済み。
既存inline RPC・socket read集約・sandbox fcba2855…は維持。
今回は性能変更を採用せず、kernel・Linux upstream・Mesa・Xfceの実装変更も行っていない。

## 処理担当の短時間ポーリングを採用、通常版30 FPSは引き続き確認が必要

前節までの要求別計測ではWAIT本体約6.2 µsに対し、公開→dispatchは約80.9 µs。
125回/更新ならこの受け渡し区間は約10.1 msに相当する。全量が消える保証はないが、
約6 msの短縮が必要な目標に対して試す規模があるため、短時間待受を比較した。

変更は許可済みの `kobox2/linux-sandbox/kobox/boot/lifecycle.c` のみ、20行追加。
通常のLinux APIを使い、host固有のsyscall・PachaOS名・新しいABIは追加しない。

- service dispatch後だけ、最大150 µsの間pending状態をポーリングする。
- 再スケジュール要求があればポーリングを終え、dispatch後にはcond_reschedを実行する。
- IRQを無効化せず、空なら既存のcompletion待機へ戻る。初回待機は従来どおり。
- pollingで処理した通知はtry_wait_for_completionで消費する。completionをresetせず、
  並行する通知を消さない。停止・エラー・WORK_READYの状態判定を維持する。
- BOの状態確認、generation / 権限検査、受信担当のprepare / complete / releaseは省略しない。

同じGTK計測module、固定Button 1個、各30秒の直前A→B→B比較:

| 項目 | 変更前 | 候補1回目 | 候補2回目 |
| --- | ---: | ---: | ---: |
| 完全frame区間 | 800 | 949 | 934 |
| 平均frame間隔 ms | 37.493 | 31.596 | 32.112 |
| 平均間隔の逆数 FPS | 26.672 | 31.650 | 31.141 |
| paint平均 ms | 16.784 | 3.006 | 3.283 |
| frame間平均 ms | 20.570 | 28.419 | 28.654 |
| 全体p95 ms | 53.335 | 59.332 | 61.016 |
| 約30秒のQEMU CPU秒 | 92.65 | 87.87 | 85.29 |

平均速度の改善は約17〜19%、CPU時間も約5〜8%減った。単なるCPU消費との交換ではない。
ただしpaintの短縮全量を全体改善とはしない。p95は改善しておらず、
常時滑らかになった・すべてのframeが33.3 ms以内になったとは主張しない。
この回では公開→dispatch区間自体は再計測していないため、その短縮量は未確定。

moduleを撤去した通常起動でも起動・hover・Applications・端末・fishbowl描画を確認。
画面上の5標本は26.720770 / 36.574438 / 28.623360 / 28.307361 / 28.218866 FPS、
中央値28.307361。いずれもButton 1個だったが、通常版ではbenchmarkの自動調整を
無効にしていない。5標本の中央値と上の30秒間全frame平均を同一指標として扱わない。
通常版のQEMU CPU時間は91.12秒。**30 FPSの完了扱いはしない。**
同条件比較で確認できた改善は残し、次はpaint外のフレーム開始待ち・Present完了を追う。

検証:

- 既存native lifecycle unit: publication / duplicate notifications / STOP / failure cleanup通過。
- Intel D3D12 / KVM / 4 vCPU / network有効、GTK + ramfbで各回を実行。
- 実Mesa multi-client Gate: dma-buf転送2回、native fence転送8回、双方の全画素検証、
  SIGKILL、backend-close=1、生存clientの共有BO保持と追加描画、第3client再openが通過。
- Linuxホストでは別出力先に同じソースの`--with-gates` coreをビルドし、既存`--all`を実行。
  SMP / memory / VFS / shmem、待機350件、RCU56件、workqueue211件、cleanup52件が警告0で通過。
  通常coreには試験用exportがないため、この基盤試験は通常coreそのものの実行ではない。

通常coreをdb10a002…から4a3eb3cf…へ更新し、stage / diskの一致を確認。
LPRは5d35c473…、sandboxはfcba2855…のまま。計測moduleはguest / stageから撤去済み。
Linux upstream・Mesa・Xfce・PachaOS kernelの編集、新しいfixture、README変更、commit/pushはない。

- [変更前の同条件計測](../.artifacts/fishbowl-owner-poll-control.Hq991D/)
- [候補1回目](../.artifacts/fishbowl-owner-poll-trial.ZEVkFW/)
- [候補2回目](../.artifacts/fishbowl-owner-poll-repeat.WYEW6P/)
- [計測器なしの通常起動](../.artifacts/fishbowl-owner-poll-normal.cMcWa8/)
- [共有・死亡回収・再open](../.artifacts/fishbowl-owner-poll-multi.cW5KhA/)
- [候補差分・ビルド・Linux基盤試験・同期ログ](../.artifacts/fishbowl-owner-poll-build.zQGbpD/)

### フレーム全体の再比較と、採用しなかったscheduler候補

同じButton 1個、KVM / 4 CPU / Intel D3D12で再比較した。
GTK thread全体のI/Oへ観測を広げ、GLibが呼ぶppollも含めた。
Linuxは標準DRM（Linux版kobox2ではない）。LinuxのI/O bufferは30秒全体では
overflowしたため、欠落前の4秒・239 frameだけをI/O内訳に使用する。
PachaOSは30秒・898 frameで欠落0。Linux Xorgの10秒ftraceは別途欠落0。

| アプリframe平均 ms | Linux（完全な4秒窓） | PachaOS（30秒） |
| --- | ---: | ---: |
| paint | 0.699 | 4.668 |
| frame間 | 15.910 | 28.544 |
| 全体 | 16.665 | 33.406 |

PachaOSのframe間28.544 msのうち、ppollは22.592 ms、通常pollは5.714 ms。
タイムアウト復帰だけでは要求13.471 msに対し実際24.479 ms、超過11.007 msだった。
Linuxの欠落前prefixでは要求14.572 ms、実際14.702 ms、超過0.131 ms。
これはCPU／GPU実行時間ではなく、呼び出し全体の経過時間である。

コード監査で、短いIPC処理がtimer slice前にblock/handoffするとCPU使用量が
計上されないことを確認し、実経過時間を計上する候補を実装した。しかしFPSは
29.934→28.220、ppoll超過は11.007→11.073 msで改善しなかった。
paintは4.668→10.626 msに増加。配分は変わるが今回の遅れは解消せず、候補を撤去した。

次に、kernel実行中に届くwakeの再schedule要求をuser-returnまで保持する候補を試した。
通常IPCを含めて適用すると22.376 FPSに悪化。ppoll超過は7.670 msへ減ったものの、
paintは25.537 ms（うちpoll24.099 ms）へ増えた。通常IPCまで即時切替を増やす適用は
不採用とし、通常IPCの動作を元に戻した。超過の削減だけをFPS改善と扱わない。

kernel変更はsubagentの範囲監査・許可後にrootが実装。各候補のunit/buildは通過。
Linux upstream / Mesa / Xfce / ABI / quantumの変更はない。

- [PachaOS基準](../.artifacts/fishbowl-broad-pacha.xux2C3/)
- [Linux比較・overflowの元データも保存](../.artifacts/fishbowl-broad-linux.Tblw2C/)
- [実時間計上候補・不採用](../.artifacts/fishbowl-runtime-trial.gjpPdr/)
- [通常IPCを含むwake候補・不採用](../.artifacts/fishbowl-wake-trial.UR109q/)

タイムアウトwakeだけに限定した候補も、30秒全体では28.293 FPSだった。
この回は115秒時点の診断dumpが窓内に入ったため厳密比較から除外した。
dump前のprefixでも29.295 FPSで改善を立証できず、この候補も撤去した。
scheduler_connection.zig / traps.zigは今回の差分ゼロ、syscalls.zigでは今回追加した
handoffだけを削除し、既存の計測差分は保持した。boot kernelも基準9d325433…へ復元。

## LPRのpoll再試行で相対timeoutを繰り返す不具合を修正

コード上、native waitのNOT_READYには期限復帰とシグナル中断の両方が含まれる。
LPRの高精度期限より少し早い復帰はRESTART_SYSCALLとなり、外側dispatcherが
poll/ppollを元の引数で呼び直していた。そのため相対timeoutが最初から再設定される。
kernelのtick期限とLPRのns期限の差が、もう一度の全時間待ちに拡大する経路だった。

lpr_socket.cのpollループに9行を追加し、graphのwatch/pin解放後にシグナルを
処理し、配送がなければ同じ絶対期限で再走査するようにした。ppollの一時maskを
維持したまま配送し、実シグナルのEINTRを握りつぶさない。kernel/ABIの追加変更はない。

診断用DRM hook・sandboxの障害ログhookを外し、通常版同士でも比較した。
順序は修正後1→修正前→修正後2、各cold boot、固定Button 1、4 CPU / 2 GiB、
KVM、Intel D3D12 VirGL、GTK表示、network有効。GTK observerだけ同条件で使用した。
先の29.934 FPSはDRM診断hook付きなので、通常版への改善率の基準には使わない。

| 項目 | 通常版・修正前 | 修正後1 | 修正後2 |
| --- | ---: | ---: | ---: |
| 完全frame区間数 / 30秒 | 934 | 977 | 996 |
| 平均frame間隔 ms | 32.073 | 30.706 | 30.117 |
| 平均間隔の逆数 FPS | 31.179 | 32.567 | 33.204 |
| paint ms/frame | 3.892 | 11.409 | 9.877 |
| frame間 ms/frame | 28.013 | 19.070 | 20.029 |
| timeout復帰ppoll数 | 880 | 697 | 778 |
| ppoll要求時間の平均 ms | 13.463 | 10.841 | 11.988 |
| 同じ呼出しの実時間 ms | 23.925 | 11.953 | 13.030 |
| 平均超過 ms | 10.462 | 1.112 | 1.042 |

待ち超過は約90%減り、FPSはこの比較で約4.5〜6.5%増えた。
paint側の送信可能待ちが増えるため、削ったtimeout超過全量をframe短縮とはしない。
Linuxの比較prefixは約60 FPS・ppoll超過0.131 ms。PachaOSの60 FPSは未達。
この修正は期限の正しさと実測改善の両方で保持する。両修正版ともframe/IO欠落ゼロ、
QEMU interaction通過。実画面の変化回数をアプリFPSとして数えてはいない。

既存async signal fixtureへppollの期限下限、一時maskでのEINTR、mask復元、
ready FDの無期限pollを追加した。LinuxとPachaOSで通過し、PachaOSは12ms指定で
平均12.587ms。既存async signal全体、pending-signal-frame / unix-wait単体試験も通過。
最初のQEMU試験はstageに旧fixtureが残って新marker欠落で失敗し、fixtureをstageへ
反映した再実行で全markerを確認した。失敗した回を合格には数えていない。
複数clientのMesa GateもIntel D3D12 VirGLで通過。dma-buf/native fence FD転送、
SIGKILL、backend-close、生存clientの追加描画、第3clientの再open・全画素検証を確認した。

次の対象はBO状態確認の往復。Linux/PachaOSとも約125回/更新で、前回の比較では
合計0.074 / 14.491ms。NOWAIT成功の単純キャッシュは他clientとの整合性がなく採らない。
adapterのrun_preparedはWORK_READYを公開するたびcpu_notifyし、処理担当が
短時間polling中でもnative signal→Linux IRQを通す。不要な通知の抑制は候補だが、
就寝前のarm/recheckと競合検証が必要で、この時点では未実装。

- [通常版基準](../.artifacts/fishbowl-poll-baseline.5KDuag/)
- [修正後1](../.artifacts/fishbowl-poll-trial.yjWger/)
- [修正後2・導入LPR照合](../.artifacts/fishbowl-poll-repeat.kdu9MH/)
- [build・signal回帰](../.artifacts/fishbowl-poll-deadline/)

導入LPRは`3470bfec9b579626b1aff5aa9fbc9ef44ca83a49bcba4ca881e1cee7a6862615`。
sandboxは通常版fcba2855…、coreは既存の短時間polling版4a3eb3cf…を維持した。
Linux upstream / Mesa / Xfceの追加改変、README変更、commit/pushはしていない。

## polling中の通知抑制は不採用

coreの短時間polling中だけadapterのWORK_READY通知を省く候補を実装した。
就寝前に通知をarmしてpendingを再確認し、cond_resched中は通知を有効にする。
GCC/Clangの既存lifecycle競合試験（ASan/UBSanを含む）は通過したが、
通常版とのfishbowl比較ではFPSが悪化した。

| 項目 | 通常版・poll修正後2 | 通知抑制候補 |
| --- | ---: | ---: |
| FPS | 33.204 | 30.548 |
| frame間隔 ms | 30.117 | 32.735 |
| paint ms/frame | 9.877 | 14.068 |
| frame間 ms/frame | 20.029 | 18.498 |
| paintとframe間のpoll合計 ms/frame | 17.894 | 23.962 |
| host QEMU CPU時間 / 30秒窓 | 95.18秒 | 81.40秒 |

CPU消費減を描画の高速化とは扱わない。pollingフラグはownerが実際にCPU上で
実行中である保証ではなく、通知省略で応答を遅らせる余地がある。ただしこの
スケジュール上の説明は推測で、悪化の内訳を証明したものではない。
実測の悪化と通知契約の複雑化から不採用とし、callback・adapter・追加unitを撤去した。
既存の短時間pollingとLPRの期限修正は残した。復元ソースからcoreを再buildし、
元の4a3eb3cf…とbyte一致、sandboxもfcba2855…へ戻して通常rootfsへ反映した。

Linux core Gateはproduction coreを渡したため、test専用の
kobox_linux_tls_probe / kobox_linux_boot_verifyがなく6件失敗（ELF loadの1件は通過）。
これをruntime回帰またはGate合格とは扱わない。候補撤去後にtest専用coreでの
再実行はしていない。PachaOSの測定Gateは通過し、frame/IO欠落はゼロだった。

- [候補の測定](../.artifacts/fishbowl-notify-trial.wAzB2W/)
- [候補diff・単体試験・復元buildとsync](../.artifacts/fishbowl-notify-arm/)

## IPCのFD容量走査と、返信前のスレッド切替を削減

コード監査でipcRecvがFDなし返信でもfdFreeCountFromを呼び、FD表全体の空きを
数えていた。初期256／上限4096スロットのうちmin_fd=16以降を毎回走査する。
受信preflightだけをprivate helperへ置換し、必要0件なら走査せず、必要N件なら
N個見つけ次第終了する。不正min_fdの検証は省かず、容量不足ではqueueを消費しない。
fdFreeCountFrom自体、導入・参照管理・generation・権限・rollbackは変更していない。
FD表はkernel所有でuserlandでは除去できないため、subagentの範囲監査・許可後に
rootが実装した。既存kernel unitへ満杯時の0FD受信・不正min_fd・不足後の再受信を
加え、181/181件通過。kernel ABIの変更はない。

さらにadapterは、DRM処理後に受信threadを起こしてから返信を公開していた。
dispatch成功時に同じopening Linux taskでcompleteを呼び、返信公開を先に行う。
prepare/next/release/stopは受信threadに残し、WORK_DONEまで共有stagingを渡さない。
completeの失敗は、受信側がrelease/stopした後のterminalからcoreへ伝える。
IRQ文脈でioctlを実行したり、DRM fileのowner制約・通知・FD検証を省いたりしない。
callbackの配置以外にwire/core ABI・Linux upstream・Mesa・Xfceの変更はない。

旧構成のWAIT完了→受信thread再開は21.175µs/件で、125件/更新なら約2.6msの規模。
これは現行構成の測定値ではない。実測の短縮全量をこの値から説明しない。

| 構成 | 完全frame数 / 30秒 | FPS | frame間隔 ms | paint ms | frame間 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 直前基準・poll修正後2 | 996 | 33.204 | 30.117 | 9.877 | 20.029 |
| FD容量走査のみ修正 | 1021 | 34.039 | 29.378 | 8.821 | 20.326 |
| 上記＋返信直接公開 | 1047 | 34.926 | 28.632 | 7.742 | 20.691 |
| 返信直接公開・terminal順序補正後の再測定 | 1069 | 35.659 | 28.044 | 7.308 | 20.543 |

同じ固定Button 1、Intel D3D12 VirGL、KVM、4CPU/2GiB、network有効。
FD容量のみは1回、返信直接公開はterminal順序補正を挟んで2回であり、
差の全量を修正効果とは断定しない。
frame/IO欠落ゼロ、interaction Gate通過。直接公開版のXorgログは
`glamor X acceleration enabled on virgl (D3D12 (Intel(R) Graphics))`を確認した。
最初の直接公開測定後、complete失敗時のterminal順序を従来と揃えたので、
この測定ELFのhashは3d0c6a64…、後続buildとは区別する。

GCC/Clang＋ASan/UBSanでlifecycleのthread所有・重複通知・STOP・失敗cleanupを確認。
query/lifecycleの既存unit、既存Mesa multi-client Gateも通過した。
後者は直接公開の最初のbuildで実施し、dma-buf/native fence共有、SIGKILL、
backend-close、生存client描画、第3clientの再openと全画素検証を確認した。
その後のterminal順序の補正は既存failure unitでも確認した。

コピー処理のbulk化は見送った。旧全native IPC計測ではcopyin+copyoutのTSC換算
累計が30.048秒中0.500秒で、主対象として小さい。約1.66%は旧構成の区間規模の
比較であり、並列性や現行頻度を含む厳密なFPS改善上限ではない。

- [FD容量修正のunit・build](../.artifacts/fishbowl-ipc-capacity/)
- [FD容量修正の測定](../.artifacts/fishbowl-capacity-trial.QR0wGw/)
- [返信直接公開の測定](../.artifacts/fishbowl-complete-trial.ov0nce/)
- [返信直接公開のbuild・unit・Mesa回帰](../.artifacts/fishbowl-direct-complete/)
- [最終版の再測定・guest導入照合](../.artifacts/fishbowl-complete-repeat.fdTM5W/)

最終版の再測定もframe欠落ゼロ、KVM有効・Intel D3D12 VirGLを確認した。
後続の監査で、この回のI/Oログは宣言12683件に対して454件で切れていたと判明した。
frame表は全件と期間末尾まで記録済みでFPS値は維持するが、この回のI/O内訳は棄却する。
guestから読み出したsandboxはstageとSHA-256一致:
`c4e63bebfb78fd7bce2c73fcd68ccfb941d4181116ec0407e830e2d82501d209`。
kernelは`9b399ba512b52c7e47d9b437a42f74fe8d7f2ae23fac8c64e93c2e296bcde789`。
core 4a3eb3cf…とLPR 3470bfec…は変更していない。実装後のkernel監査でも、
事前承認した最小範囲内・受信意味の保存を確認した。2変更は保持する。
Linux標準DRMの約60 FPSに対してまだ約28ms/frameあり、60 FPSは未達。
次は残る要求受け渡しの経路に集中し、過去の80.9µsを現行値として流用しない。

## 現行経路の再分解と、時計syscall削減候補の撤去

前節の通常版（sandbox c4e63beb…、core 4a3eb3cf…、kernel 9b399ba5…）に
native側だけの一時プローブを付けた。WORK_READY/WORK_DONEで同一要求の時刻を
同期し、TSC 3000億〜3900億の範囲を集計、4260億以降に一度だけ出力した。
プローブのsourceは診断ELF生成直後に撤去した。通常版のsourceやABIには残さない。

TSC周波数3,686,399,000 Hz、区間約24.414秒、時刻順序不正0件。

| 平均µs / 要求 | 件数 | 公開→DRM開始 | DRM処理 | 返信公開 | 公開完了→受信担当再開 |
| --- | ---: | ---: | ---: | ---: | ---: |
| WAIT | 101890 | 16.361 | 6.037 | 20.520 | 17.848 |
| EXECBUFFER | 5002 | 86.416 | 135.210 | 62.230 | 53.848 |
| PAGE_FLIP | 818 | 29.551 | 85.597 | 163.615 | 55.970 |
| その他・event | 3606 | 80.742 | 42.707 | 67.851 | 38.585 |

返信公開にはnative sendから戻るまでのdeschedule等も含む。peerの実際の受信時刻や
GPU実行時間ではない。WAITのcpu_notify呼出しは平均9.364µs、通知returnから受信担当
再開までは51.402µsで、表の区間と重複するため加算しない。
診断版は33.79 FPSであり、通常版34.93〜35.66 FPSとの性能比較基準には混ぜない。

コード上、150µsの短時間pollはpending確認ごとにktime_get_mono_fast_nsを呼んでいた。
そのclocksourceはtask/port.cのhost_clock_readからhost monotonic_nsへ進み、
PachaOSではCLOCK_GETTIME syscallになる。Linux hostはclock_gettime経由。
待受ループ内の期限読み取りだけを16回に1回にまとめ、pending/停止/need_reschedは
毎回確認するようにした。期限超過は最大15回の追加非blocking probeのぶん増え得るが、
Linux時計・timer・fenceの精度は変えず、IRQ有効、cond_reschedも維持する。
変更は許可済みkobox/boot/lifecycle.cのみ。PachaOS kernel/ABI変更はない。

直接の公開→DRM開始は125回/更新とすれば約2.0msの規模であり、単独で60 FPSへ
届く見積もりではない。一方、頻繁な時計syscallが他threadの実行へ与える影響は
この区間のみでは上限を置けない。事前の旧CLOCK_GETTIME 6.45%測定は短時間poll
導入前のため、現行pollの費用として流用しない。

| 通常版 | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒 / 約30秒 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 直前基準・返信直接公開 | 35.659 | 28.044 | 7.308 | 20.543 | 98.18 |
| 時計確認を16回に1回・Iconだったため比較除外 | 43.735 | 22.865 | 5.576 | 17.146 | 100.20 |
| 同候補・Buttonで再測定 | 35.171 | 28.432 | 7.327 | 20.916 | 98.83 |

Intel D3D12 VirGL、KVM、4CPU/2GiB、network有効。初回は同じButtonだと誤認して
約22.6%改善と報告したが、画像を確認すると次へボタンのクリックが効かずIconだった。
この改善報告は撤回する。基準35.659と再測定35.171はいずれも画像上Buttonであり、
同一負荷では改善を再現できなかった。frame/IOは候補2回とも宣言件数と実件数が一致。

時計確認頻度を下げても、要求ごとの通知・スレッド受渡し・返信回数は変わらない。
既存短時間pollによりWAIT公開→開始はすでに16.36µsで、その一部だけを削る候補だった。
時計syscallの現行削減量は直接測っていないため、無駄が全くないとは断定しないが、
描画を速くする効果が得られず期限確認を粗くする分岐だけを残す理由はない。
候補7行を撤去し、再buildしたcoreは基準4a3eb3cf…とbyte一致。既存150µs poll、
直接返信、FD容量修正は保持した。診断sandboxも通常c4e63beb…へ戻した。

Linuxでも同じsourceから--with-gatesのtest専用coreを別に生成し、既存Gateの
ELF load、TLS、正規boot/package転送、resource stop/stale-stop/disconnect/early-stop
を再実行、7/7件通過。production coreへtest probeは追加していない。
これはLinux標準DRMの約60 FPS測定とは別で、Linux hostでkobox coreが動く回帰試験。

ログ欠落の再発防止として、既存observerのFRAME_OBSERVER_DONEまでrunnerが待つ
ようにし、analyzerはio_count宣言と実際の行数の不一致を拒否するようにした。
この出力待ち修正自体にguest wrapperやobserverの.so変更はない。完全な候補ログの解析成功と、
前節の454/12683件ログの拒否を確認した。近接する他7回のログは件数一致だった。
さらに既存GTK observerへ、測定開始時に実際のGtkFishbowlの子が1個のGtkButtonで
あることを確認するopt-in検証を追加した。Linux/PachaOS両runnerでこの検証を要求し、
クリックを送っただけでButtonになったと扱わない。既存OSSの実装には変更しない。
前節の正常版・FD容量版・直接返信版の保存画像はButtonであることも確認した。

- [現行経路の診断結果](../.artifacts/fishbowl-path-current.fsHa2I/)
- [診断ELF・生成diff・通常sandbox保存](../.artifacts/fishbowl-current-path/)
- [時計確認削減のbuild・Linux Gate](../.artifacts/fishbowl-clock-batch/)
- [通常版候補の初回測定](../.artifacts/fishbowl-clock-batch-trial.Dh3TQS/)
- [Buttonでの再測定・候補不採用](../.artifacts/fishbowl-clock-batch-repeat.u2NbKs/)

## Button負荷の実行時確認と、資源枯渇の切り分け

通常core・sandboxへ復元した後の`fishbowl-workload-verified.ostM36`は、
測定開始前に`TableFull`、DMA失敗、sandbox終了とgeneration再起動が発生した。
この回は性能比較から除外する。`failed-or-zero`のログの出元は
`_kobox/src/linux_personality/linux_dma.c`であり、新adapterの
`userland/kobox2_adapter/device_dma.c`ではない。隣接する汎用`mem: refused`だけで
gpudのDMA回収漏れと断定してはいけない。

kernel監査では、native DMA capability作成時の候補は共通object表4096個と
呼出元FD表に絞られ、install失敗のobject/IOVA rollbackに明白な漏れは見つからなかった。
ただし旧koboxのエラーをこの経路へ直接結びつける根拠にはならない。
事前承認を受けてrootが割当失敗時だけの内訳ログを追加し、2回実行したが枯渇は再現せず、
満杯になった表・保有者は未確定。容量拡張はしていない。
診断版はbuildとkernel unit 181/181件を通過。最終監査で新規staticカウンタの
明示的mapping範囲確認が必要と指摘されたため、診断は通常版へ残さず撤去した。
保存した診断diff/ELFは、この配置確認なしに再利用しない。

1回目の診断runはButtonを目視できたが、observerが子を取得できず中断した。
GtkFishbowlは描画対象をinternal childrenとして公開するため、`get_children`でなく
公開APIの`gtk_container_forall`で列挙するようobserverだけを修正した。
[GTKの実装](https://raw.githubusercontent.com/GNOME/gtk/3.24.49/demos/gtk-demo/gtkfishbowl.c)
の`gtk_fishbowl_forall`でこの区別を確認した。GTK/Mesa/Xfceの実装には変更していない。

修正後の`fishbowl-button-verified.mujnvv`はQEMU Gate通過。
`FRAME_OBSERVER_CONTENT type=GtkButton valid=1`とDONEの両方を確認した。

| 確認項目 | 結果 |
| --- | ---: |
| 完全frame数 / FPS | 1041 / 34.724 |
| 平均frame / paint / frame間 | 28.799 / 8.459 / 20.149 ms |
| paint中のsocket writable待ち | 2.750回・6.969 ms / frame |
| QEMU CPU秒 / 約30秒 | 98.09 |
| I/O宣言 / 取得 | 12474 / 12474、drop 0 |

これは失敗時診断だけを加えたkernelでの確認であり、新たな高速化ではない。
core `4a3eb3cf…`、sandbox `c4e63beb…`、Intel D3D12 VirGL、KVM・4CPU・network有効。
Xorgのglamor renderer、DRI3、Present初期化を確認した。

コード上の次の検討対象は、`lifecycle.c`の要求ごとのreceiver→Linux task→receiverの
所有権受渡し。現在はreceiverがprepare/next/release、Linux taskがdispatch/completeを
行う。準備・解放を移すなら、QUIESCEの優先確認、失敗時cleanup、IPCの単独所有、
stagingを返信公開前に再利用しない条件をまとめて保存する必要がある。
前節の公開→開始16.361µsと返信→receiver再開17.848µsを125回/更新へ換算すると
それぞれ約2.05ms・2.23msの規模だが、後者はpeer側処理と重なるため足して
FPS改善量とすることはできない。通知削除だけの旧候補は既に不採用なので繰り返さない。
今回のpaint 8.459msのうち6.969msはsocket writable待ちであり、GTK描画計算を
削る根拠ではない。Xorg側の消費・DRM往復と同じ待ちを二重加算しないこと。

- [測定前の資源枯渇・診断build/unit・復元](../.artifacts/fishbowl-workload-verified.ostM36/)
- [診断1回目・負荷確認の不備により棄却](../.artifacts/fishbowl-exhaustion-diagnostic.YwUqbM/)
- [Button確認済み・全ログ取得](../.artifacts/fishbowl-button-verified.mujnvv/)

## 連続要求の所有権を保持し、receiver往復を削減

前節のコード分析に基づき、最初の要求だけreceiverが準備し、以後はopening Linux taskが
準備→dispatch→complete→releaseを続けるようにした。既存150µs pollの期限または
need_reschedでidleへ戻るとき、receiverへ所有権を返す。待受期間は延長していない。
これにより連続要求ごとのWORK_DONE wake・receiver再開・再通知が不要になる。
coreの`lifecycle`へtask-context限定の任意poll/idle callbackペアを追加した。
IRQ用pendingは従来どおりatomic state参照のみで、IPCやstaging処理は行わない。
追加フィールドはprocess-local host契約であり、wire/PachaOS syscall ABI変更はない。
coreとsandboxは同時に再build・配備し、Linux hostの既存callback無し経路も維持した。

停止はringより先に毎回IPCを確認する。完了公開とreleaseが済む前にstagingを再利用しない。
エラー時はstagingを解放してreceiverへ戻し、stop完了後にterminalを公開する。
途中QUIESCEが残りのring要求に優先すること、2件目をreceiverに戻さず準備すること、
継続中のprepare失敗も既存lifecycle unitへ追加した。GCC/Clang＋ASan/UBSanで通過。
GPU query unit、LinuxのELF/TLS/正規boot/package/STOP関連7 Gateも通過した。
実Mesa multi-client Gateは相互dma-buf/fence転送、SIGKILL回収、生存client描画、
第3client再openまで通過した。

| PachaOS・Button 1個 | FPS | frame ms | paint ms | frame間 ms | CPU秒 / 約30秒 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 直前基準 | 34.724 | 28.799 | 8.459 | 20.149 | 98.09 |
| 所有権保持・初回完走 | 39.474 | 25.333 | 5.739 | 19.425 | 71.59 |
| 同版・再測定 | 39.609 | 25.246 | 5.401 | 19.674 | 70.74 |

以前の基準35.659 FPSに対しても改善している。直前基準比で約14% FPS増、
平均frameは約3.55ms短縮、CPU秒は約28%減。Button class/countを実行時確認済みで、
frame欠落0、I/Oも初回13800/13800・再測定13780/13780件、DONEまで取得した。
CPU秒の窓はrunner側約30秒で、GTK frame記録と厳密に同一の開始時刻ではない。
paint中のsocket writable待ちは6.969→3.939ms/frameへ減った。
これはGTK本体を変更せず、下流の要求消費を速くした結果と整合する。
旧時刻プローブの区間値はこの版の値として流用しない。

最初の候補run `fishbowl-owned-burst-trial.UfbM4x`は、測定開始前に基準版でも見た
TableFullで失敗した。creates=2461、closes=806であり、前回の2445/804に近い。
性能値には採用していない。診断を再追加し、前節で指摘されたstatic範囲も修正した。
変更は事前・事後にkernel監査の承認を受けrootが実装。共通object表満杯とprocess FD表満杯の
失敗時内訳を各最大4回出すだけで、容量・ABI・成功時スキャンは変更しない。
kernel unit 181/181通過。2回の完走では枯渇が出ず、原因確定まではこの診断を保持する。

完走2回は同じ通常版core/sandboxと失敗時診断付きkernelを使用した。
Intel D3D12 VirGL、KVM・4CPU・2GiB・network有効、DRI3/Present初期化を確認。
Linux upstream、Mesa、Xfceのsource変更はない。この高速化は保持するが60 FPSは未達。

- core: `a8bcf9b8701254b73a21b60ce88e34fbb884b406c3cb79ad9ef40df63d8f2b9a`
- sandbox: `c52db440c4271aaa6f435e56bbb97838cf8003f9a7d176b6756953d24e90d4b4`
- kernel: `b4c97bdc42ea8501c8e3dec931c4d9e99129d307dfa812b0401574417d29f1eb`
- LPR: 前節と同じ`3470bfec…`
- [build・unit・Linux Gate・Mesa multi・診断差分](../.artifacts/fishbowl-owned-burst/)
- [初回候補・資源枯渇による棄却](../.artifacts/fishbowl-owned-burst-trial.UfbM4x/)
- [初回完走](../.artifacts/fishbowl-owned-burst-diagnostic.RLHY9e/)
- [同版再測定](../.artifacts/fishbowl-owned-burst-repeat.TkY8HN/)

### 同時期のLinux標準DRM比較

`fishbowl-owned-burst-linux.LtscGq`でLinux標準virtio-gpuも測り直した。
これはLinux kernel内DRMであり、Linux上のkobox sandbox測定ではない。
共有diskはread-onlyとRAM overlayで使い、同じ配備済みMesa/Xfce/GTK/observerを使用。
KVM、4CPU/2GiB、Intel D3D12 VirGL、Button 1個を実行時確認した。

| 約30秒・同じButton負荷 | 完全frame数 | FPS | frame ms | paint ms | frame間 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Linux標準DRM | 1799 | 60.000 | 16.667 | 0.690 | 15.912 |
| PachaOS・所有権保持 | 1188 | 39.609 | 25.246 | 5.401 | 19.674 |

Linuxのframe記録は欠落0。I/O記録は32768件で容量に達し122966件を記録できなかったため、
全30秒のI/O内訳は使わない。frameだけの全期間解析と、I/Oが全件残るguest 44〜48秒の
239 frameのprefix解析を分けた。prefix内のpaintのwritable待ちは1.017回、
0.00136ms/frameであり、PachaOSの2.597回、3.939ms/frameとの差が残る。
この比較は4秒prefix対30秒のため厳密な同期間差分ではない。
合計約8.58ms/frameの差を、そのままGPU実行時間の差とは扱わない。
Linuxのframe間約15.9msは主に60Hz pacingを含み、PachaOSの追加約3.76msも
個別の待機原因が未確定。次は残るXorgの消費とLPR→gpud→sandboxの往復を調べる。

- [Linux標準DRMの全frame・完全prefix I/O・renderer検証](../.artifacts/fishbowl-owned-burst-linux.LtscGq/)

## socket容量候補の棄却と、共有object表の枯渇修正

通信のコードと既存記録を照合した。STREAMは64KiBの固定ringで、部分read中は
record全体を消費するまでその領域をwriterへ戻さない。既存39.61 FPS記録のGTK送信は
約47.8KB/frame、1回最大16628 bytes。容量による早いbackpressureを疑い、256KiBを試した。
ただし大きなキューは下流の処理能力自体を上げず、接続ごとの最大容量も双方向で384KiB増える。
定数を共有するdatagramの上限も変わるため、効果なしに残す変更ではない。

候補の最初の2回は従来構成でも見ていたTableFullで描画停止し、FPS比較から除外した。
失敗時診断により、満杯なのは全process共通のKernelObject表4096枠と確定した。
初回はDMA mapping1816、VMO1294、channel595などで、reply作成も失敗していた。
監査の事前承認後、rootが初回失敗時のみのowner/参照集計を一時追加した。
再現時はDMA1795、VMO1312で、すべて有効FDから到達し、zero-ref/no-FDはいずれも0。
owner8のDMA1662に対し、その時点のQEMU生存GPU資源は1657個だった。
VMOは各processへ分散しており、close済みDMA viewのmapping残存では説明できない。
mmapだけが保持するNativeVmoRefと、FDのKernelObject表は別物である。
これは全アプリの寿命管理に漏れがない証明ではないが、単純な未解放kernel slotとは異なる。

監査の承認を得て、共通表だけ8192へ拡張した。processごとのFD上限4096とABIは不変。
既存のIRQ全表走査も倍になる副作用があり、容量だけを性能改善とは扱わない。
BSS増分は448KiB。最初の8192版は追加診断の配列値コピーでboot時にstackを越えたため停止した。
逆アセンブルでcreateKernelObjectの予約量0x1b0d98を確認し、診断3箇所をslice/pointer走査へ
直したところ0xd98へ縮小した。差分は72 bytes×8192枠×3配列であり、stackは拡張していない。
owner詳細診断は役目を終えたので撤去し、既存kind別失敗時ログのslice修正だけ残した。

同じ8192枠kernel、Intel D3D12 VirGL、KVM・4CPU/2GiB・network有効、Button 1個を
各60秒測った結果は以下。双方とも負荷の実行時確認、frame/I/O欠落0、DONEを確認した。

| 通信ring | FPS | frame ms | paint ms | frame間 ms | paint送信待ち ms | paint返信待ち ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 64KiB | 38.787 | 25.782 | 6.175 | 19.409 | 4.699 | 0.295 |
| 256KiB候補 | 39.583 | 25.264 | 7.159 | 17.897 | 3.814 | 1.864 |

送信待ちはPOLLIN/POLLOUT要求でPOLLOUTが返ったcall、返信待ちはPOLLINのみのcall。
すべてpaintの内数であり、加算してframe費用にしない。QEMU CPU秒/約30秒は71.98→73.59。
各1回の約2%差は改善の再現性を確立していない。4倍の容量でも送信待ちが残り、返信待ちが増えた。
大幅な改善を得られず、メモリ増加とdatagram上限変更を正当化できないため候補は撤去した。
下流の消費・同期処理を速くせず、キューだけ広げる方針は採らない。
64KiB/LPR3470bfec…/unixd07b8e5ea…へsource・配備とも復元した。

60秒の完走2回では枯渇なし。QEMU生存資源は64KiBでpeak1633→終了時1134、
256KiBでpeak1679→1133と減少しており、この窓では単調増加ではなかった。
共通表の拡張は描画停止への対処として保持し、60 FPS達成や新たな高速化成功とは扱わない。
最終kernelはowner詳細診断撤去後に再buildし、181/181 unit通過。
SHA-256: `a9e73c8de4e8a31b58bbcdcea722154ad355e047830c3dddd5bb038540efe50e`。
最終配備のMesa multi-client Gateも通過。Intel D3D12 VirGLの独立client間で
dma-buf/native fence共有、SIGKILL、backend-close=1、生存client描画、第3client再openと
全画素検証を確認した。Linux upstreamの差分はなく、既存kobox/配下の変更のみである。

- [候補build・回帰・診断diff](../.artifacts/fishbowl-socket-capacity/)
- [最初の枯渇](../.artifacts/fishbowl-socket-capacity-trial.hIZNit/)
- [保有者診断による再現](../.artifacts/fishbowl-object-owner.d5jJhc/)
- [診断の配列コピーによるboot失敗・除外](../.artifacts/fishbowl-object-capacity.QAtipG/)
- [8192枠・64KiB基準](../.artifacts/fishbowl-object-capacity-fixed.G4VKM6/)
- [8192枠・256KiB候補、棄却](../.artifacts/fishbowl-socket-capacity-final.YxstIH/)

## DRM要求ごとの死亡監視を一括pollへ変更

`serve_generation`は各要求の前に`gpud_drm_service_reap_hangups`を呼び、
従来はclient/watchとmapping/PRIME leaseの各FDを別々のsyscallでpollしていた。
既存のwait-source列挙を使い、通常のHANGUPなしの場合だけ一括pollで戻るようにした。
監視頻度は落とさない。readinessまたはエラーがあれば従来の個別確認・回収を行う。
回収によって他watchも消えるため、回収後に一括pollの古い結果を使い回さない。
変更はgpudの16行追加のみ。kernel、wire ABI、core、既存OSSは変更していない。

直前の2候補は不採用にした。eventFDの空読み削減は39.898 FPS・CPU 72.38秒/30秒、
連続処理中のring arm省略は39.077 FPS・CPU 72.16秒/30秒だった。
前節の64KiB基準38.787 FPS・71.98秒と比べ明確なCPU削減はなく、
要求/返信そのものの往復も減らない。追加状態・callback引数・専用試験を撤去した。
これは効果が完全にゼロという証明ではなく、維持する性能上の根拠が不足した判断。
この比較の前節kernelには失敗時owner診断があり、厳密な同一build比較ではない。

死亡監視一括版の初回60秒測定は、Xorgの`GL_OUT_OF_MEMORY in glMapBufferRange`
とNULL書込みfault、その後のX session再起動によりobserver出力が完了せず失敗した。
object-table-fullログはなく、原因と今回の変更との関係は未確定。FPS比較から除外する。
その後、同じkernel・core・sandboxで変更前→変更後を各30秒測定した。

| 構成 | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒/約30秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 個別poll・再測定 | 1124 | 37.492 | 26.673 | 15.145 | 11.389 | 73.01 |
| 一括poll | 1248 | 41.643 | 24.014 | 4.970 | 18.867 | 74.25 |

固定Button 1、Intel D3D12 VirGL、KVM、4CPU/2GiB、network有効。
frame/IO欠落ゼロ、IO宣言/取得は14055/14055と14416/14416。
約11.1%のFPS差だが一組の比較であり、安定した改善率とは断定しない。
CPU絶対消費は減っていない。描画数増加と呼出し集約の根拠から変更を保持する。
Linux標準DRMの前節約60 FPSには未達であり、初回停止の原因も未解明のままである。

既存Mesa multi-client Gateでdma-buf/native fence共有、SIGKILL、backend-close、
生存clientの共有BO保持と追加描画、第3clientの再open・画素検証を確認した。
既存DRM file参照管理unitもGCC/Clang＋ASan/UBSanで通過したが、これは一括pollの
専用unitではない。gpud実装・stage・guest導入ELFのSHA-256は
`2a5fee93c71828dc23b72b066aae06ef633c29712cc7a6019e5feafeb7f4523e`で一致。
復元sandboxはdebug情報/build-id以外が旧通常版と一致。Linux upstream差分ゼロ。

- [eventFD候補](../.artifacts/fishbowl-event-drain-trial.5aZSVI/)
- [ring arm候補](../.artifacts/fishbowl-ring-arm-trial.ap8VzC/)
- [一括監視build・Mesa回帰・導入照合](../.artifacts/fishbowl-hangup-batch/)
- [初回Xorg停止・比較除外](../.artifacts/fishbowl-hangup-batch-trial.rY2U1I/)
- [変更前](../.artifacts/fishbowl-hangup-baseline.54tfPm/)
- [変更後](../.artifacts/fishbowl-hangup-candidate.aK9D4m/)

## 描画中のMAP枯渇をGEMの寿命管理で修正

前節のXorg停止を、LPRの失敗時だけの診断で再現した。
Linux PID 44の`VIRTGPU_MAP (0xc0106441)`が`-ENOSPC (-28)`を受けていた。
native mmapやLPRのlease取得より前の失敗である。診断はsourceから撤去し、
LPR ELFも通常版3470bfec…へ復元した。失敗runにFPS値は付けない。

コード上、gpudのmappingはDRM fileのhandleだけを持ち、GEM_CLOSEでは所有参照を
閉じていなかった。munmapでleaseがなくなってもmappingのGEM pinが残り、
DRM file全体のcloseまで64枠を占有する。また同じGEMへのMAP再要求でも新しい枠を
作っていた。既存card0/render smokeへBOの入れ替えを追加すると、変更前は既存1枠に
続いてiteration=63でENOSPCを再現した。red Gateはこの失敗文字列を期待した確認であり、
正常動作のGate合格ではない。

gpudのprivate metadataにGEM handleを記録し、成功したGEM_CLOSEで対応するmappingの
所有参照を閉じる。VMAのleaseが残る間はGEM pinを維持し、最後のleaseが消えれば回収する。
同じfile・生存GEMへのMAPは、入力検証後に既存offsetを返す。閉じたGEMの番号が
再利用されても旧mappingは選ばない。64枠の拡張、wire ABI、kernel、core、OSSの変更はない。

修正版は96回のcreate→MAP→mmap→GEM_CLOSE→VMA読み書き→munmapを完走。
MAP再要求のoffset一致、既存PRIME共有、最後のFD close後のVMA保持、client強制終了と
再openも既存Gateで通過。初回green fixtureには異なるBOのoffset非再利用も要求していたが、
Linuxでoffset再利用は許されるため、その余分な条件は削除して再build・導入した。
Mesa multi-clientの共有・fence・SIGKILL・生存client描画・第三client再openも通過。
translation unitはGCC/Clang、query unitはGCC（各ASan/UBSan）で通過した。

通常LPR・修正gpudでfishbowlを60秒実行し、2515完全frame、**41.924 FPS**。
frame 23.853ms、paint 4.590ms、frame間19.046ms、QEMU CPU 73.51秒/約30秒窓。
frame/IO欠落ゼロ、IO宣言/取得28631/28631。Xorg停止、MAP OOM、object-table-fullなし。
Intel D3D12 VirGL、KVM・4CPU/2GiB・network有効。これは約42 FPSの維持と枯渇修正の確認で、
新たなFPS改善率は主張しない。Linux標準の約60 FPSには未達。
gpudのbuild・stage・guest読出しSHA-256は
`88a2bac1a1e49fcd521a20155851fc3daa5e2bfcfbe1a5a6cc14c5f969cdc46c`で一致。
並行検討した返信待ちの追加probe候補は評価前に撤去し、通常版には入れていない。

- [LPR失敗時診断・通常版復元](../.artifacts/fishbowl-map-failure/)
- [ENOSPC再現run](../.artifacts/fishbowl-map-failure-trial.apcvTV/)
- [red/green・build・unit・Mesa回帰・導入照合](../.artifacts/fishbowl-mapping-lifetime/)
- [修正後の60秒描画](../.artifacts/fishbowl-mapping-fixed.Hup2j6/)

## 返信前の追加受信probeは不採用

gpudのGPU RPCは最初の空受信でclock取得とFD_WAIT_MANYに進む。
短い返信で休眠を避ける候補として、RPCごとに最大3回だけ追加受信した。
予算はevent受信で補充せず、DRM呼出し・busy判定・通知契約は変更しない。

| 同条件の30秒測定 | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒/約30秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 通常版 | 1285 | 42.864 | 23.330 | 3.791 | 19.324 | 74.00 |
| 最大3回追加受信 | 1237 | 41.266 | 24.233 | 4.833 | 19.187 | 75.27 |

固定Button 1、Intel D3D12 VirGL、KVM・4CPU/2GiB・network有効。
両runともframe/IO欠落ゼロで完走。改善の根拠がなく、候補を撤去した。
追加probeもnative syscallであり、要求の往復数・実処理は減らない。
休眠回避回数を直接計数していないので、競合増加を確定原因とはしない。
この一組から一般的なbusy-pollの有効性を否定するものでもない。
以後は追加syscallで待つ方式ではなく、受信と待機の統合を検討する。

- [通常版](../.artifacts/fishbowl-reply-baseline.3xYV5f/)
- [不採用候補](../.artifacts/fishbowl-reply-candidate.9Gd86T/)
- [build・候補ELF・復元](../.artifacts/fishbowl-reply-probe/)

## 有限RECV_WAITへの統合とHANGUP修正

追加probeの次に、gpudのEMPTY→clock→FD_WAIT_MANY→RECVを期限付き受信へ統合した。
既存RECV_WAITは有限timeoutを拒否していた。またpeer close時の共通起床処理が
direct-recvをpollとして扱い、メッセージを書かずに成功を返し得た。
kernel監査エージェントの事前承認と差分再監査を受け、rootが次の限定範囲を実装した。

- native_ipc: 有限ticksを既存blockへ渡す。EMPTY後だけpeerのlast-closeを確認しCLOSED。
  queued messageは先に受信し、recv/waitにPOLL権限を追加しない。
- fd: token claim・wait group取消後、direct-recvのHANGUPをCLOSEDで完了する。
  この経路ではusercopy・FD installを行わない。
- scheduler_connection: BSP/APの期限加算2箇所を飽和加算にする。方針・quantumは変更しない。

ABI番号やレコードは変更しない。gpudは絶対deadlineから残量を算出し、割込み等の
EAGAIN後も期限を延長しない。IPC packetのgeneration・所有権検証は従来処理を共用する。
gpud/gpu_rpc.cは13行追加・42行削除。追加のthread・queue・busy loopはない。

| 構成 | 測定秒 | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒/約30秒窓 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Linux標準virtio_gpu・既存比較 | 30 | 1799 | 60.000 | 16.667 | 0.690 | 15.912 | — |
| PachaOS直前通常版 | 30 | 1285 | 42.864 | 23.330 | 3.791 | 19.324 | 74.00 |
| PachaOS有限受信版 | 60 | 2555 | 42.598 | 23.475 | 3.784 | 19.463 | 73.65 |

Linuxはkobox経由ではなく標準kernel DRMである。比較条件は固定Button 1個、
Intel D3D12 VirGL、KVM・4CPU/2GiB・network有効。PachaOS有限受信版のframe/IO欠落はゼロ、
IO28825/28825。Linuxの全frameは有効だが全IOはoverflowしており、ここでは使わない。
測定時間が異なり、FPS/CPUとも改善とは判断できない。60 FPSには未達。

有限受信版を保持する理由は、実在するHANGUP誤成功の修正、既存timeout契約の実装と
待機処理の重複削減であり、実測できた速度改善ではない。受信syscall数だけを削っても
この場面の残る約6.8ms/frameを解消できない、という結果として次の分析に使う。
4KiB出力コピーもコード上は残るが、支配的と測定できていないので変更していない。

確認済み:

- kernel 182/182。返信claim→timer、timer→旧claim、取消起床→旧claimを実関数で
  決定的に検証。これはschedulerの線形化試験であり、実usercopyまでの競合試験とは区別する。
- 既存native IPC Gate: 有限timeout、旧buffer保持、後続FD受信、巨大有限値、無限待機中の
  child終了、close-before-wait、queued message後のclose、peer dupのlast-close、
  POLL権限なしの有限待機、ゼロtimeout、19FD共有・MOVE・generation・死亡回収。
- Mesa multi-client、sandbox強制終了→generation 2→再open・cube描画/fence/画素/表示、
  LPR async signal Gateを通過。kernel単体に加えて実IPCの取消・プロセス終了経路を確認した。
- IPC unitはGCC/Clang＋ASan/UBSan。通常LPRのSHA-256は3470bfec…で変更なし。

既存native IPC試験のpackage参照は旧ET_DYN coreだったため、通常buildと同じ
gpud-production-runtimeのET_EXEC coreへ更新した。coreの実装・バイナリ自体は改変しない。
再起動Gateの初回はrootfs全再buildでXfce/Mesaのlibgallium重複衝突によりQEMU起動前に失敗。
SKIP_SYNC対応を他Gateと揃え、配備済み環境で再実行して通過した。全再buildの衝突自体を
解決したという結果ではない。試験後は通常boot設定へ復元している。

配備済みgpud SHA-256: `eb576985664c54edb1d3cce9609857445f4e53060bfe50fb048e1783f3c3fa51`。
sandbox: `3dec45cd22781674f7bc1e2c8a8dc2d2b2e16dc7613901214ed0a20990303895`。
guest読出しと一致。kernel: `f1fe1a497c39bdef1c9438bc5105d489ccccfbed895e112ea8fce491d47422ec`。
Linux upstreamの差分ゼロ。Mesa・Xfceソース編集なし。

- [build・unit・native/Mesa/restart/signal回帰](../.artifacts/fishbowl-timed-receive/)
- [有限受信版60秒測定](../.artifacts/fishbowl-timed-receive-trial.0RyM1z/)
- [Linux標準DRMの既存比較](../.artifacts/fishbowl-owned-burst-linux.LtscGq/)

## 一時staging再利用は不採用

sandboxの描画命令用private stagingを、毎回の匿名map/unmapから容量内再利用へ変更して比較した。
完了公開後の再利用・内容消去・容量拡張・停止時の一度だけの解放はGCC/Clangの
ASan/UBSan unitで通過したが、通常版同士では描画速度の改善を確認できなかった。

| 固定Button 1個・30秒 | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒/約30秒窓 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 再利用なし・対照 | 1304 | 43.482 | 22.998 | 3.943 | 18.823 | 73.40 |
| 再利用候補 | 1305 | 43.512 | 22.982 | 3.812 | 18.988 | 72.66 |

Intel D3D12 VirGL、KVM・4CPU/2GiB・network有効、frame/IO欠落なし。
各1回で約0.07%のFPS差、CPU差も約1%であり改善とはしない。
近接診断runのGPU submitは約213回/秒・flushあたり約5回。対象はこの描画命令等の
一時領域であり、多数のWAITとIPC往復を減らさない。最大16.25MiBの保持と管理処理を
追加する利益が確認できないため、候補と専用test追加を撤去した。過去42.598 FPSから
候補43.512 FPSへの差も、この修正の効果とは扱わない。Linux標準DRMの約60 FPSには未達。

対照の初回はWriterアイコンの追加で画像一致条件が失敗したが、保存画像ではdesktopは
表示済みだった。Home/File Systemと壁紙の既知hashは維持し、追加shortcut領域だけ
判定から外して再実行した。測定moduleが要求されたのに読み込めなかった場合も、
今後はrunnerが失敗扱いにする。測定module欠落runと内訳probeが出なかった診断値は、
通常版の区間比較には使用しない。

別途、Xfceの通常buildで起きたGallium重複衝突を修正した。固定Mesa版・許可済みpatch記録・
ライブラリhashを確認して共有Mesaのbuildに揃え、他ファイルの厳密な重複検査は維持する。
Xfceの447固定package buildと通常rootfs同期は通過。OSSソースの追加編集はない。

- [候補・unit・build・復元](../.artifacts/fishbowl-private-staging/)
- [候補測定](../.artifacts/fishbowl-private-staging-trial.CjjDYL/)
- [通常版対照](../.artifacts/fishbowl-private-staging-control.GIpqUg/)
- [Xfce build](../.artifacts/fishbowl-mesa-overlay/build.log)

## RPC返信待ちの分解とchannel SEND起床試行の撤去

通常版に一時カウンタを付け、固定Button 1個の測定窓内に完全に入る
163,840要求を集計した。guest TSCはQMPで得た3,686,399,000 Hzで換算。
clock 2.839µs、要求公開5.604µs、返信待ち42.773µs、返信取得・解放0.183µs。
公開直後にused ringが完成していたのは114件（0.070%）だけだった。
完成済み返信の通知待ち省略は対象が小さく、複雑な通知状態管理を追加しない。
これは全RPCの平均であり、WAIT ioctl限定やGPU実行時間ではない。

別診断runの定常131,072要求では、sandboxの準備2.421µs、準備完了→dispatch入口
11.088µs、dispatch 11.603µs、used公開0.216µs、通知sendから戻るまで68.855µs。
先頭のログが混線した集計は除外し、完全な後半4組だけを使用した。
通知sendには受信側へhandoffしている時間が含まれ、gpud側の待ちと重なる。
異なるrunの平均を引き算したり、69µsを全量削減できる費用とは扱わない。
診断runのFPSはそれぞれ43.454、42.357で、通常版性能の比較からは分ける。

kernel監査の事前許可後、channelへのSENDだけを非Preferred起床・明示handoffなしに
する最小試行をrootが実装した。enqueue前にFD種別を保存し、CALL/REPLY、endpoint・
replyへのSEND、token claim、generation検証、copyout、FD回収、ABIは維持した。
変更後監査、kernel 182/182、native IPC実Gateは通過した。

| 計測hookなし・Button 1個・30秒 | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒/約30秒窓 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 直前の通常版 | 1304 | 43.482 | 22.998 | 3.943 | 18.823 | 73.40 |
| channel SEND起床変更 | 1167 | 38.911 | 25.700 | 5.653 | 19.816 | 90.77 |
| 撤去後の通常版 | 1283 | 42.773 | 23.379 | 3.734 | 19.420 | 74.23 |

各1回だが、候補は復元対照より約9%遅く、CPU消費も約22%増えたため不採用。
送信側を継続させるだけでは同期往復は短縮せず、受信側への実行引渡しを失う。
remote起床・競合のどの費用が増えたかは今回の集計だけでは確定しない。
両runともframe/IO欠落ゼロ、interaction Gate通過、KVM/4CPU/2GiB/network有効、
Intel D3D12 VirGLのglamor、DRI3/PresentをXorgログで確認した。
Linux比較基準は引き続き標準kernel virtio-gpuの約60 FPSで、Linux kobox版ではない。

今回のkernel変更とgpud/sandboxの計測コードは撤去した。復元buildは元のhashと一致:
kernel `f1fe1a49…`（boot image読出しも一致）、gpud `eb576985…`、sandbox `3dec45cd…`。
Linux upstream、Mesa、Xfceの追加変更なし。通常版約43 FPSを維持し、60 FPSは未達。
次の候補評価では、全RPC平均のdispatch-gapをWAIT固有の費用と決めつけず、
Linux owner内の継続受付とreceiver経由の再起動をコードと対応づけて切り分ける。

- [gpud RPC診断](../.artifacts/fishbowl-rpc-stages-trial.qcigN1/)
- [sandbox区間診断](../.artifacts/fishbowl-queue-stages-trial.ByOE4z/)
- [kernel試行のbuild・native IPC・復元](../.artifacts/fishbowl-channel-wake/)
- [試行版](../.artifacts/fishbowl-channel-wake-trial.LoOxLr/)
- [復元対照](../.artifacts/fishbowl-channel-wake-control.odkRjr/)

## dispatch入口と共通syscall処理の追加切り分け

sandboxのdispatch-gapを、Linux ownerによる継続受付とnative receiver経由に分けた。
固定Button測定窓内の完全な4組、計131,072要求を使用した。

| WAIT要求の受付 | 回数 | 準備完了→dispatch入口の平均 | dispatch本体の平均 |
| --- | ---: | ---: | ---: |
| Linux owner内 | 115,903 | 0.128µs | 5.653µs |
| receiver経由 | 6,955 | 145.026µs | 7.972µs |

WAITの94.34%は既存の継続受付で、入口までの空白はほぼない。
全RPC平均の約11µsを全WAITから削れるわけではない。receiver経由のWAITのgap合計は
約1.009秒/約23.7秒の集計区間で、42 FPS換算では約1ms/frameに相当する。
すべてcritical pathから消えるという楽観的な換算でも、60 FPSまでの差全体ではない。
poll継続時間の延長は行わず、計測コードは撤去。gpud/sandboxの通常build hashは復元一致。
このrunはXorgの警告がobserverのIO行に混線したため、IO全体の比較には使わない。
frameのみの診断値は41.370 FPSで、通常版の改善結果ではない。

別runでkernel監査許可後、共通入口のwait-group回収とunlock時のdeadline走査を計測した。
30.0578秒のQMP差分、TSC 3,686,399,000Hzで換算。frame測定窓とは端点が少し異なる。

| 共通処理 | 回数 | 累積時間 | 対象1回の平均 |
| --- | ---: | ---: | ---: |
| 回収入口 | 8,063,794 | 未計測 | — |
| 非空bucketのwait-group回収 | 4,424 | 12.002ms | 2.713µs |
| deadline走査＋publish | 8,092,140 | 1.634秒 | 0.202µs |

非空bucketなのに一致groupを一つも回収しない例は0件、回収group数は4,424。
wait-group全走査の対象は小さく、索引の追加は行わない。deadlineのbitmap化は以前の
起動時間試行で不採用だった案で、今回も再実装しない。表の全走査だけで大幅な改善を
見込める結果ではない。native syscall全体は8,063,888件、うち時計取得1,766,940件、
GETTID 890,110件。次は回数の多い呼出の実装と共通lockの必要性を区別して評価する。
一時計測は撤去し、通常kernel `f1fe1a49…`へのbuild hash一致を確認した。

- [受付経路の診断](../.artifacts/fishbowl-queue-path-trial.PaA5oB/)
- [共通処理の診断](../.artifacts/fishbowl-common-scan-trial.ZeRZFT/)
- [共通処理probeのbuild・通常版復元](../.artifacts/fishbowl-common-scan/)

## PID/TID専用入口を描画負荷で再評価し、撤去

前の起動時間調査で不採用だったidentity fast pathを、今回判明したGETTID回数を理由に
描画負荷で一度だけ再評価した。kernel監査の事前承認後、rootが同じ9行を再適用。
GETPID/GETTIDだけをmutable descriptor参照・global KernelState lockより前で処理し、
lifecycle admission、既存返値生成、外側のsignal/user-returnを維持した。
copyoutを伴うCLOCK_GETTIME、lock_policy、ABIは変更していない。

| 固定Button 1個・30秒・通常build | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 先行する通常版対照 | 1283 | 42.773 | 23.379 | 3.734 | 19.420 | 74.23 |
| identity候補 | 1261 | 42.087 | 23.760 | 4.159 | 19.364 | 74.31 |
| 撤去後の通常版対照 | 1209 | 40.328 | 24.796 | 8.039 | 16.585 | 75.48 |

候補は前後の通常版の間に入り、CPU消費の明確な低下も確認できない。通常版の振れがあり、
候補が遅くしたとも、復元対照との差が改善効果だとも断定しない。対象の共通deadline
走査費だけなら約0.18秒/30秒で、lock競合解消による大幅な間接効果もこの比較では
確認できなかった。性能上の採用根拠が不足するため、9行と専用test追加は撤去した。
候補はkernel 182/182 unit、既存native fixtureの4CPUでPID/TID、通知、fault復帰、時計を
通過した。候補/復元対照ともinteraction PASS、frame/IO欠落ゼロ、Intel D3D12 VirGL、
glamor・DRI3・Present、KVM/4CPU/2GiB/network有効。Mesa multi-client等の再実行はせず、
通常版へのsource/build/boot image復元で終了した。今回の診断だけで60 FPS達成とはしない。

kernel `f1fe1a49…`は変更前・再build・boot image読出しで一致。
gpud `eb576985…`、sandbox `3dec45cd…`、core `a8bcf9b8…`も維持。
Linux upstream差分ゼロ、Mesa/Xfce追加編集なし。

- [identity試行build・unit・native 4CPU・復元](../.artifacts/fishbowl-identity-path/)
- [候補測定](../.artifacts/fishbowl-identity-path-trial.5TBrh6/)
- [復元対照](../.artifacts/fishbowl-identity-path-control.kDViLb/)

## owner空待ちの共有ring確認も不採用

ownerの空待ちは各反復でcontrol channelをRECVし、event FDをREADしてから時計を取る。
既存queue engineのarm/recheckを利用するprivateなready callbackを試し、共有ringも
event_pendingも空の間だけRECV/READを省略した。要求が見つかれば従来どおりcontrolを
先に受信し、STOPを優先する。controlだけの要求は既存150µs burst終了まで遅れ得る。
receiverのblocking前のIPC確認・nextによるarm/recheckは維持した。
続いて、memory-only確認を最大16回まとめ、残る時計取得の頻度も抑えた。
kernel、core、ABI、OSSは変更していない。

| 固定Button 1個・30秒・通常build | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 直前の通常版 | 1209 | 40.328 | 24.796 | 8.039 | 16.585 | 75.48 |
| memory-only確認 | 1251 | 41.702 | 23.980 | 4.583 | 19.198 | 73.67 |
| memory-only確認を16回まとめる | 1249 | 41.651 | 24.009 | 4.034 | 19.750 | 74.00 |

通常版の先行42.773 FPS/74.23 CPU秒も含めると、改善を確立できる結果ではない。
時計確認をさらに減らしても同等だった。この方式は空待ちの反復だけを対象とし、
同期DRM要求そのものの往復回数・実行費を変えない。前節のowner dispatch-gapも
元から0.128µs程度であり、大きな短縮の採用根拠が不足する。callback、batch、
専用test追加をすべて撤去した。CPUが低下したという再現性もまだ主張しない。

両候補はframe/IO欠落ゼロ、interaction PASS、Intel D3D12 VirGL・KVM・4CPU・network有効。
lifecycle/query unitはGCC/Clang＋ASan/UBSanで通過。空状態でIPCを呼ばないこと、
publicationの検出、STOPが次requestより先に処理されることを既存fixtureで確認した。
候補sandboxのguest読出しを照合した後、通常版を再build・同期した。
復元gpud `eb576985…`、sandbox `3dec45cd…`（guest読出しも一致）。kernelは未変更。

- [build・unit・復元](../.artifacts/fishbowl-ring-idle/)
- [単回の共有ring確認](../.artifacts/fishbowl-ring-idle-trial.f3QJ5J/)
- [16回まとめた候補](../.artifacts/fishbowl-ring-idle-batch.YRWnOl/)

## syscall入口のkernel PCID保持も不採用

8百万回/30秒のnative syscallすべてに関わる入口を確認した。既存PCID割当は有効だが、
kernelへ入るCR3ロードも毎回flushingだった。監査の事前許可後、local CR4.PCIDEが有効で
入口のuser PCIDが非0の場合だけ、一時レジスタのkernel CR3へNOFLUSHを付けた。
PCID 0はuser側のfallbackにも使うため、この場合とPCID無効時は従来どおりflushする。
保存CR3、user-return、IRQ/例外入口、copy windowのINVLPG、全CPU shootdownは未変更。
過去のuser NOFLUSH撤去理由だったprocess slot/PCID再利用には触れていない。

| 固定Button 1個・30秒・通常build | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| kernel PCID保持候補 | 1250 | 41.675 | 23.995 | 4.779 | 19.002 | 74.87 |
| 撤去後の通常版対照 | 1266 | 42.211 | 23.690 | 4.734 | 18.766 | 74.80 |

FPS・CPU消費とも改善を確認できず撤去。候補は片道のTLB invalidationだけを省き、
CR3ロード自体、user側flush、syscall/DRMの往復回数は減らさない。追加CR4/CR3読取りの
費用とTLB miss削減は個別には測っていないため、どちらが相殺したかは未確定。
別の4CPU native起動で各CPUのCR4=0x606a0を確認し、PCID機能が無効だったとはしない。

生成ELFの分岐・保存済みRAX利用を確認、事後監査も承認範囲内。kernel 182/182 unit、
4CPUの通知・fault復帰・時計をPCID有効/無効の両構成で通過。PCID無効構成もKVM有効。
性能を理由に不採用としたため追加のMesa multi-client等は回さず、通常版へ復元した。
両fishbowl測定はinteraction PASS、frame/IO欠落ゼロ、Intel D3D12 VirGL・DRI3・Present、
KVM/4CPU/2GiB/network有効。候補のIOは14,263件、対照は14,502件。

traps.zigの差分ゼロ、再buildとboot image読出しは元のkernel `f1fe1a49…`と一致。
gpud `eb576985…`、sandbox `3dec45cd…`、core `a8bcf9b8…`は未変更。
Linux upstream差分ゼロ、Mesa/Xfce追加編集なし。

- [build・unit・native PCID有効/無効・復元](../.artifacts/fishbowl-kernel-pcid/)
- [候補測定](../.artifacts/fishbowl-kernel-pcid-trial.EllNL7/)
- [復元対照](../.artifacts/fishbowl-kernel-pcid-control.u4ei20/)

## kernelのred-zone設定を修正し、ReleaseFast候補は撤去

kernel本体のReleaseSmall固定は旧boot方式から継承されており、速度優先の生成コードを
比較した。事前監査の条件としてELF範囲と割込み入口を確認したところ、**元のSmall版にも
red-zoneの実使用**が見つかった。例えば`pci.interruptVectorBaseForResourceId`はRSPを
確保せずRBP-64まで一時配列を置く。timer／wake IPI／device IRQはIST 0なので、ring 0への
割込みがこの領域を上書きし得る。実際の破損や、これがFPS低下の原因だったとは断定しない。

別途監査許可を得てrootが`kernel/build.zig`の2 moduleへ`.red_zone = false`を追加した。
LLVMの`noredzone`属性と、既知の関数に`sub rsp,0x40`が入ることを確認。
外部C schedulerは既に`-mno-red-zone`。compiler-rtを含むlinked ELFの単純leaf走査では
候補0件になったが、この走査を任意の制御フローの網羅的証明とはしない。
安全性修正は性能比較とは独立して保持する。新規ABI・wrapper・fixtureは追加していない。

同じred-zone禁止条件で、production module 2箇所だけをReleaseFastにして比較した。
XSAVE前／XRSTOR後のSIMD clobber追加はなく、ELF最終LOAD末尾は`0xffffffff8ca08510`で
既存256MiB mapping内。CPU target、Cの最適化、Linux core、Mesa、Xfceは変更していない。

| 固定Button 1個・30秒・通常build | 完全frame | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Small＋red-zone禁止 | 1308 | 43.621 | 22.925 | 12.288 | 10.450 | 74.57 |
| Fast＋red-zone禁止 | 1313 | 43.794 | 22.834 | 3.669 | 18.929 | 75.36 |

FastのFPS差は約0.4%、CPU消費は減らない。paintとframe間の配分は大きく変わるが、
frame全体の改善とはしない。サイズ優先から速度優先へ変更するだけの効果は確認できず、
最適化指定2行をSmallへ戻した。Linux標準DRMの約60 FPSには未達。
両測定はIntel D3D12 VirGL、glamor/DRI3/Present、KVM/4CPU/2GiB/network有効、
frame/IO欠落ゼロ（IO 16,261件／14,538件）、interaction PASS。

Debug/Fastの既存unitは各182/182通過。両候補のnative 4CPUで通知・fault復帰・時計がPASS。
保持するSmall＋red-zone禁止では、既存のsignal/SSE/exec状態、fork COW/mprotect、
DRM mapping churn 96回、FD共有・死亡回収・Mesa 3client再openを同じQEMUで確認した。
native IPC・process/launch・stale/backpressureも通過した。
gpud再起動Gateもgeneration 1→2、再open、描画・fence・画素・KMS表示までPASS。
終了後のconsole-shell→startx、seed0rootの再起動試験flag OFFへの復元が完了した。

保持kernel: `5d202653028a7865e735a07cc8fe57a9ed1c04dda5606321b29914ce539ead8c`。
boot imageからの読出しも一致。gpud/sandbox/coreの通常hashは維持した。
不採用Fast: `1dd97f3f4cfadf7795fcc805a02e9651673e90790b684a33639bd2ca8ce66db0`。

- [監査用生成コード・unit・native・回帰ログ](../.artifacts/fishbowl-kernel-optimize/)
- [Small＋red-zone禁止の測定](../.artifacts/fishbowl-kernel-small-nored.SCwgE4/)
- [Fast＋red-zone禁止の測定](../.artifacts/fishbowl-kernel-fast-nored.Kvl6RC/)

## 通常版のLinux比較とISR探索の重複読取り

同じ配備済みMesa/Xfce、Button 1個、Intel D3D12 VirGL、KVM/4CPU/2GiBで再比較した。
Linuxは標準kernel DRMであり、Linux版kobox sandboxではない。Linux runnerへ任意の
`INTERACTION_KVM_STATS=1`を追加し、PachaOS runnerと同様にQMP統計とhost CPU差分を採取。
host側約30秒窓とGTKの30秒窓は近接しているが、厳密に同時開始ではない。

| 構成 | FPS | frame ms | paint ms | frame間 ms | QEMU CPU秒 | KVM exit回数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Linux標準DRM | 59.999 | 16.667 | 0.696 | 15.912 | 17.61 | 246,347 |
| PachaOS通常版 | 42.092 | 23.758 | 5.049 | 18.512 | 75.78 | 2,528,935 |
| PachaOS・ISR word探索候補 | 41.741 | 23.957 | 4.421 | 19.323 | 75.96 | 2,375,273 |

通常版のQEMU vCPU thread CPU合計はLinux 9.20秒、PachaOS 66.56秒。
main threadは8.41／9.22秒で、CPU差の大半はゲスト実行側にある。
Linuxのframe欠落は0だがIO記録は32768件でoverflowしているため、全期間の表は
frame-only解析。IO回数・待ち内訳の完全な全期間比較には使っていない。
VM exit総数だけではMSR、割込み、その他の内訳や所要時間は分からない。

コードでは`activeInterruptVectorInRange`が、同じ32bit ISRをvectorごとに再readしていた。
IF clear・EOI前でISRが変化しない範囲に限り、wordごとに1回読み、範囲maskとctzで
従来どおり最小vectorを選ぶ候補を試した。GPUはboot登録slot 1なので、0x50から
0x60/0x61までの探索が17/18回readから2回になる。事前・事後kernel監査を通過。
EOI、配送、pending drain、ABIは未変更。既存182 unitと別実行のlapic 6 unitがPASS。

exitは約6%減ったが、FPS・CPUの改善を確認できないため候補と専用test登録を撤去。
読取り回数削減を、そのままexit削減数やフレーム短縮量として扱うのは誤りだった。
IRQ処理のこの重複だけでは、現行の同期DRM往復・アプリ側待ちを解消しない。
通常kernel `5d202653…`を再buildし、boot image読出しも一致。lapic.zig差分ゼロ。
両Pacha測定はinteraction PASS、frame/IO欠落ゼロ。Linux upstream差分ゼロ。

- [Linux標準DRM・KVM/CPU比較](../.artifacts/fishbowl-kvm-current-linux.CTHY73/)
- [PachaOS通常版・KVM/CPU比較](../.artifacts/fishbowl-kvm-current-pacha.uW9SSg/)
- [ISR探索候補・unit・復元](../.artifacts/fishbowl-isr-word/)
- [ISR探索候補の実測](../.artifacts/fishbowl-isr-word-trial.yQG8um/)

### gpudの返信ringを短くpollする候補も撤去

既存の`kb2_vq_take_used`を最大1024回、syscallなしで試してから従来の有限RECV_WAITへ
進む候補を試した。即完了を検出しても、事前armで要求したused通知とFD attachmentは
必ず受け取り、chainを返信通知より先に解放しない。kernel、protocol、sandbox、coreは
変更せずgpudだけを変更。ABI・通知の抑制・新しいthreadやfixtureは追加していない。

実Mesa fishbowlは1223完全frame、40.776 FPS、frame 24.524ms、paint 4.727ms、frame間
19.593ms。CPU 76.72秒/30.058秒で、通常版42.092 FPS/75.78秒に対し改善なし。
KVM halt exitは195619→190954回だが、描画量も減っており、約155→156回/frameで
処理量あたりの削減にはならない（hostとGTKの窓は近接であり厳密な対応ではない）。
完了検出までのメモリpollだけでは、必須のIPC受信・通知・起床そのものはなくならない。
pollの成功率・個別のpark回数は直接記録していないため、どの要求で失敗したかまでは
断定しない。上限を増やすだけの追加試行はせず、候補を撤去した。
GCC C11警告check・native build・interaction PASS、frame/IO欠落ゼロ。
撤去後のgpud sourceは試行前と一致。再build・stage・guest読出しも通常版
`eb576985…`と一致し、kernel `5d202653…`、sandbox `3dec45cd…`、core `a8bcf9b8…`を維持。

- [gpudの候補build・配備照合・復元](../.artifacts/fishbowl-rpc-ring-wait/)
- [共有ring短時間pollの実測](../.artifacts/fishbowl-rpc-ring-wait-trial.M6zv3y/)
