# netd 再設計・実装と検証の記録

2026-09-09。対象設計は [netd-redesign-plan.md](netd-redesign-plan.md)。
**指定範囲の実装・検証を完了。netdの旧AF_UNIX実装・専用opcode・SCM wireとLPR旧UNIX経路を削除し、unixdと方向別共有VMOへ移行した。通常版へ戻したQEMUで全socket probeが成功し、同一の本番binaryでXFCE・Writer操作とRTTも検証済み。OSのUID/GID・補助グループ・認可やsystem busの機能追加、既存DPMS復帰制約の解消は含めない。以下の古い「未完了」「次の対象」は履歴であり、最新記録を優先する。**

## 最新の実機経路チェックポイント（以前の項目より優先）

### 完了確認と通常版への復帰

- `.artifacts/test-results/unixd-netd-stopped-final/`: nativeの観測capability権限、PRIVATEのfork除外、threadの自然終了／kill、別プロセスへの観測FD転送、世代非再利用を16反復して `UNIX_NATIVE_CONTRACT=OK`。netdは起動側が既に持つprocess capabilityで停止し、検査の前後でFD情報のstate=4を確認。その間に通常のinit・シェルから全socket probeを実行し `ISOLATION_EXIT status=0`。最後にnetdをCONTINUEして `UNIXD_NETD_STOP_TEST status=0 stopped_state=4 resumed=0 native_status=0`。kernel変更なし。
- socket probeにSIGKILL検査を追加。STREAM/SEQPACKET各8回、送信確定後の送信者死亡でもSCM_RIGHTSの参照とデータが残ること、受信後の保有プロセスをkillした場合も最後のsocket参照を回収してpeerへEOF/HUPを返すことを確認した。全16回で送信者をkillし、うち8回は受信者もkill。Linuxの同一検査も成功。
- native検査の初期失敗は、有限waitのNOT_READYを再試行せず失敗扱いしていた検査側の問題だった。単調時計による期限内で再確認するよう修正。Linux probeを起動側から直接EXECする初期手順はnetd停止前後ともexit 127だったため、通常の登録済みinit・シェル経由へ修正した。OSの実行権限・認証機能を追加して通してはいない。完了通知は空ファイル作成と本文書込の間を成功・失敗どちらとも判定せず、改行まで読んで確定する。タイマーFDで待機する。
- 停止用コードは `SEED0ROOT_UNIXD_CONTRACT_TEST=ON` の別ビルドだけで有効。通常CMake cacheはOFF、配置済みSEED0RT.ELFにも停止用markerがないことを確認。通常版を再ビルド・再配置して `.artifacts/test-results/unixd-restored-final/` で全probe再実行、`UNIXD_PAIR_DONE / RESTORED_EXIT status=0`、harness exit 0（6.5秒）。全QEMU終了済み。
- 最終通常版のkernel/LPR/unixd/supervisor/seed0rootの5 binaryは、GUI成功時 `unixd-edge-xfce/binaries.sha256` とすべて一致。異なるのはSIGKILL検査を追加したfixtureだけ。したがってGUI成功を別の本番実装へ流用していない。最終のconsole/serial/host-time/run/hashを保存。PAGE FAULT / GENERAL PROTECTIONなし。

### 設計項目と根拠の対応

| 確認対象 | 実装・確認した根拠 |
| --- | --- |
| netdからの分離、直接転送 | netd旧unix_socket.c/.h・旧opcode削除、LPRのunix専用backend、netd停止中の全probe完走 |
| STREAM/SEQPACKETの部分I/O・境界・shutdown、backpressure | 最終QEMUのPAIR_STREAM / PAIR_SEQPACKET / MESSAGE_IO / BACKLOG_WAKE / POLL_EPOLL |
| pathname/abstract、inode同一性・unlink/rebind | 最終QEMUのNAMED_CONNECT / PATHNAME、probeのhardlink・unlink後既存接続・再bind検査 |
| DGRAM分離・宛先順序/容量・切断時の破棄 | 最終QEMUのDGRAM_PAIR / NAMED / BOUNDARIES / DISCONNECT、broker/rightsの偽造header・破棄済みHEAD検査 |
| OFDのdup/fork/exec、FD転送・切詰め・PEEK | 最終QEMUのRIGHTS_IO / PTY_RIGHTS / SOCKET_RIGHTS / FORK_EXEC、rights補助検査の253参照・CLOEXEC・循環GC |
| 資格情報と制御capability | 最終QEMUのCREDENTIALS（全3種）、native PRIVATE/rights/世代検査、service側の所有者・cap権限検査。既存root資格情報ドメインでの確認 |
| 確定前後の死亡・応答再送 | transport/rights補助検査の未公開取消・公開保持・消費前後のthread死亡・FINISH再送/ACK/再利用、QEMUのnative死亡通知とKILLED_PROCESS_RIGHTS |
| 独立待機者・arm/recheck・通知喪失防止 | `.artifacts/unixd-final-notify-check.log`の独立待機口・飽和・stale解除・arm/commit/death境界・50000メッセージ、最終QEMUのREMOTE_EDGE / REMOTE_WRITE_EDGE（全3種） |
| 上限・回収余力・待機登録再利用 | service rights/waitのadmission・private受信・終了回収補助検査、最終QEMUのWAIT_REGISTRATION_LIFETIMEとXFCE負荷。保持数は既存上限で制限 |
| 診断と責務分割 | broker DIAGのsocket/session/operation/delivery、参照数・backlog/容量、共有方向の公開/消費位置・reader/writer。共有値は独立atomic観測であり認可には使わない |
| XFCE・Writer・キーボード/マウス・RTT | 同一本番binaryの `unixd-edge-xfce/`、手動画面・ODT内部本文・RTT生値・harness exit 0。詳細と既存制約は次節参照 |

### 別プロセスのキュー操作とepoll ET（version 7）

- fork後に親だけがepollを作り、子がキューを空にして終了、親が再送してからepoll_waitする検査を追加。Linuxは3種類とも成功。version 6のQEMUはSTREAMで通知を取りこぼし、`remote-edge-refill-event`でexit 1。証拠 `unixd-remote-edge-before/`。harness自体のexit 0はゲスト検査成功を意味しない。
- TX/RXに成功した公開・消費のprogressを追加。失敗・cancel・PEEKは進めず、shutdownと死亡回復も進める。公開位置の確定後、owner解除前に世代を進めるため、その間の死亡は既存owner回復で再通知できる。DGRAM配送・宛先容量回収・listener接続待ち・保留エラーはbroker側の世代をPOLL応答で返す。通常の接続済みSTREAM/SEQPACKET pollは共有領域だけを参照する。
- epollがIN/OUT/ERR別の観測済み世代を保持し、同じreadyが続いて見えても新しい公開・空き容量を検出する。待機graphにも観測済み世代を渡し、slot登録後にもう一度比較する。変更していない方向を再通知せず、ONESHOTのdisarmは維持。epoll内の記録増加で従来の127登録枠を減らさないようinstanceを4KiBから8KiBへ変更。
- 共有headerとPOLL応答のlayout変更が理由でUNIX内部ABIをversion 7へ更新し、LPR/unixd/supervisor/seed0rootを一括ビルド・配置した。opcode追加はなく、既存の連番はそのまま。kernel編集・native syscall ABI変更なし。
- `unixd-remote-edge-final/` で、親のepollに子が関与しないINの空→再送、OUTの満杯→全消費を3種類すべて検証し、同じedgeの再通知がないことも確認。全既存probeを含め `UNIXD_PAIR_DONE / REMOTE_FINAL_EXIT status=0`。Linux側OUT検査は最初512B消費だけではwritable閾値に戻らず失敗したため、全消費後に検査するよう条件を修正した。QEMUに合わせて期待値を弱めたものではない。
- unitは世代変化の方向分離・ONESHOT、transport・broker・service rights/wait・LPR mapping/client/waitを補助検査。出力は `.artifacts/unixd-remote-edge-host-checks.log`。
- 最新GUIは `.artifacts/test-results/unixd-edge-xfce/`。同じversion 7 binaryのhash一致、QEMU 4 vCPU、desktop ready 18.143秒、5アプリ表示PASS。Writerで `Unixd epoll edges verified` を編集・保存し、終了して端末prompt復帰後に別プロセスで再オープンした。続けて `Saved again` の行を追加保存。回収した `unixd-edge.odt` のZIP全entryと本文2行の一致を検査し、画面も保存。Thunarの/usr→/rootへのマウス操作・Applicationsメニュー・端末入力を目視確認した。
- version 7の64B要求+64B応答、1024往復×5回、CLOCK_MONOTONIC、4 vCPU/KVM、2GiB、Q35、XFCE稼働中。socketpairスレッド間RTT中央値141.601µs（138.671〜144.531µs）、pathnameプロセス間144.531µs（133.789〜146.484µs）、全trial後RTT_OK。1ms刻みの集計時計のため、商の桁数を単発の計時精度と解釈しない。旧netd比の数値ではない。
- 今回はmanual-doneを期限内に作成し、同じharnessがexit 0、classification=readyで終了した。400.8秒は全手動操作込みで起動時間ではない。自動input_checkはnot-runのまま、手動画像・文書と区別する。PAGE FAULT / GENERAL PROTECTION・FD枯渇ログなし。既存のDPMS復帰制約をこの作業へ追加せず、試験セッションだけxset -dpms / xset s offで放置消灯を無効にした。system bus等の既存制約解消を含む無条件のXFCE正常化は主張しない。QEMUは終了し、全ログ・画像・文書・hashを回収済み。

### 範囲撤回後のversion 6 GUI・往復実測

- 証拠は `.artifacts/test-results/unixd-scoped-final/`。passwd/groupを元のrootのみへ戻して一括配置し、kernel/LPR/unixd/supervisor/seed0rootのhashが資格情報probe成功時と一致することを確認した。QEMUは終了済み。5アプリ表示検査は全件PASS、desktop readyは19.918秒。
- Writerで `Unixd redesign verified` を入力し `/root/unixd-scoped.odt` に保存、終了後に別プロセスで再オープンして本文を目視確認した。保存文書をconsole経由で回収し、ZIP全entry検査とcontent.xmlの本文一致も確認。`writer-saved.png`、`writer-reopened.png`、`unixd-scoped.odt`を保存。再オープン後の終了待ちが短く、計測コマンドの一部を文書へ誤入力したため、その未保存変更は「Don't Save」で破棄した。回収文書に誤入力が入っていないことも確認済み。
- 端末コマンド、Thunarの/usr表示からマウスで/rootへの移動、Applicationsメニュー展開を画像で確認。手動確認中の放置後に表示が無効になったが、Xorgは入力・xset照会に応答していた。xrandrの出力off/autoで表示復帰。drmdのDPMS ONがscanoutを復元しない処理はHEADと同一で差分なし。今回この既存表示制約の修正は追加せず、無条件のXFCE正常化成功とも書かない。
- 64B要求+64B応答、1024往復×5回、CLOCK_MONOTONIC、4 vCPU/KVM、2GiB、Q35、virtio GPU、XFCE稼働中。socketpairのスレッド間RTT中央値137.695µs（127.929〜143.554µs）、pathnameのプロセス間中央値133.789µs（126.953〜146.484µs）。各trialの生値はconsole.log。旧netdの同条件計測はなく、旧実装比の高速化率は算出しない。ゲスト時計の集計値は1ms刻みなので、商の桁数は単発RTTの計時精度を意味しない。
- 手動操作の証拠は上記の通りだが、manual-done作成が700秒の期限より約4秒遅れたため、harnessはexit 1 / manual-incomplete。result.jsonを成功へ書き換えない。この実行全体を自動検査PASSとは扱わない。console/serial/host-time/python/run/hashを保存。PAGE FAULT / GENERAL PROTECTIONなし。全socket probeの再実行はこのGUI起動では行っておらず、同一binaryの `unixd-credentials-final` が直近の証拠。

### ユーザー指定による範囲の訂正

以前から未実装のOS側UID/GID・補助グループ変更・認可の拡張は今回の対象外。system busの新規稼働も完了条件にしない。以下の履歴にある「次に実装」「確認待ち」「最終GUIの前提」は撤回する。追加したmessagebus/polkitdのpasswd/group登録は私の追加行だけを削除し、元のrootのみの定義へ戻した。netd/AF_UNIXの再設計と、その変更によるXFCE/Writerの退行検証を再開する。権限モデルやsystem bus設定の変更は行わない。

### system bus用アカウント登録と権限変更の境界

- 既存のrootfs公開元 `userland/fixtures/base/passwd` / `group` にmessagebus（UID/GID 100）とpolkitd（101）を登録し、既存packアプリで配置した。UID/GIDはこのrootfsの固定割当。ロック済みAlpine dbus/polkit-commonの.pre-installは両者をサービスユーザーとして作るが、ビルドは--no-scriptsで、プロジェクト共通passwd/groupも従来rootだけだった。対話ログイン用アカウントの新設ではない。kernel/ABI/LPR資格情報変更はこの段階では行っていない。
- `.artifacts/test-results/unixd-system-accounts/`: QEMUでpasswd/groupの配置を確認。`dbus-daemon --system --fork --nopidfile`の親はexit 0だったが、稼働成功の証拠にはしない。`unixd-system-bus-query/`で実GetId要求を行うとsocket不在で失敗（SYSTEM_QUERY_EXIT status=1）。
- `.artifacts/test-results/unixd-system-bus-foreground/`: --noforkでログを残して起動し、GetId要求まで実行。`Failed to drop supplementary groups: Function not implemented`、`Failed to set GID to 100: Operation not permitted`を確認。デーモン自身もExit 1、SYSTEM_FOREGROUND_EXIT status=1。console/serial/host-time/run/input hashesを保存、QEMU終了済み。/run/dbusは診断のためゲスト内に作成しただけで、自動起動手順はまだ変更していない。
- 次の境界はLinux UID/GID・補助グループ変更と、それに対応するsupervisor/unixd/filed等の認可。現行setidは0以外EPERM、capgetは全ゼロ。値だけ変える／権限を落とさず成功を返す／system busのuser設定をrootへ変えて済ませる実装はしていない。netd再設計からOS権限モデルへ広がるため、ここでユーザーに範囲の確認を求める。全体の完了・blocked状態への変更はしていない。

### 資格情報受信とsystem busの実行確認（version 6）

- PEERCRED=11をNAME直後、OPTIONS=20をFLAGS直後に挿入し、後続も連番更新（DIAG=37）。SO_PEERCREDをVMO attachから分離し、DGRAM socketpair・listener・未接続にも正しい情報を返す。名前付きDGRAM connectではSO_PEERCREDを新たに作らない。socket optionはbroker所有へ移し、LPRの独立optionsコピーを削除。RXに受信者のoptionsを置き、peerはRO参照する。共有RX layoutとunixd wireが変わるためversion 6とし、unixd/LPR/supervisor/seed0rootを再ビルドして一括配置。kernel/native syscall ABIの変更なし。
- SO_PASSCREDをSTREAM/SEQPACKET/DGRAMへ接続。送信元/受信先が要求したSTREAM/SEQPACKET送信は既存escrow取引のFDゼロ件を用いて認証し、通常の無資格情報転送は直送を維持する。DGRAMは配送記録の認証済み資格情報を使用。SCM_CREDENTIALSはSCM_RIGHTSより先に出力し、小さい制御bufferの部分出力/MSG_CTRUNCも扱う。送信時に取得しなかった資格情報は後付けせず、PID 0・overflow UID/GID 65534。根拠は[Linux unix_maybe_add_creds](https://github.com/torvalds/linux/blob/master/net/unix/af_unix.c)と同一probeのLinux実行。
- STREAMの途中までの受信でも送信者資格情報が残るよう、ticketをrecord末尾まで保持する設計へ修正。FD参照は最初の非PEEK消費で解放し、後続部分はFDゼロ件のmetadataだけを保持。受信ACKを前の部分から引き継がない。PASSCREDのaccept継承、connect/DGRAM send時の未bind socketの自動abstract名、accept前の送信資格情報保存も追加。
- `.artifacts/test-results/unixd-credentials-integration/` は最初のQEMU検査。基本資格情報の検査後に、fixtureが使用した/dev/nullのFD転送がEOPNOTSUPPとなりprobe exit 1。この起動を合格扱いにはしない。system busは/run/dbus不在で失敗した。console/serial/host-time/run/hash保存。
- fixtureを対応済みの通常ファイル/etc/passwdへ変更し、`.artifacts/test-results/unixd-credentials-final/` でQEMU全probe完走。3種類のsocket、制御buffer容量0〜36、PEEK、資格情報+FD、STREAM 5分割受信、forkした送信者PIDとSO_PEERCRED作成時PIDの区別、dup経由のoption変更、listener資格情報、accept継承、自動bindを検証。UNIXD_CREDENTIALS=OK / UNIXD_PAIR_DONE / CREDENTIAL_FINAL_EXIT status=0。harness exit 0、全実行5.9秒。console/serial/host-time/run/hash保存、faultなし。Linuxの同じ検査も成功（`.artifacts/unixd-credentials-linux-probe.log`）。
- 同じ起動で/run/dbusをゲスト内に作成後、標準の`dbus-daemon --system --fork --nopidfile`を実行。今度は`Could not get UID and GID for username "messagebus"`で失敗し、SYSTEM_BUS_EXIT status=1。polkitdの未登録警告もある。これはsystem bus成功ではない。rootfsアカウント生成・起動手順と、現在root以外を拒否するLinuxのID変更/syscall・supervisor認証情報更新の修正が次の対象。資格情報受信の成功を、一般UID/GID権限変更やsystem bus全体の完了と混同しない。
- 高速化/追加性能計測は停止したまま。資格情報変更、system bus、全面競合/死亡/応答消失検証、修正後のXFCE/Writer最終確認は未完了。

### 切断処理の修正（version 5）

- ユーザーの指摘に従い、高速化と追加性能計測は停止。切断・資格情報・system busの正しさを先に修正し、その後にQEMUでXFCE/Writerを最終確認する。以前のbulk計測予定は実行しない。
- AF_UNSPECと別peer再接続の受信キュー/escrow破棄を実装。初回接続と同一peer再接続はキューを保持し、別socketと接続済みの宛先へのconnectはEPERM。共有RXを読取中でもownerを奪わずbroker配送長からcursorを進め、破棄されたHEADのFINISHはESTALE、LPRはhidden importを破棄して再取得。単純な配送ID上限による成功応答を廃し、破棄した配送が後続受信によって成功扱いにならないよう修正。
- FD escrowに「送信成功後に配送が破棄された」内部状態を追加。socket/external参照は破棄時に解放し、送信元ACK前のCOMMIT成功記録は保持。これは受信待ちキューの破棄であり、未完送信のcancelとは区別する。
- Linux比較で、未読配送を捨てた双方向DGRAM接続の旧peerにECONNRESETが必要と確認。保留エラーはbroker所有とし、dup/forkで共有、poll/POLLERR、I/O、SO_ERRORのread-and-clearに接続。根拠はLinux実行と[Linux af_unix.c の unix_dgram_disconnected](https://github.com/torvalds/linux/blob/master/net/unix/af_unix.c)。unixd wireはversion 5、FLAGS直後にERROR=19を挿入し後続を+1（DIAG=35）。ローカルLPRだけのerrorでは別process/dupの状態共有が成立しないための契約変更。kernel/native ABI変更なし。
- `.artifacts/test-results/unixd-disconnect-integration/`: QEMUで従来全probe＋切断/再接続/未読FDの最後の参照解放64回/POLLERR/SO_ERRORのdup共有/エラー取得後の経路再利用を完走。UNIXD_DGRAM_DISCONNECT=OK、UNIXD_PAIR_DONE、DISCONNECT_PROBE_EXIT status=0、5.0秒。console/serial/host-time/run/hash保存、faultなし。Linux参照も同じ検査が成功。読取owner保持中のpurge、送信header改変下の回収、CLAIM後のpurge、外部参照解放と送信再試行はhost ASAN検査で確認（`.artifacts/unixd-disconnect-host-checks.log`）。
- 追加で自己接続切断のエラーなし、満杯で待機中のwriterを切断で起床しECONNRESET、回収後の再送受信をLinux参照と最新QEMUの両方で確認。`.artifacts/test-results/unixd-disconnect-final/` は全probe完走、UNIXD_DGRAM_DISCONNECT=OK / UNIXD_PAIR_DONE / DISCONNECT_FINAL_EXIT status=0、5.2秒、harness exit 0。console/serial/host-time/run/hash保存、faultなし、QEMU終了済み。自己接続へECONNRESETを設定しない修正も配置済み。資格情報、system bus、全面競合/死亡/応答消失検証と最終GUIは未完了。

### 以前のチェックポイント（version 4以下、未完項目の記載は上記が優先）

- unixd契約をversion 4へ更新。受信待ちバイト数を診断RPCやVMO添付に依存せず取得する `PENDING=12` をPOLLの直後へ挿入し、ATTACH以下を+1、DIAG=34に振り直した。DGRAM HEADは受信shutdownかつ空キューを `result=0, delivery.id=0, capなし` で返し、IDが非ゼロのゼロ長パケットと区別する。unixd/LPR/supervisor/seed0rootを再ビルド、rootfsとbootfsを配置。kernel/native syscall ABIの編集なし。
- DGRAM shutdownをbroker所有の方向別状態にした。SHUT_RD後も既存キューは保持し、空キューはblocking受信だけ0、nonblockingはEAGAIN。peer側にはSTREAMのEOFを伝播させない。SHUT_WRまたは宛先SHUT_RDでは関係するTX予約を閉じ、満杯routeで待つwriterもEPIPEへ起床できるようにした。DGRAMはEPIPEでもSIGPIPEを生成しない。FIONREADはbrokerのhead lengthを返す。未bind送信元のrecvfrom/recvmsgはアドレス長0で、バッファを変更しない。
- `.artifacts/test-results/unixd-boundary-integration/`: 実QEMUで従来全probeと新しい6組（未接続/socketpair×SHUT_RD/WR/RDWR）のpending長・poll/RDHUP/HUP・残データ・blocking/nonblocking差・SIGPIPEなし・reader/writer待機起床を完走。`UNIXD_DGRAM_BOUNDARIES=OK` / `UNIXD_PAIR_DONE` / `BOUNDARY_PROBE_EXIT status=0`、5.1秒。console/serial/host-time/run/バイナリ識別値を保存。Linux参照実行 `.artifacts/dgram-boundary-linux-probe.log` でも同じ追加検査が完走した。
- Linux参照の探索記録は `.artifacts/dgram-boundary-reference.c/.log`。AF_UNSPEC切断・別peerへの再接続には未受信キューとescrowの破棄も必要で、単にpeer IDを消すだけでは不十分。現在のLPRはAF_UNSPECをまだ拒否し、brokerの再接続purgeも残る。共有RXで進行中の読み取りと破棄を整合させる必要があり、この点を完了扱いにしない。なおLinuxでsenderの送信bufferまで満杯にした状態の「peer shutdown」による即時起床は探索中timeoutとなったため、共通の待機回帰検査は「writer自身のshutdown」を使用し、相手SHUT_RDのEPIPEは非満杯時に別途検査している。
- 前回追加した `/root/Desktop=@dir` を新規rootfsに配置。GUI前のlsで存在を確認し、初回XFCEの `unixd-boundary-xfce/` で全5アプリ表示PASS、DesktopのG_IS_FILE_INFO assertionなし。Writerの `Unixd shutdown verified` を `/root/unixd-shutdown.odt` に保存→終了→新プロセスで再オープンして本文保持を目視確認。STREAMの64B/1024往復×5もDONE、中央値socketpair139.648µs / pathname141.601µs。ただしこちらの終了通知が遅れ、harnessは600秒の手動期限で `manual-incomplete` / exit 1。全体の成功例にはしない。終了後に作成されたmanual-doneは観測メモだけで、遅れたマウス操作もQMP切断で実行されていない。fault/session-exitなし。rootfsを更新せず `unixd-boundary-xfce-final/` で再起動検証中。
- 再検証 `unixd-boundary-xfce-final/` は正常終了（harness exit 0、ready 17.516秒、全体432.2秒は手動確認時間込み）。rootfs再生成なしの再起動後も先のODT本文が保持され、`Reboot verified` を追記・再保存・終了・新プロセス再オープンして両段落の保持を確認。単一ODTを取り出した `unixd-shutdown.odt` のunzip -tとcontent.xmlも一致。最後のマウスメニュー操作、全5アプリ表示、Desktop assertionなしを確認。console/serial/python/host-time/run/result/バイナリ識別値/screenshots/manual-doneを保存、PAGE FAULT/GENERAL PROTECTION/session-exitなし。`input_check=not-run`は自動入力検査を使用しない設定で、手動の証拠はscreenshots/manual-done。QEMUは終了済み。system bus等の警告、資格情報、切断purge、全面競合/死亡検証は残る。
- 既存 `lpr_futex_pingpong_bench.c` に `--bulk [bytes] [chunk] [trials] [type]` を追加。default 64MiB/16KiB/3回、type=0はSTREAM/SEQPACKET/DGRAM全部。socketpair・2threadの一方向連続転送で、作成・buffer準備を計時外にし、受信側の内容/総量検査と最後の1byte ACKまでを計時。大きなbufferは事前heap確保し、pthread stackへ置かない。ホストLinuxで8MiB+3bytes/16KiB/2回×3種とDONEを確認（`.artifacts/unixd-bulk-linux-reference.log`）。末尾の短いpacketも検査。guest向けビルド成功（`unixd-bulk-guest-build.log`）だが、追加rootfsコピーはせず次回のまとめた配置でQEMU実行する。今回のGUI内のRTT測定は配置済みの従来benchであり、新bulkモードを使った測定ではない。
- version 4でbroker/service wait/rights/service rights/LPR client/wait/mapping/ABI layoutのhost検査も成功（`.artifacts/unixd-boundary-host-checks.log`）。追加したPENDING検査では、送信元が共有headerの長さをUINT32_MAXへ改変してもbroker配送記録の3bytesを返し、非所有sessionの問い合わせをEBADFで拒否する。

- DGRAM のLinux側read/write・readv/writev・send/recv・sendto/recvfrom・sendmsg/recvmsgを `lpr_unix/datagram.c` へ接続。本文は送信元別の方向VMOで直送し、配送FIFO・長さ・位置・送信元・ticketはbrokerのHEAD記録を使用。送信側RW TX/RO RX、受信側RO TX/RW RXを検証してmap。pathname ROUTEはfiledの認可済みinode解決のみを行い、sendtoでconnect状態を変えない。FD送信はroute付きescrow、受信は既存CLAIM/FINISHへ接続。共有ownerの競合はarm/recheck、配送キュー待機は登録→drain→再試行→wait。宛先容量解放時は当該配送元だけでなく全流入routeの送信元を起床する。
- `.artifacts/test-results/unixd-dgram-integration/console.log`: 実QEMUで従来の全probeに加え、DGRAM socketpair・ゼロ長と非EOF・PEEK/TRUNC・iov・ファイルSCM_RIGHTS・abstract/pathname・送信元名の切詰め・sendto後も未接続・poll起床・blocking read・別送信元の容量待機・送信元close後の配送と名前保持・受信timeoutを完走。`UNIXD_DGRAM_PAIR=OK` / `UNIXD_DGRAM_NAMED=OK` / `UNIXD_PAIR_DONE` / `DGRAM_PROBE_EXIT status=0`。5.4秒。console/run/host-time/バイナリ識別値を保存。serialの保存元を誤指定したため、この起動のserialコピーはなく、次のGUI起動で上書きされた。通常Linux上でも `--datagram-only` を実行し同じ追加検査が成功した。
- WAIT_REGISTERはunixd側でもprivate waiter channelごとに256件を強制。既存登録の再試行は上限でも成功し、新規だけEAGAIN、解除後は再利用可。LPR cache容量との静的整合性を追加。service waitの直接要求検査とroute mapの実RO保護・不正権限/サイズ・部分map失敗回収、既存rights検査に成功。kernel/native ABI変更なし。
- DGRAMの残り: shutdown・AF_UNSPEC disconnect・FIONREAD・未bind送信元のrecvfromアドレス長等のLinux境界、資格情報受信、同一route複数reader/writer・途中死亡/制御応答消失の全面実証。route mappingは今は呼出し単位であり、DGRAM自体のRTT/スループット改善は未測定。STREAMの登録再利用の性能結果をDGRAMに流用しない。
- Desktop作成修正を含むalpine_xfceを再ビルドし、今回のLPR/unixd/probeとまとめてrootfsを意図して一度更新。`unixd-dgram-xfce/` の実GUIでは全5アプリ表示成功だがDesktop assertionは再発。`pack/internal/manifests/manifests.go:247` はoverlayの空ディレクトリを除外し、生成manifestにも `/root/Desktop` がなかった。既存 `rootfsDirs` へ追加して `/root/Desktop=@dir` の生成を確認済み。ただしこの起動後の配置定義修正で、追加rootfsコピーはせず次回のまとめた配置で初回起動を検証する。
- 同じ `unixd-dgram-xfce/` でWriter本文入力→ `/root/unixd-datagram.odt` 保存→終了→新プロセスで再オープン、本文保持を目視確認。Writer終了後のSTREAM 64B RTTはsocketpair140.625µs / pathname139.648µs（1024往復×5の試行平均中央値）。前回の約140µsを維持。GUI端末から全probeを再実行しDGRAMを含めDONEまで成功。DGRAM自体の速度を測ったわけではない。
- 上記GUIはmanual-doneで終了しharness exit 0、ready 20.335秒、全体545.3秒（手動操作・確認時間を含む）。console/serial/python/host-time/run/result/バイナリ識別値を保存、PAGE FAULT/GENERAL PROTECTION/session-exitなし。`input_check=not-run` は自動入力検査を無効にした値であり、手動操作の証拠はscreenshotsとmanual-done。ODT本文をguest unzipで照合し、単一文書をbase64で取り出した `unixd-datagram.odt` もhost unzip -t成功。最後のマウスメニュー操作も成功。QEMUは終了済み。

- 待機登録再利用後のGUIも `.artifacts/test-results/unixd-wait-cache-xfce/` で確認完了。全5アプリ表示、Writer編集→保存→終了→新プロセス再オープン、ODT内の本文照合、端末からThunarの`/usr`表示、マウスメニュー操作が成功。PAGE FAULT/GENERAL PROTECTION/session-exitなし。XFCE起動中の64B/1024往復×5の試行平均中央値はsocketpair139.648µs / pathname141.601µs。初回Desktopのassertionとsystem bus警告は未解決で、XFCE全体の正常化は未完了。前回の検証文書はrootfs更新前に一ファイルだけ取り出し `.artifacts/test-results/unixd-writer-document/unixd-writer-verified.odt` に保管、ZIP整合性を確認した。
- 次の確認事項: 256件の登録保持上限は現在LPR側の制御であり、unixdが直接受ける登録要求自体の予算強制は別途必要。DGRAM I/O・資格情報受信・一般UID/GID認可・競合/死亡時の全面検証も引き続き未完了。Desktop作成を追加済みの `tools/build_wsl_alpine_xfce.sh` はまだ再ビルド・新規rootfs初回起動で検証していない。
- 通常blocking I/O・pollのWAIT_REGISTER/WAIT_REMOVEを、thread内の有限な登録再利用へ変更。容量は既存poll graphと同じ256件。active参照は退避不可、I/O終了時は共有slotをdisarmしてローカル参照だけ減らす。idle登録はFD pin/socket ownerを保持しない。close後に同じbroker socket IDをSCM_RIGHTSで再受領するケースは、ローカルsocket状態に一度割り当てる非再利用64bit identityで再登録する。evictionの解除応答消失でも古いcache hitへ戻さない。新kernel編集・native syscall ABI変更なし。LPR内部socket状態の配置変更は同一ビルドの生成・継承・読取で更新し、旧状態との互換は設けない。
- `.artifacts/test-results/unixd-wait-cache-lifetime/` の実QEMUでwarm poll→最終owner close→同socket再受領→遅延送信の起床、重複FD poll、300組の待機登録/close/peer EOF、既存PTY/SCM/poll/epoll/fork/execを完走。wait/context/service waitのhost検査も成功（有限cache、active追出し拒否、重複参照、解除応答消失、broker HUP、fork cleanup等）。通常LPRの同一kernel・4vCPU・64B/1024往復×5で、変更前→後の試行平均中央値はsocketpair579.101→134.765µs、pathname578.125→133.789µs、約77%短縮。旧netd比ではない。証拠は `unixd-wait-cache-before/` / `unixd-wait-cache-after/`。GUIは同配置の `unixd-wait-cache-xfce/` で確認中。
- 最新配置のkernelでも `.artifacts/test-results/unixd-writer-final-vm/` で全5アプリ表示、再起動前に保存したODTの本文保持、追記・再保存・Writer終了・新プロセスでの再オープンが成功。ODTのcontent.xmlでも2段落を確認し、操作後のマウスメニュー展開も確認。XFCE稼働中に64B/1024往復×5を追加実測し、試行平均の中央値はsocketpair 993.164µs / pathname別process 1283.203µs（DONE）。低負荷console測定とは負荷とkernelが異なる。冷起動のWriter表示は遅く、system bus警告も残り、正常化・高速化全体は未完了。通常blocking I/OごとのWAIT_REGISTER/REMOVE RPCを除く作業も残る。
- `.artifacts/test-results/mremap-munmap-policy/` 実QEMUで疎匿名MREMAP成長600回、各回の旧内容/新tail zero保持、fork子の追加成長・書込・munmap、親COW内容保持、ゼロサイズ拒否後の旧内容保持、最終munmapをすべて完走（DONE、exit status=0）。600回move-only・同一PT隣接VMA保持・prepare失敗後refaultは別途未確認。成長修正後に子munmapでも同じsupervisor seed誤拒否が出たため、kernel_reviewの必要性・最終承認を受け、native munmapは全域連続VMA所属をVM transaction内で検証できた場合だけ同じpolicyを使用。pinned拒否・strict fallback・fixed targetは維持。穴ありmunmapの意味論全体をこの修正で解決したとはしない。
- LibreOffice Writer の実操作は `.artifacts/test-results/unixd-writer-manual/` で成功。本文入力→ODT保存→アプリ終了→新しいWriterプロセスで再オープン→本文保持を目視確認し、ODT内content.xmlもゲストから読んで照合した。操作後のデスクトップも確認、実行全体405.8秒でPAGE FAULT/GENERAL PROTECTION/session-exitなし。これはPT回収修正版の実機結果。後述の過去Writer失敗は履歴として残す。後続source lazy-hole許容修正版のGUI再検証、system bus警告解消等の全体正常化はまだ別途必要。
- source lazy-hole誤拒否の修正はkernel_reviewの必要性・最終差分承認済み。VMA所属を検証済みのmremap source preflight/unmapだけ明示wrapperでsupervisor seed PTEをskip。strict unmapやfixed targetは変更しない。kernelをビルド・bootfsだけ更新し、rootfs再コピーなしで実probe検証へ進んだ。
- Writer障害調査に伴い、kernel_review承認後にnative faultの物理確保・VMO登録・PT生成とMREMAP失敗段階を診断。MREMAPでPTEを先に無効化すると最終unmapが空PTを回収しない不具合を修正し配置済み。親PDE切断→全CPU shootdown完了→PT枠再利用の順に変更。これだけでWriter解決とは未確認。
- 実ゲストの疎匿名MREMAP probeは成長2回目でENOMEM。追加診断で `source preflight failed va=0x23000000 size=0x402000` と特定。低位identity PDEから生成したsupervisor PTEが未fault部分に残り、所有済みlazy VMAの解除事前検査に拒否される別問題。fixed move probe初回もInvalidStateで、同一VMAへ統合された隣接領域の扱いを調査中。証拠は `.artifacts/test-results/mremap-stage-diag/` と `mremap-move-reclaim/`。600反復・COW・失敗後保持を通過したとは扱わない。
- ユーザー指定により LibreOffice の編集・保存・再オープンと UNIX socket 往復計測を完了条件に明記。最新の測定・GUI証拠は [netd-unixd-measurements.md](netd-unixd-measurements.md)。64B/1024往復×5、4vCPU/KVM で試行平均の中央値は socketpair 535.156µs / pathname別process 540.039µs。旧netd比の高速化率や飽和スループットはまだ測っていない。
- Writer は一度新規文書画面まで起動したが、保存検査の再実行で X セッションが落ちた。user PAGE FAULT の RIP=0x20062d98 は配置済み musl の __libc_realloc、mremap成功直後に新領域末尾へ書く命令。新VMA=0x407910000+0x796000、fault=end-4。native anonymous VMAの末尾lazy fault解決失敗を調査する。サブエージェント kernel_review は、kernel VM/fault処理の失敗段階診断と直接原因の修正を条件付き承認済み。物理確保/VMO登録/PTE作成を切り分け、原因未確認の事前全割当・定数拡大は不可。この段階では追加kernel編集なし。疎領域成長・COW・失敗atomicityとWriter保存後XFCE生存が受入条件。
- 全5アプリ＋再度の端末/Thunar操作は `.artifacts/test-results/unixd-xfce-pty-all/` で成功。desktop-icons.png に Home/File System を目視確認。Desktop作成済みの再起動ではfile_info assertionなし。初期rootfsにDesktopを用意するbuild設定を追加したが、その変更での新規rootfs初回起動は未確認。

- PTY SCM_RIGHTS を修正し、通常ビルドの QEMU で端末から Thunar を起動、Ctrl+L で `/usr` へ移動できた。`.artifacts/test-results/unixd-xfce-pty/thunar-open.png` と `thunar-usr.png` を目視確認。desktop ready は約33秒。以前の `The connection is closed` は、GApplication.CommandLine が PTY stdin を渡す際に LPR が console 以外を拒否していたことが原因だった（修正前の D-Bus message 記録: `unixd-xfce-terminal-messages/console.log`）。
- 転送記述子の末尾を未使用 reserved から provider_data に再設計。TTY の console/master/slave を保持し、受信時に値を検査する。unixd は provider 固有情報を解釈せず escrow・再送照合に保持し、socket provider ではゼロを要求する。既存の64bit termd handle を狭めず、lease による同一 OFD 保持を維持するための変更。UNIX_SERVICE_VERSION=3、旧版互換なし。opcode・kernel ABI は変更していない。
- `.artifacts/test-results/unixd-pty-rights/` の実ゲストで PTY 両端の転送、元FD close 後の read/write、master ioctl、device番号、CLOEXEC、受信FDの再転送が成功（UNIXD_PTY_RIGHTS=OK）。既存 STREAM/SEQPACKET/path/SCM/socket transfer/poll/fork/exec も同じ実行で成功。LPR の診断用 PID 固定条件を除き、通常ビルドへ戻して配置した。
- XFCE 全体は引き続き未完了。デスクトップアイコンの `G_IS_FILE_INFO(file_info)` assertion、system bus 不在の警告が残る。上記の端末・Thunar 操作成功を、アイコンや全アプリの正常化の代用にはしない。DGRAM I/O、資格情報受信、障害・競合検証、速度実測も残る。

- 追加の対話確認 `.artifacts/test-results/unixd-xfce-interactive-v2/`: メニュー操作・端末上のpwd応答は再確認できたが、端末から `thunar` を実行すると `The connection is closed` で終了（thunar-open.png）。続くCtrl+Lは端末に入り、`/usr`も端末で実行されたため、Thunarのディレクトリ移動成功とは扱わない。自動検査のmapped-window PASSと対話起動の失敗を区別する。次は対話端末のsession bus環境と接続認証・継承経路を確認する。
- 同じ配置を再起動した `.artifacts/test-results/unixd-xfce-interactive/` では、検査スクリプトの `/root/.xfce-app-acceptance` 削除が `Directory not empty` で失敗し、XFCE起動前に終了した。検査ハーネスは今はready以外やapplication-failureを終了コード1にする。次の実行では既存検査データをゲスト内 `/root/.xfce-app-acceptance-dynamic-fd` にrenameして保持し、rootfs再生成せず継続した。削除・ディレクトリ列挙の問題自体は未修正。元へ戻せる退避であり削除していない。

- 汎用 native FD テーブルを初期256・必要時に最大4096まで拡張する実装を追加。FD操作群の先頭32に `FD_TABLE(minimum_capacity, info)` を挿入し、旧32..75は33..76へ一括改番。旧ABI互換なし。0は照会、非0は最低容量の確保で縮小しない。出力はcapacity/maximum/free_slotsの3個のu64。free_slotsは16以降の空き数の観測であり予約ではない。userlandだけではkernel所有のcapability tableを拡張できず、正当なlive FDで256枠に到達した実測が根拠。サブエージェントの必要性・最終差分レビュー承認済み。poll件数などの上限は拡大しない。
- growは割当・copy後に切替、inlineの旧参照を消去。fork/createは継承前に子の容量確保。高FDのlookup/IPC/scan/CLOEXEC/revoke/終了を更新。終了時は各FDをVM transaction内でcloseし、解放後にwakeする。metadata返却時のfree-range枯渇では領域を保持して後で回収する。unixdとsupervisorのadmissionは全FDへのGET_INFOループを容量照会・確保に変更。
- 新ABIでkernelとnative userlandをビルド・配置し、`.artifacts/test-results/unixd-dynamic-fd-pair/` のQEMUでSTREAM/SEQPACKET・path・SCM_RIGHTS・poll/epoll・fork/exec一式成功。`.artifacts/test-results/unixd-xfce-dynamic-fd/` では約33秒でdesktop ready、unixdの256→512拡張、thumbnailer起動成功、Thunar/Pine2/about/GTK demo/terminalの全5表示検査成功を確認。mouse-menu.pngでマウスによるApplicationsメニュー展開、screenshot.pngでCtrl+Alt+T起動の端末上の `pwd` と `/` の返答・次のpromptを目視確認した（自動result.jsonのinput_checkは推測で成功に書き換えずattempted-unverifiedのまま）。今回のログにFD admission失敗・No file descriptors available・wait capacity exhaustedなし。system bus不在とxfdesktopのfile_info assertionは残り、正常化全体は未完了。hostのgrow/fork/CLOEXEC/OOM/metadata返却失敗・descriptor再利用後回収検査、service rights/wait、LPR mapping/wait検査も成功したが、実機検証の代替とはしない。

- 2 VMO 配置の実 QEMU: `.artifacts/test-results/unixd-two-vmo-pair/` で接続・poll・SCM_RIGHTS・fork/exec 一式成功。`.artifacts/test-results/unixd-xfce-two-vmo-v2/` では約31秒で DESKTOP_READY、terminal の mapped window 検査が PASS。壁紙・アイコン・上下パネルを画像でも確認した。一方、同時起動サービスが増えると VMO=102/channel=77/thread=38 等で再び FD admission に到達。パネル子プロセス・thumbnailer に失敗が残る。QMP のマウス／Ctrl+Alt+T／pwd を投入したが、最終画像に端末・コマンド結果はなく、入力操作成功とは扱わない。XFCE 正常化、Thunar 操作は未完了。
- サブエージェントは、正当な live FD が残ることを条件に汎用のプロセス単位 FD テーブル動的拡張を承認済み。ただし定数増加・unixd 専用例外・wait 個数などの一括拡大は不承認。今回は kernel 未編集。まず2 VMO化を優先した。kernel に進む前に不要保持・live 参照の再確認が必要。

- XFCE の実起動を開始し、画面の「Error sending credentials: Not supported」から SCM_CREDENTIALS の送信未対応を特定。LPR が cmsg を解釈し、unixd の認証済み世代と照合する既存 PREPARE/COMMIT に接続した。SO_PASSCRED・受信側資格情報付加の全面対応ではない。
- QEMU で通知キューの空状態が `IPC_RECV → EMPTY` になるのに、LPR が NOT_READY だけを空扱いしていた不具合を特定・修正。以前の wait unit の模擬値も実際と違っており修正した。修正後、XFCE の壁紙・上下パネル・GTK アプリを画面で確認。証拠 `.artifacts/test-results/unixd-xfce-waitfix/`。Thunar/pine2 起動確認は失敗し、正常化は未完了。
- XFCE の FD 不足は unixd の実測で free=20、VMO=136、channel=52、thread=29。固定 256 FD 枠に約34接続で到達。書込所有者が同じ outgoing TX と incoming RX を `unix_endpoint` にまとめ、STREAM/SEQPACKET を接続あたり2 VMO・ATTACH/HANDOFF/CLAIM 2 caps に変更中。own は RW、peer は RO、resize/revoke 権限は渡さない。DGRAM は従来の方向別領域のまま。サブエージェントの安全性レビュー済み。kernel 変更なし。旧4cap互換は維持せず UNIX_SERVICE_VERSION=2 に変更、起動側・supervisor・unixd・LPR を揃えて配置する。新配置の QEMU 確認は継続中。

- UNIX OFD の fork/exec 継承を追加。HANDOFF を RETAIN の直後の opcode 14 に挿入し後続を +1（DIAG=33）、旧番号互換は残さない。unixd の専用 grant channel が一時的に socket 参照を保持する。grant channel で宛先 session を認可した後、その session 自身の認証済み窓口で確定する 2 段階なので、数値 session/grant ID だけでは import できない。確認の再送では参照を重複加算しない。grant close/HUP で一時参照を解放し、未実行 fork/exec の rollback に既存 prepared-lease cleanup を利用する。
- manifest record は process token、mapping pointer、方向 VMO FD、lock state をすべて除いた状態と grant FD のみ。fork 子ではコピーされた古い mapping を解放し、新 session の初回利用時に grant を消費して再 ATTACH。exec も supervisor ACTIVATE 後の初回利用で取り込む。利用せず close した場合は grant のみを閉じ、子が取得していない親参照を CLOSE しない。方向 mapping 失敗後は所有参照を保持して再試行可能。別参照で接続された未 mapping socket の I/O／poll も lazy ATTACH を行う。
- QEMU で STREAM/SEQPACKET の fork 直後の双方向通信、子の exec、CLOEXEC の閉鎖と dup の寿命、exec 後の親子間通信、親が fork 後に作った通常ファイル／UNIX socket を SCM で子へ転送、waitpid・最後の EOF を確認。既存の PAIR/NAMED/PATHNAME/MESSAGE_IO/RIGHTS_IO/SOCKET_RIGHTS/POLL_EPOLL も同じ実行で成功。証拠: `.artifacts/test-results/unixd-handoff.C1ckFr/`。初回の manifest capability 領域の過小計数はゲストで検出して修正した。
- **継承の残り**: 多 thread の同時 FD 変更と fork snapshot の完全な整合性、非公開 SCM 予約の thread death/fork 回収、exec 最終失敗時の既存 FD table 全体の復元、grant 確定 reply 喪失を含む障害注入検証は未完了。option/timeout の完全共有、DGRAM I/O、XFCE 正常化も残る。kernel は今回編集していない。以下の「manifest が UNIX を拒否」は履歴。

- UNIX socket 自体の SCM_RIGHTS を接続。APPEND 成功まで送信元 Linux OFD を pin し、broker が参照を持ってから解除する。CLAIM は socket を専用の 1-item segment として返し、受信 record の reservation を検証してから attachment metadata と 4 方向 VMO を渡す。listener／未接続／DGRAM は generation=0・方向 cap なし。FINISH より前の rollback は mapping と非公開 FD 予約だけを解放し、まだ所有していない broker 参照を CLOSE しない。FINISH 後に process token を設定して公開する。数値 FD やローカル pointer の移送ではない。
- wire に `unix_attachment`（listening を含む 80 bytes）を明示追加し `unix_control` は 1224→1304 bytes、diagnostic offset は 1056→1136。旧 layout の互換は残さず unixd／LPR／supervisor を同時ビルド・配置した。opcode 16 は一方向 SET_FLAGS から query/set の FLAGS 契約へ変更。同じ OFD の O_NONBLOCK は connected socket では既存 shared TX flags、未接続では broker を参照し、F_GETFL/F_SETFL/FIONBIO/I/O/connect/accept に反映する。
- QEMU: STREAM/SEQPACKET の socket FD を送り元 FD を close、queued data の読出しと双方向通信、PEEK の別 FD、FD_CLOEXEC の独立性、O_NONBLOCK の共有変更、epoll、再転送と peer close 後の ATTACH・残データ・EOF を確認。file/socket/file の混在 SCM segment、listener close 後の転送 listener の SO_ACCEPTCONN・connect・accept4・通信も成功。既存 RIGHTS_IO/PATHNAME/MESSAGE_IO/POLL_EPOLL/DONE を含む同一実行の証拠: `.artifacts/test-results/unixd-socket-rights.OTXcvE/`。
- focused service rights／broker／mapping 検査と ABI layout 検査も成功。kernel 追加編集なし。**残る制約**: socket option/timeout はまだ backend-local で、SCM の完全な共有／継承になっていない。転送済み未接続 socket の別参照で connect した場合の lazy ATTACH、listener state 変更の別参照への反映、一般的な rights attenuation、thread death／応答喪失時の import 回復も残る。DGRAM は作成・OFD 移送の経路のみで I/O は未接続。fork/exec は現状の manifest が UNIX backend を拒否しており、所有参照の継承と pointer-free record の統合が次の対象。XFCE 正常化は未検証。
- 以下の SCM 記録の「UNIX socket 未接続」はこの変更前の履歴。

- `sendmsg/recvmsg` の SCM_RIGHTS を unixd の PREPARE/APPEND/COMMIT/CLAIM/FINISH/ACK に接続。通常 payload は shared ring に直接コピーし、権利付き record の公開／消費だけ broker が確定する。既存 filed／console TTY／DRM／input／sync-file provider を新しい `unix_transfer_item` へ移行し、旧 netd UNIX socket provider を削除。LPR FD table は受信資源を非公開予約（active=2）し、FINISH 成功後に一括公開する。pin/close/dup/flags 操作と backend lookup は予約中 FD を利用できない。失敗時は予約と native capability を回収する。
- QEMU で STREAM と SEQPACKET の通常ファイル FD 転送を実行。送信元 close/unlink 後の読出し、MSG_PEEK の独立 FD、CMSG_CLOEXEC、20 FD の分割転送、制御バッファ不足の prefix/CTRUNC、control なし recvmsg／通常 read の FD 破棄、途中に無効 FD がある sendmsg の取消し・payload 非公開、各 type 40 回の反復を確認。既存 PAIR/NAMED/BACKLOG/PATHNAME/MESSAGE_IO/POLL_EPOLL/DONE も成功。初成功証拠: `.artifacts/test-results/unixd-rights.DQYmal/`。
- netd の転送 occurrence／SCM 配列・transaction/count・UNIX notify_ack を wire から除去。`netd_io.data` の offset を詰め、旧サイズ互換は残さず netd と LPR を同時再配置する。LPR の旧 AF_UNIX 通知・poll キャッシュ・SEQPACKET 分岐も削除。INET/NETLINK の exec に必要な netd OFD lease は残す（UNIX SCM には使わない）。kernel の追加編集なし。
  - 削除後も QEMU を再実行し、同じ RIGHTS_IO および既存チェックが成功。最新証拠: `.artifacts/test-results/unixd-rights-cleanup.U6zKye/`（console/serial/実行ログと配置 ELF SHA-256）。ABI layout 検査と diff check も成功。
- **SCM の残り**: UNIX socket 自身、pipe/event/DMABUF 等の未接続 provider、SCM_CREDENTIALS/PASSCRED、受信途中の thread 死亡／fork/exec と非公開予約の回収、応答喪失時の確定・ACK 回復、一般権限検証の強化が必要。未接続 provider の送信は EOPNOTSUPP。既存接続 provider のうち今回 QEMU で実証したのは filed の通常ファイルのみ。異なる process 間、共有 offset/status flags、peer death／資源枯渇等の網羅的な実証や XFCE 正常化は未完了。
- 以下の message/option 記録の「SCM 未接続」は今回の変更前の履歴。message/option 時点の証拠は `.artifacts/test-results/unixd-message.k9dEQd/` に保存済み。

- readv/writev と control なし sendmsg/recvmsg を新 UNIX backend へ接続。iovec は一度 snapshot・全範囲検査してから、一つの transport reservation の spans へ scatter/gather する。SEQPACKET の iovec を別々の packet にしない。MSG_PEEK/MSG_TRUNC、ゼロ長 packet、無効な後続 iovec で何も公開しないことを QEMU で確認した。LPR の旧 netd UNIX SCM send/receive 実装・補助関数を削除。新 SCM_RIGHTS adapter はまだ未接続で、control 付き送信や escrow 付き受信は明示的に拒否する。
- FIONREAD は共有 ring の未読 payload を集計し、FIONBIO と汎用 fcntl も新 backend へ接続。SO_PEERCRED は認証済み ATTACH の snapshot、SO_TYPE/DOMAIN/PROTOCOL/ERROR/ACCEPTCONN・固定容量の SNDBUF/RCVBUF 取得、REUSEADDR/KEEPALIVE、送受信 timeout を追加。timeout は absolute deadline として I/O／connect／accept の再試行を通して保持し、accepted socket には listener の option と timeout を引き継ぐ。
- QEMU の同一実行で PAIR/NAMED/BACKLOG/PATHNAME/MESSAGE_IO/POLL_EPOLL と DONE を確認。MESSAGE_IO は writev/readv、複数 STREAM record の FIONREAD、FIONBIO と fcntl の実際の NONBLOCK 切替、peer PID/UID/GID、20 ms の受信 timeout、SEQPACKET の scatter/gather・PEEK/TRUNC・ゼロ packet・無効 vector の非公開を含む。最初の実行では fcntl 入口の UNIX backend 判別漏れを検出し修正した。
- **message/option の未完了範囲**: SCM_RIGHTS/CREDENTIALS/PASSCRED、MSG_WAITALL、DGRAM I/O、SEQPACKET recvmsg の source address、buffer resize／linger 等の option、recvmmsg の timeout/zero-packet semantics は残る。connect/accept/send timeout と option 継承は実装済みだが、この回のゲスト検証は受信 timeout のみ。非対応 option を成功扱いにはしていない。fork/exec、制御 RPC の障害時回復、一般資格情報、全 ET 競合、XFCE 正常化もまだ未完了。

- pathname の bind/connect を接続。seed0root が filed↔unixd の専用 channel を作成し、公開 VFS endpoint には新 opcode を追加しない。宛先で filed は RECV/WAIT/POLL/INSPECT/CLOSE、unixd は CALL/INSPECT/CLOSE へ減権し、PRIVATE|CLOEXEC にする。LPR は inode を自己申告できず、unixd は filed が返す mount ID と inode 番号だけを採用する。filed は bind 中の inode を内部 open-file 参照で保持し、broker の socket 最終回収で解放。公開 VFS handle を残さず、保持 token は専用 channel 内だけで扱うため、公開 CLOSE 等による番号指定で保持参照を操作できない。unixd の channel が閉じた場合も全保持参照を回収する。
- 専用窓口のため private bootstrap を変更: storage_filed_bootstrap は control_fd の直後へ unix_path_fd を挿入し 672→680 bytes、unix_boot_config は admin_endpoint の直後へ filed_path_channel を挿入し 32→40 bytes。seed0root/filed/unixd を同時ビルド・配置。旧配置・旧サイズの互換維持はしない。kernel の追加編集なし。
- QEMU: `/tmp` の socket inode 作成、hard link 経由の同一 socket への接続、両方の名前を unlink した後の既存通信、同名を別 inode で再 bind・別 listener へ接続、close 後の残存 socket ファイルへの ECONNREFUSED と再 bind の EADDRINUSE、相対パス bind/connect を確認。`UNIXD_PATHNAME=OK` に加え既存 PAIR/NAMED/BACKLOG/POLL_EPOLL/DONE も同一実行で成功。
- 初回の pathname QEMU は制御 VMO を渡す権限指定不足で EPROTO。INSPECT/CLOSE/MAP_READ/MAP_WRITE を明示して修正した。宛先での減権後・内部 open-file 保持へ変更後も QEMU を再実行して上記 marker を確認した。unit test のみで pathname を完了扱いにしていない。
  - 最新証拠: `.artifacts/test-results/unixd-path-private.tSS5xW/`（console/serial/実行ログ・配置済み ELF の SHA-256）。前段の pathname 初成功は `.artifacts/test-results/unixd-path.7AlPbx/`。profile は console-shell のまま。XFCE は未検証。
- **pathname の制限・未検証点**: 現 supervisor の root 資格情報領域のみを受理し、非 root session は明示的に拒否する。一般 UID/GID の DAC、pathname 操作と cwd/close/資格情報変更との競合、filed 障害・応答消失、資源枯渇時の完全な回収は未実証。filed への制御 RPC は現在同期であり、他 daemon との制御呼出し循環・待ち停滞の検証も必要。DGRAM_ROUTE の pathname は未接続。以下の「pathname は EACCES」の記述は以前の段階の履歴。

- abstract 名の bind/connect/listen/accept・getsockname/getpeername を新 backend へ接続し、LPR の旧名前付き netd RPC を削除。内部 NUL を含む長さ付き名前、出力切詰め、blocking accept、backlog 満杯での EAGAIN／accept 後の接続再開／listener close 後の ECONNREFUSED を QEMU で確認。証拠: `.artifacts/test-results/unixd-named.xVAnqc/`。pathname は filed の認可・inode 保持が未接続のため EACCES のまま。
- netd から SOCKETPAIR/LISTEN/ACCEPT/ATTACH_WAIT/UNIX_NAME と AF_UNIX 専用型を削除。残る opcode は HELLO=0/PAGE_ATTACH=1/SOCKET=2/CONNECT=3/CLOSE=4/SEND=5/RECV=6/POLL=7/BIND=8/UEVENT_PUBLISH=9/DUP=10 に連続で振り直した。理由は UNIX の接続・名前・待機状態を netd が保持しなくなったため。旧 wire 互換は残さず、netd/LPR/inputd/drmd を再ビルド。旧 UNIX 専用 FD-budget テストも削除（Git から復元可能）。
- `lpr_unix/poll.c` を既存 wait graph と poll/select/epoll へ接続。接続済み STREAM/SEQPACKET の readiness は共有領域から読み、listener の pending は unixd に問い合わせる。UNIX_OP_POLL=11 を NAME の直後へ置き、ATTACH 以下をすべて +1 にした（DIAG=32）。接続制御側の readiness を診断 RPC に依存させないため。graph 構築中は資源を持たず、block 中だけ FD pin・watch・両方向の shared slot を保持し、drain→arm→再確認→wait→全解除を行う。epoll の既報 ET readiness は再確認から除き、常時 ready による空回りを防ぐ。
- 同一 QEMU 実行で `UNIXD_PAIR_BLOCKING/STREAM/SEQPACKET`、`UNIXD_NAMED_CONNECT`、`UNIXD_BACKLOG_WAKE`、`UNIXD_POLL_EPOLL` の OK と DONE を確認。追加対象は listener poll、poll timeout、遅延送信での poll/select/epoll 起床、ET の未消費データの重複報告抑制と drain/refill、RDHUP、events=0 でも peer close の HUP。これは ET の全競合条件やプロセス間継承の証明ではない。設計上の ready 遷移世代による全 ET 統合・複数待機者・SCM 等は残る。
- poll probe 初回は ELF 起動前に失敗。実 disk inode のサイズが配置元より末尾 8 bytes 短いことを read-only で特定し、32 MiB の独立 ext4 でも debugfs 1.47 の trailing sparse-hole 切詰めを再現した。`pack/internal/rootsync/ext4.go` で write 後に元サイズを明示し、抽出ファイルとの完全一致と fsck を確認。修正後の rootfs を意図して一度反映した後に上記 QEMU が成功。rootfs 全体を診断用にコピーしていない。

以下の箇条書きは直前の socketpair 段階の履歴。現在の統合範囲は上記を優先する。

最新 QEMU 証拠: `.artifacts/test-results/unixd-poll.jFlcnv/`（console/serial/実行ログ・配置済み ELF の SHA-256）。配置不具合の診断ログは `.artifacts/test-results/unixd-poll-failed.llVa6Y/`。`go test ./internal/rootsync -run TestExt4SparseTailPreservesEOF -count=1` は本物の mkfs/debugfs で sparse EOF の保持を確認。現在の boot profile は引き続き console-shell で、XFCE は未検証。

- `netd/src/unix_socket.c/.h` と service dispatch／main の通知・待機監視を削除。CMake 入力からも除去し、netd build 成功。旧実装専用の netd-unix-socket／lpr-linux-socket-transfer ホストテストを削除（Git から復元可能）。netlink と TCP/IP は今回の削除に含めない。旧 UNIX wire 定義と LPR の名前付き操作／転送の残存コードは削除・再接続途中。
- `lpr_unix/socket.c` を Linux FD 管理へ接続。専用 backend ID UNIX=9 を SOCKET の直後に入れ、EPOLL=10／DMABUF=11／SYNC_FILE=12 に振り直した。netd handle と unixd socket 所有者を区別するためで、旧 provider ID の互換維持はしない。作成は FD batch install、read/write は pin の生存中に直接 spans copy、close は mapping と broker 所有参照を回収する。send/recv・shutdown の入口も接続。packet の blocking 送信は指定長に必要な空きを再確認する。
- QEMU 実行: `.artifacts/bin/pacgo qemu-test --timeout 90s --send /cmd/lpr_unixd_pair_probe.elf --expect UNIXD_PAIR_DONE` 成功。`UNIXD_PAIR_BLOCKING=OK`／`STREAM=OK`／`SEQPACKET=OK`／`DONE` を console で確認。実際の Linux socketpair・pthread からの遅延送信・blocking read の起床・dup 後の close・send/recv PEEK・shutdown EOF・100 回の nonblocking SEQPACKET 作成／ゼロ長 packet／破棄が対象。native capability／VMO／unixd dispatch を mock に置き換えていない。
- unixd／supervisor／seed0root／termd／netd／LPR／probe を限定して pack staging へ反映し、rootfs は `--no-build` で一度生成・配置、bootfs も更新した。最初の rootfs sync は新 probe の staging 不足で manifest 検査時に停止し、ディスクを書き換えていない。QEMU の自動 profile 切替による繰返しコピーを避けるため、`.artifacts/seed0root_boot_profile.txt` は現在 `console-shell`。XFCE 検証前に通常 profile へ戻して反映すること。
- **新 backend の DGRAM route I/O・readv/writev・名前付き接続・SCM・socket options・poll/epoll・fork/exec は未統合**。DGRAM/SCM の未対応経路は明示的に拒否し、escrow を黙って捨てない。非 CLOEXEC UNIX FD を保持した fork/exec もまだ完了扱いにしない。XFCE は未確認。この節より下の「ゲスト未実行」「rootfs 未更新」は各部品を追加した当時の履歴であり、全体の現状ではない。

## 入っている実装

- `userland/unixd/`: NIC/libuinet に依存しない native daemon の土台。
  - STREAM/SEQPACKET の方向別 TX/RX VMO、直接コピー用 spans、atomic 公開／消費、部分読み・PEEK・shutdown・死亡 owner 回復。
  - broker の socket、pair、bind、listen/connect/accept、所有参照、資格情報 snapshot。
  - DGRAM は送信元／宛先ごとの VMO と broker 配送順序・容量管理。受信長／位置／資格情報は broker 記録を信用し、sender 書込可能な record header は信用しない。
  - native 制御口、admin 経由 session 登録、thread 観測 FD 登録と死亡処理、VMO の権限を減らした attachment。
  - 所有 socket の DIAG: 参照数、接続相手、backlog、配送数・容量、head の操作 ID、方向別 owner／公開／消費位置。共有領域は独立した atomic sample であり一貫した snapshot ではない。
  - native FD 受信余力を確保前に確認。
  - 方向別管理ページに 64 slot の waiter bank、SC の公開／消費・変化番号、通知 ID＋待機世代による登録／解除。逆側 mapping の RO は維持。
  - native service の WAIT_REGISTER/SYNC/NOTIFY/REMOVE。受信口は PRIVATE|CLOEXEC・dup/transfer 不可、通知先取得は所有 socket と相手関係で認可。thread 死亡・最後の所有参照 close に加え、受信口の HANGUP でも登録を回収する。これにより、生存 thread の exec 後にも旧待機登録を残さない。普通の socket dup close は登録を残す。
  - `notify_client.c` は更新 thread ごとの有限 capability cache。warm path は直接 SEND、cap 確保失敗は制御 RPC で通知委譲。SEND 用 cap も PRIVATE|CLOEXEC。native FD なし SEND の queue-full は ALLOC であるため、既存通知ありとして扱う。LPR 送信側 adapter は追加済み（下記）、Linux I/O 呼出元との接続は未完了。
  - `rights.c` に STREAM/SEQPACKET/DGRAM の FD escrow を分離。準備→共有 ring の公開とともに queued→受信側所有参照と cursor を確定、という broker 内の処理を追加。socket の保持参照と service-owned 外部 provider 参照を扱う。
  - DGRAM の ticket／操作 ID は broker の配送記録に保持する。FD 付き送信も宛先全体の容量・FIFO に従い、sender の共有ヘッダ書換えでは別の配送に結び替えられない。通常の DGRAM_CONSUME は FD 付き head を拒否し、FINISH の prefix=0 で明示的に破棄する。
  - `escrow.c` に外部 provider の native capability 保持・参照数・解放を追加。実際の FD 権限で TRANSFER を確認し、descriptor の数値だけで権限を作らない。capture 途中失敗では未採用 FD を呼出側に残し、採用分は呼出側の rollback で閉じる。
  - service の RIGHTS_PREPARE/APPEND/COMMIT/CLAIM/FINISH/ACK/CANCEL を接続。最大 16 descriptor ごと、native reply の FD 上限に収まる単位で分割する。APPEND 再試行は最初に受理した参照を再利用し、追加受信した capability は RPC cleanup で閉じる。CLAIM は複製用 capability を返すだけで、受信 socket の所有権は FINISH まで渡さない。**LPR の provider import と実 IPC による end-to-end 転送は未接続・未検証**。
  - 準備中は操作が参照を保持し、queued は受信 socket から FD への辺とする。socket close/session 死亡では所有者から mark/sweep して循環を回収する。peer や名前は root にしない。listener の pending 子も扱う。
  - 受信 prefix の所有権確定、余剰破棄、PEEK の独立参照、完了操作の再試行と ACK を追加。thread ごとに受信結果を ACK まで保持し、操作番号の watermark で回収後の古い再試行を拒否する。死亡 thread の完了受信は同一 process の回復処理が確認できるよう ACK まで残す。
- kernel の不足は、ユーザー指定の subagent `/root/kernel_review` に必要性・範囲を審査させた。最終差分は承認済み。
  - `cloneFdTableForFork`: private FD を除外。親の認証 capability を子へ継承させない。
  - `FD_DUP(PACHA_THREAD_SELF_FD, ...)`: 現在 thread の世代に結び付く観測 FD。inspect/wait/poll/transfer/close 以外の要求と未知 flags は拒否。新 syscall 番号は増やさない。
  - `thread_kill`: self-kill を拒否。remote release 失敗は NOT_READY で状態を変えない。成功した世代の全 thread object に terminal を反映する。
  - `thread_exit`: scheduler 解放成功後の callback で clear_tid → terminal → wake。AP 側も解放失敗時に callback を呼ばない。
  - task-FD の wake は terminal object のみに限定し、生存 sibling／process を起こさない。
- LPR fork 子の最初の native FD 確保で空の event を作り、private 除外で空いた bootstrap 固定 FD 245 を予約する。元 manifest の権限は引き継がず、exec の backup/restore に必要な管理権限のみ持つ。
- 起動経路に unixd を接続。seed0root は filed 起動後、device handoff／NIC 探索より前に `/srv/unixd.elf` と supervisor を起動する。unixd ready を確認し、管理口は supervisor の専用 bootstrap FD にのみ渡す。filed のサービス endpoint 登録や Linux manifest には追加しない。
  - supervisor は受け取った管理口を CALL/INSPECT/CLOSE、private に減権し、元の DUP/TRANSFER/SET_FLAGS 付き bootstrap FD を閉じて HELLO を確認する。認証済み process control からの要求で unixd session を発行する経路を追加した（下記）。
  - `unixd/client.c` は native 制御 RPC。要求／応答 ID・wire・status の照合、capability 数制限、失敗時の受信 FD・page・reply の解放を扱う。通常データ経路には使わない。輸送失敗が操作の未実行を保証するわけではない。
  - `pack/pack.yaml` に unixd の build／`/srv/unixd.elf` 配置定義と、seed0root／supervisor の依存入力を追加。**配置定義のみであり rootfs 更新・ゲスト起動はしていない**。後段の旧 netd／drmd／inputd はまだ NIC に依存するため、NIC 不在で Linux 起動全体が完走する状態ではない。
- supervisor のプロセス専用制御 channel を追加。呼出元は実際に受信した channel で決め、数値 token は対象の識別子としてのみ使う。共有管理口から通常の process 操作は拒否する。要求 page は認可前に snapshot し、共有 payload の書換えによる認可と操作対象の不一致を防ぐ。
  - 初期起動／fork は one-shot bootstrap を渡し、子の native process FD 登録後に ACTIVATE する。子が先に走った場合は登録まで activation CALL を保留する。最終制御 cap は CALL/INSPECT/CLOSE のみ、PRIVATE|CLOEXEC、dup/transfer/flags 変更不可。親の制御 cap は fork で継承しない。
  - exec は現在の制御口で EXEC_PREPARE し、返された private・非 CLOEXEC の handoff FD を manifest の `supervisor_bootstrap_fd` に記録する。native exec が現在の FD table を保持する既存契約を利用し、filed への制御 cap 移送や新たな kernel 変更は行わない。次の image はこの FD で ACTIVATE し、消費した動的 bootstrap FD を閉じる。
  - 現在の制御 client FD は native exec 成功時だけ CLOEXEC で閉じる。supervisor 側の旧 server は次の ACTIVATE で閉じる。準備／staging／commit 失敗時は所有する exec reservation だけを transaction cleanup で取り消し、旧制御口は維持する。fork transaction の cleanup は exec を取り消さない。
  - **初期起動／fork／exec の実 IPC・ゲスト動作は未検証**。また、この制御口 cleanup は exec 全体の原子性を保証しない。既存の `lpr_close_local_state_before_self_exec` と replacement lease の所有権移行が最終 native exec より前にあるため、その syscall 失敗時の Linux FD／cwd の復元は別途必要。RPC の応答喪失時に実行済み PREPARE を確定／回収する経路も未実装。
- supervisor の PROCESS_UNIX_SESSION と LPR の取得 client を追加。呼出元専用 channel と一致する token だけを受理し、共有管理口・bootstrap・他の process token からの取得は拒否する。native process 登録前・終了後も発行しない。
  - PID と資格情報を supervisor registry から unixd の PROCESS_REGISTER に渡す。現行 LPR は UID/GID=0 固定、非 0 の set*id を拒否しているため、初期登録はその仕様と一致させる。fork は registry をコピーし、子 PID・資格情報世代を新しくする。**set*id による資格情報変更・失効は未実装で、これを最終仕様にはしない**。
  - LPR へ渡す unixd cap は CALL/INSPECT/CLOSE/WAIT/POLL のみ、PRIVATE|CLOEXEC。WAIT/POLL は通知 SEND cap が別 process に残っていても unixd 自体の終了を検知するために追加した。RECV/DUP/TRANSFER/SET_FLAGS は与えず、通常の supervisor 制御 cap は変更しない。supervisor が元 endpoint を保持し、再取得・self-exec で session/socket 所有者を変えない。exec の image 世代と、資格情報が変わった場合の世代は別に扱う。終了を観測した時点で保持 endpoint を閉じ、zombie の wait4 まで UNIX 参照を残さない。
  - 取得 client は返された session ID と実際の FD kind／rights／flags を検査する。不正 reply cap は閉じる。**AF_UNIX backend からの呼出し、fork 時の socket 所有参照の登録は未接続**。現在の syscall 経路は依然として旧 netd を使う。
- `runtime/lpr_unix/client.c` に LPR 用制御 RPC adapter を追加。認証済み supervisor 経由の session 取得、native VMO／IPC、reply waiter の signal 中断後の再待機、FD／mapping cleanup を接続した。client は取得時の process token を保持し、fork でコピーされた親の private FD 番号で CALL／CLOSE しない。
  - `unixd/src/client_wire.c` の応答照合・capability 個数制限・失敗 cleanup は native daemon と LPR で共有する。違いは page 確保／解放、CALL、RECV、CLOSE の syscall wrapper のみ。これは制御 RPC の実装であり、通常データを daemon 経由にするものではない。
  - LPR build と pack の入力依存にも追加。**Linux FD/OFD へのインストール、Linux send/recv はまだ未接続**。この段階の socketpair テストは下記 loopback fixture であり、Linux socketpair syscall を切り替えたわけではない。
- `runtime/lpr_unix/mapping.c` に STREAM/SEQPACKET の ATTACH→4 VMO mapping→解放を追加。FD の数・重複・kind・正確な rights／size／flags、応答の type／socket、共有 header の magic／generation／capacity を検査する。送信 TX・受信 RX だけ RW、送信 RX・受信 TX は RO。過剰な権限も拒否する。途中 map 失敗は作成済み mapping と全受信 FD を解放する。
  - peer ID=0 は peer close 後の正常な状態として受理し、残データと EOF を取得できる。mapping の破棄は broker socket 所有参照の CLOSE ではない。exec/SCM の転送 record にローカル仮想アドレスを保存しない。
  - transport／notify core を LPR build に接続。mapped spans を使う直接コピーはホストで確認済みだが、Linux send/recv と wait/notify には未接続。DGRAM は別の route attachment が必要で、この mapping API は明示的に拒否する。
- LPR unixd client に現在 thread の観測 FD 登録を追加。承認済みの native FD_DUP(THREAD_SELF) を使用し、source は観測＋TRANSFER のみ PRIVATE|CLOEXEC、unixd に渡す権限は観測のみ（kill／set-context／dup／transfer を与えない）。返された owner を検査し、成功／失敗の両方でローカル観測 FD を閉じる。thread ごとの保持は下記 context に接続済み。**Linux I/O／死亡回復との接続は未完了**。応答喪失で実行済みとなった登録は thread/session 終了まで残り得る。

- `runtime/lpr_unix/notify.c` に送信側 native adapter を追加。WAIT_SYNC で取得した通知 cap の ID・kind・正確な rights／PRIVATE|CLOEXEC を検査し、有限 cache から直接 SEND する。FD 不足・無効 cap・送信失敗時は WAIT_NOTIFY へ委譲する。native ALLOC だけを既存通知ありとして扱い、NOT_READY を queue-full と誤認しない。fork 子は親からコピーされた private FD 番号を使用・close しない。
  - LPR build／pack の入力依存へ接続済み。更新 thread ごとの保持は下記 context に接続済み。**Linux send/recv の commit 後の呼出しは未接続**。通知失敗は公開済みデータを巻き戻さない。今回 opcode／wire layout の変更はなく、通知 cap の CLOEXEC と受信口 HANGUP による登録回収を lifetime 契約に追加した。
- `runtime/lpr_unix/wait.c` に受信側 adapter を追加。待機 thread の channel に複数 socket の watch を登録でき、受信 cap の権限／flags／ID を検査する。自分の queue だけを有限回 drain し、通知 payload は信頼せず状態を読み直す。受信 cap close で unixd の全 watch 回収を促し、fork 子では親の private FD 番号を閉じない。
  - 単一方向の blocking I/O 用に drain→状態確認→共有 slot arm→状態と変化番号の再確認→既存 wait graph での待機→必ず disarm を接続。signal restart／EINTR／timeout でも登録を残さず、呼出側の同じ絶対 deadline を使う。graph は受信口 READABLE|HANGUP と unixd session HANGUP を監視し、自動 drain をしない。通知 queue の連続補充時は有限回で I/O 再確認へ戻す。共有 bank 満杯時は未登録で眠らず EAGAIN を返すため、Linux blocking 呼出側の扱いも統合時に必要。
  - **Linux socket syscall／poll／epoll への統合は未完了**。この adapter のホスト試験は kernel の実待機を置換しており、native の起床や signal 配送自体の証明ではない。
- `runtime/lpr_unix/context.c` を既存の thread record に接続。実行中 thread の record に session client・観測 owner・通知 SEND cache・受信 waiter・request counter を保持し、初回取得時だけ session／owner を登録する。同じ process の thread は unixd session 所有者を共有するが、native client cap／owner／waiter／cache は分離する。制御 RPC 中に thread-list lock は保持しない。
  - record に直接埋め込み、追加の heap allocation／TID 別 table は設けない。通常 thread exit と musl の unmapself exit で private cap を解放してから native exit に進む。fork 子では既存の thread record reset が context も初期化し、継承しなかった private FD の数値を閉じたり他 thread のコピーされた record を辿ったりしない。exec では PRIVATE|CLOEXEC cap と旧 address space の既存回収を使う。
  - 内部 thread launch record の大きさが増えるが、stack 配置と初期化は実際の sizeof を使う。既存 prefix の offset は検査し、userland wire／syscall opcode は変えない。LPR build と、この内部 header を使うホストテストの unixd include 依存を更新。**context の Linux AF_UNIX 呼出元はまだ未接続であり、kernel thread 起動／終了・fork/exec のゲスト証明はない**。

## 確認済み

- `bash tests/run-unix-transport-unit.sh`: ASan/UBSan 成功。STREAM/packet、wrap、圧力、PEEK、確定前後の process 死亡、反対側 mapping の RO、別 process 間 10,000 メッセージ。
- `bash tests/run-unix-broker-unit.sh`: ASan/UBSan 成功。所有権、名前／backlog、資格情報、失敗巻戻し、DGRAM 分離／順序／sender close 後の配送、診断情報の所有権検査。
- `bash tests/run-unix-notify-unit.sh`: ASan/UBSan 成功。独立 waiter、古い disarm、slot／通知口満杯、登録と commit／通知前死亡の前後関係、RO peer、別 process 間 50,000 メッセージ。cache の warm path、確保失敗の relay、偽装 ID、eviction も確認。
- `bash tests/run-unix-service-wait-unit.sh`: 実際の service 登録コードと bounded native IPC mock で成功。PRIVATE|CLOEXEC 受信口、SEND 専用配布、所有／接続関係検査、再利用、queue-full、FD 余力、thread 死亡・dup close／最後の close を確認。生存 thread の受信口 close→HANGUP による登録と共有 slot の回収も確認。guest の native IPC／exec 動作を代替する証明ではない。
- `bash tests/run-lpr-unix-notify-unit.sh`: ASan/UBSan 成功。実装 LPR adapter／共通 notify core と native syscall・RPC mock で、warm path の RPC 不使用、両方向 bank の通知、ALLOC と NOT_READY の区別、FD 不足時の relay、relay 失敗の伝達、余分な権限・CLOEXEC 不足・kind 不一致 cap の解放、偽装 ID、fork cleanup、request ID 枯渇を確認。実 IPC／Linux blocking I/O の確認ではない。
- `bash tests/run-lpr-unix-wait-unit.sh`: ASan/UBSan 成功。実装 receiver／共有 arm core／既存 graph 構築と RPC・native poll/recv・block mock で、cap 拒否と回収、channel 再利用、有限 drain、初回／arm 後の ready・変化番号更新時の睡眠回避、bank 満杯、attempt 枯渇、signal restart／EINTR／ready callback error の disarm、通知 peer と broker の独立した HANGUP、graph の重複統合・自動 drain 不使用、fork cleanup を確認。kernel の blocked waiter／timeout／signal 実配送は対象外。
- `bash tests/run-lpr-unix-context-unit.sh`: ASan/UBSan 成功。実装 context と実際の thread lookup／fork reset、通知 destructor を使い、session／thread 登録は RPC mock で検証。初期化失敗の FD 回収、1,000 回の current 取得で再登録しないこと、2 thread の独立した cap／owner と同じ session、request counter 分離・枯渇、受信／SEND／client の一度だけの解放、fork reset が copied private FD を閉じないことを確認。native clone・exit は実行していない。
- `bash tests/run-unix-rights-unit.sh`: ASan/UBSan 成功。送信者死亡後の queue 保持、prefix/余剰破棄、部分 STREAM、PEEK、未公開の所有権、宛先／位置／資格情報偽装、準備失敗の rollback、循環 GC、253 socket 参照、ゼロ長 SEQPACKET、消費前後の死亡、ACK 後の replay 拒否と 1,500 回の記録回収。外部 provider は参照数 mock であり、native FD の end-to-end 転送を証明するものではない。
  - DGRAM の複数送信元での容量制限・配送順、非 head の受領拒否、sender 死亡、共有 TX/header 改変後の受領、PEEK と prefix=0 の余剰解放も確認。
- `bash tests/run-unix-escrow-unit.sh`: ASan/UBSan 成功。native FD table mock で保持・解放、実権限の利用、範囲／重複 FD／TRANSFER 不足拒否、失敗時の所有権維持、ID 枯渇を確認。
- `bash tests/run-unix-service-rights-unit.sh`: ASan/UBSan 成功。実際の service RIGHTS dispatch と bounded native FD table mock で、分割送受信、APPEND 再試行と不一致拒否、途中失敗の解放、所有 session/thread の認可、FINISH 前の UNIX 参照非公開、prefix=0 の破棄、死亡 thread の結果照会と ACK を確認。実際の native IPC や LPR hidden import を検証したものではない。
- `bash tests/run-unix-client-unit.sh`: ASan/UBSan 成功。native RPC mock で応答照合、受信 capacity 超過、service error／送信・受信・map・確保失敗時の FD 解放を確認。実 IPC は未検証。
- `bash tests/run-lpr-unix-client-unit.sh`: ASan/UBSan 成功。実装 LPR process client／unixd adapter／共通 wire と実際の broker を、syscall loopback fixture で接続。session 取得→STREAM socketpair→ATTACH→CLOSE、SEQPACKET／DGRAM の pair 作成・解放、peer credentials、cap 個数不足・不正 reply・確保／map／CALL／RECV 失敗時の cleanup、signal 中断時に CALL をやり直さず同じ reply を待つこと、fork 後に親の FD 番号を CALL／CLOSE しないことを確認。実際の service dispatch／native IPC／Linux FD のテストではない。
  - thread 登録の自己観測 FD／移送権限、各 RPC 失敗時の donor FD 解放も追加検証。native thread 終了の監視自体はこの fixture の対象外。
- `bash tests/run-lpr-unix-mapping-unit.sh`: ASan/UBSan 成功。実装 mapping と transport、ホスト memfd の実 RO/RW mapping で、4 本の cap/header 検査、各 map 途中失敗の cleanup、両 endpoint の RO 側へ child が書こうとすると fault すること、STREAM/SEQPACKET の直接コピー・PEEK・ゼロ長 packet・EOF を確認。peer ID=0 で再 ATTACH した後も残データを読み、続いて EOF になることを確認。コピー／消費時の制御 RPC はゼロ（待機者なし）。**native IPC／Linux socket syscall／通知待機の end-to-end テストではない**。
- `bash tests/run-lpr-supervisor-control-unit.sh`: ASan/UBSan 成功。実装 helper＋native FD mock で channel 認可、one-shot activation、子の登録待ち、private/CLOEXEC 権限、exec 準備と staged process の取消しで旧制御口を保持すること、1,000 回の切替で supervisor FD 数が増加しないことを確認。ゲストでの FD 継承／close の証明ではない。
- `bash tests/run-lpr-process-control-unit.sh`: ASan/UBSan 成功。実装 client＋RPC mock で private/CLOEXEC の厳密検査、動的な制御口への送信、exec handoff の検査と不正 cap の取消し、PREPARE 拒否時に他の reservation を取り消さないこと、準備後も旧制御口が使えることを確認。
- `bash tests/run-lpr-exec-control-lifetime-unit.sh`: ASan/UBSan 成功。実装 transaction cleanup＋FD/RPC mock で exec reservation の取消し、二重 cleanup、fork との区別、最終 exec まで handoff の所有権を維持することを確認。manifest 固定 FD の置換についても、dup／flags 設定失敗で旧 slot を復元し donor を caller に残すこと、成功時だけ donor を消費することを確認。exec caller は成功後に donor 番号を無効化し、後続 RPC 後の二重 close を避ける。
- `bash tests/run-lpr-supervisor-child-notification-unit.sh`: 成功。
- `bash tests/run-lpr-supervisor-unix-session-unit.sh`: ASan/UBSan 成功。実装 supervisor registry/session helper＋unixd RPC/native FD mock で、自己申告 PID 不使用、管理口／他プロセス拒否、登録失敗と不正 cap の cleanup、1,000 回の再取得で再登録しないこと、exec で保持・fork 子で分離、未回収 zombie の保持 endpoint を即時解放することを確認。unixd の実 IPC・socket の最終回収を証明するテストではない。
  - `run-lpr-process-control-unit.sh` には session 取得の検証も追加。余分な DUP、PRIVATE／CLOEXEC 不足、ゼロ session ID、WAIT／POLL 不足の返信を拒否し、受信 FD を閉じる。supervisor 側も upstream の WAIT／POLL 不足を拒否する。
- `cd kernel && zig build test -j2 --summary all`: 111/111 成功。追加テストは private 除外と参照数・通常 FD 属性、複数 thread object の世代別終端、sibling／process 非 ready、短い wake 出力バッファ。
- `cd kernel && zig build limine -j2 --summary all`: 成功。
- `bash userland/personality/linux/build-lpr.sh`: 成功。
  - 共通 `client_wire.c` と LPR unixd adapter 追加後も成功。freestanding link／namespace 検査を含む。supervisor の native client 側の build も成功（`.artifacts/{lpr,lprs}-unix-client-build.log`）。
  - LPR mapping／thread 登録と transport／notify core のリンク追加後も成功（`.artifacts/lpr-unix-mapping-build.log`）。
  - LPR 通知送信 adapter／共通通知 cache のリンク追加後も成功（`.artifacts/lpr-unix-notify-build.log`）。unixd の受信口 HANGUP 回収追加後の native build も成功（`.artifacts/unixd-notify-build.log`）。
  - LPR 通知受信 adapter／wait graph 接続と session WAIT/POLL 追加後も LPR／supervisor の build 成功（`.artifacts/{lpr,lprs}-unix-wait-build.log`）。
  - thread record への context 接続後も LPR build 成功（`.artifacts/lpr-unix-context-build.log`）。既存の utimens-plan／linux-socket-transfer／netd-attachment-fd／linux-mremap／filed-session-lifetime／epoll-io-race／pending-signal-frame／pipe-poll／readv-inline／inotify-close-race／exec-control-lifetime のホスト回帰も成功。
- `bash tests/run-userland-service-abi-layout.sh`: 成功。unixd wire と native 疑似 FD 宣言の一致も確認。
  - FD ticket、送信／受信操作 ID、分割 descriptor を明示したため unix_control は 1,224 bytes。完了記録を安全に回収する ACK を FINISH の直後へ追加し、後続 opcode を振り直した。旧 wire は維持しない。
  - supervisor bootstrap に unix_admin_fd を endpoint の直後へ追加。flags を後ろへ移し、全体は 128 bytes。管理権限を LPR 用 endpoint とは別に渡すための変更で、seed0root と supervisor を同時更新し、管理 FD がない旧 bootstrap は拒否する。
  - supervisor の ACTIVATE と EXEC_PREPARE はそれぞれ登録・exec 操作群に配置して後続 opcode を振り直した。管理口と通常 process 権限を分離するための契約変更で、旧 opcode／共有 endpoint による process 操作は維持しない。manifest の supervisor フィールドも one-shot bootstrap を表す名前へ変更した。
  - PROCESS_UNIX_SESSION は認証・cap 発行の ACTIVATE 直後（4）へ配置し、後続 opcode を全て振り直した。資格情報を自己申告せずに UNIX 所有権を取得するための変更。supervisor／LPR／seed0root／termd と ABI 検証を同時更新する。
- `cmake --build .artifacts/cmake/unixd --parallel 1`: native unixd ビルド成功。
- seed0root／lpr_supervisor の CMake configure・build 成功。`cd pack && go run ./cmd/pacgo plan` で変更後の設定読込成功（read-only、生成／コピー／sync は実行していない）。
  - PROCESS_UNIX_SESSION 追加後も LPR／supervisor／seed0root／termd の build 成功。native daemon の並列 build は共有 musl staging を再生成するため競合した。seed0root は他の build 終了後に単独で再実行し成功。関連ログは `.artifacts/*unix-session-build.log`。
- `tests/unix_native_contract.c` と `tests/build-unix-native-contract.sh`: native guest 用回帰 probe を追加。**ゲスト未実行**。結果 marker は `UNIX_NATIVE_CONTRACT=OK`。
  - 権限の全未許可 bit、private fork 除外、fork 初期 thread の自己観測 FD、IPC 転送先の終了／kill 監視、self-kill 拒否、旧 FD の終端維持。
  - child ACK は wait 登録前なので、実行だけで「実際に blocked だった waiter の起床」を常に証明するわけではない。待機登録後の終了ケースを別途確実に検証する必要がある。
- rootfs のコピー／更新、pack 配備はまだ行っていない。

## 残作業（完了条件から外さない）

1. 上記通知経路と thread context の Linux I/O 接続、poll 統合、ET の非 ready→ready 世代／ONESHOT。現在の `changes` は操作／lock 解放の変化番号であり、正確な ET readiness 世代ではない。listener／DGRAM の broker-owned readiness の参照も必要。
2. SCM_RIGHTS／SCM_CREDENTIALS を上記 broker core・service wire と LPR provider に接続する。LPR の hidden import・分割再試行時の重複解放・失敗 cleanup・死亡後 publish 回復、MSG_CTRUNC/CMSG_CLOEXEC への反映が残る。service の CLAIM 成功だけでは native IPC 配送／provider import／Linux FD 公開の成功にならない。core／mock の成功を SCM 全体の完成とは扱わない。
3. supervisor の専用 process control channel は初期起動／fork／exec の実装に接続済み、ゲスト未検証。root-only の現行資格情報 registry・unixd session 発行・LPR 取得 client は追加済み。資格情報変更と失効、LPR AF_UNIX backend からの取得、fork の所有参照登録が残る。制御 RPC の応答喪失時の exec 予約回収も残る。
4. filed による pathname inode／権限の認証。native service は現状 PATH を EACCES としているので未対応であり仕様完了ではない。
5. LPR の AF_UNIX backend、send/recv/poll、FD/OFD/dup/fork/exec/SCM 転送と上記を統合。既存 netd に結び付く transfer occurrence も再設計する。最終 native exec 失敗時の Linux FD／cwd／replacement lease の巻戻しも必要。
6. seed0root／filed／pack の統合を完了する。unixd と supervisor を NIC 探索前に起動するコード・pack 定義は追加済み。drmd／inputd の provider lease が旧 netd に依存し、後段起動はまだ NIC 必須である。これを分離し、NIC 不在でも UNIX を利用する Linux 起動を完走させる。AF_UNIX と必要な netlink 機能を netd から分離し、netd を NIC/TCP/IP/AF_INET に絞る。旧 ABI は残さない。
7. ゲストで native 観測 FD、fork→exec→fork→exec、複数 waiter、各 commit 前後の thread/process 死亡、SCM／資格情報偽装、netd 停止中の UNIX 通信、性能を検証。
8. **QEMU と XFCE の正常動作確認は必須の完了条件**。XFCE 起動・画面表示と更新・キーボード／マウス・端末／ファイルマネージャーの起動と操作まで実測し、不具合は修正する。ログの ready marker だけでは完了にしない。既存の `run-lpr-qemu-xfce-startup-stability.sh`／`run-lpr-qemu-xfce-supervision.sh` と guest 用 app acceptance を活用し、画面・入力・アプリ操作の証拠も `.artifacts/` に残す。これらの QEMU wrapper は既定で rootfs の強制 sync を行うため、配置対象を決めずに実行しない。**現時点では未実行**。

ログ／成果物は `.artifacts/`。変更は未 commit。
