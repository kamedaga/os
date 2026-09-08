# netd・AF_UNIX 再設計

## 今回の範囲

netdからAF_UNIXを分離し、契約・実装・デバッグ経路を再設計する。QEMUでsocket動作、XFCE操作、LibreOfficeの編集・保存・再オープン、UNIX往復性能を確認する。以前から未実装のUID/GID変更・補助グループ・OS全体の認可、およびそれらを必要とするsystem busの新規稼働は対象外。既存制約と今回の退行を区別し、system bus警告だけを完了阻害条件にしない。サービスユーザー追加も行わない。

## 責務と通信経路

netd は NIC・TCP/IP、unixd は名前・接続・所有権・FD 転送、LPR は Linux API と直接転送を担当する。unixd は NIC/libuinet に依存しない。

bind/listen は unixd に受付を作り、connect は接続を受付キューへ積んだ時点で成立する。accept はその接続を取り出す。backlog 満杯は待機または EAGAIN。socketpair は同じ接続を名前なしで作る。pathname は filed の inode・権限に結び付け、unlink 後も既存接続を維持する。abstract 名は長さ付きバイト列とし、最後の参照で消す。

通常の STREAM/SEQPACKET は **送信側 LPR → 方向別の共有領域 → 受信側 LPR**。送受信・poll の unixd RPC は不要にする。STREAM は部分送受信、SEQPACKET はメッセージ単位で公開する。メッセージ型のゼロ長データを EOF と混同しない。

## 共有領域と DGRAM

- データ・公開位置は送信側が書き、受信側には読み取り権限だけを渡す。STREAM/SEQPACKET は各端点の送信領域と、その端点が書く受信消費位置を一つの VMO の別ページにまとめる（接続あたり2 VMO）。自分側は RW、相手側は RO とし、相手の VMO の書込・resize・revoke 権限は渡さない。相手の位置・長さはローカルへ一度読み、範囲検査して使う。
- DGRAM は送信元・宛先 socket の組と世代ごとに領域を分け、他の送信者・宛先へその領域を渡さない。**宛先全体の順序と容量は unixd が小さな配送記録で管理する**。送信者がデータを書いた後、unixd が宛先・容量を検査して記録を公開した時点で送信成功とする。payload は受信側が直接読む。受信完了は配送記録を消費して容量を返す。
- DGRAM の接続先指定・送信元アドレスも配送記録に持たせ、接続済み受信側の相手制限を公開前に検査する。宛先は socket ID と世代で識別し、同名再 bind へ古い接続を付け替えない。
- DGRAM のshutdownはsocket実体の方向別状態とし、受信停止後も既存配送を読めるようにする。空キューでのblocking受信0とnonblocking EAGAIN、ゼロ長配送を区別し、peerへSTREAMのEOFは伝えない。書込不能のEPIPEでもDGRAMではSIGPIPEを発生させない。FIONREADの長さは配送記録から返す。
- AF_UNSPEC切断・別peerへの再接続では未受信配送とescrowを破棄する。初回接続・同じpeerへの再接続は保持。共有RXの生存中ownerを奪わず、broker配送記録から容量を回収し、古いHEADの受信完了をESTALEで拒否する。LPRは隠れたFD importを戻して再取得する。破棄済み配送を消費済みIDの上限で受信成功と推定しない。
- 双方向DGRAM接続で未読配送を捨てた場合、旧peerへECONNRESETを通知する（自己接続を除く）。保留エラーはbrokerのsocket実体に置き、pollでは取得せず通知し、I/OまたはSO_ERRORが一度だけ取得する。送信成功後の破棄は送信失敗とは別状態にし、FD参照を解放してもACK前の成功記録は維持する。
- 公開済み領域は消費確認まで再利用しない。共有 payload 自体は相手が変更可能な入力として扱い、認証・権限・FD の所有権判断には使わない。改変で影響できる範囲をその送信元のデータに限定する。

DGRAM の制御往復は残る。これで送信者間の分離と一つの受信順序を保ち、データの中央コピーを省く。全 socket 種別が同じだけ高速になるとは扱わない。

## 認証と所有権

- supervisor が PID・UID/GID と世代を管理し、unixd にプロセス専用の制御口を登録する。アプリに渡す制御 capability は private、dup/transfer/set-flags 不可にする。fork の子には supervisor が別の制御口を発行する。自己申告の PID や推測可能な番号を認証に使わない。
- SO_PEERCRED は connect/listen/socketpair 時点の資格情報を保存する。SCM_CREDENTIALS は送信ごとに unixd が supervisor の資格情報と照合し、許される PID・UID/GID だけを配送記録へ入れる。既存のroot資格情報ドメインで動作させ、OS側のUID/GID変更機能は今回追加しない。
- SO_PEERCREDの参照はVMOへのattachと分離する。未接続・名前付きDGRAM接続には資格情報がなく、DGRAM socketpairには作成時の情報がある。SO_PASSCRED等のsocket optionはunixdの実体で保持し、dup/fork/SCMで共有、acceptに継承する。STREAM/SEQPACKETは受信側所有の共有RXにoptionを反映し、資格情報が必要な送信だけbrokerで認証する。分割受信では資格情報を末尾まで残し、FD参照は最初の消費で解放する。後からPASSCREDを有効にしても未取得の送信者情報は生成せず、Linuxの未取得値を返す。
- dup/fork/SCM_RIGHTS は同じ socket 実体を参照する。接続・キュー位置・O_NONBLOCK は実体側、FD 番号・CLOEXEC は FD 側。fork は子の所有参照を登録してから実行を開始する。
- shutdown は方向別に確定し、STREAM は残データの後に EOF、書込不能なら EPIPE/SIGPIPE。最後の所有参照がなくなれば unixd が切断する。listener の未 accept 接続と未受領 FD も回収し、名前だけでは socket を延命させない。

## FD 転送と途中死亡

SCM_RIGHTS は次の三状態とし、unixd が遷移を直列化する。ticket は送受信 socket・世代・データ位置に結び付け、単なる番号の提示では受領できない。

| 状態 | 所有者と確定点 |
| --- | --- |
| 準備中 | 送信操作が転送参照を保持。データを準備し、COMMIT を unixd が受理するまでは相手に見せない。死亡・失敗なら破棄。 |
| キュー内 | unixd が FD と対応するデータ位置を一緒に公開。送信者が死亡しても残す。STREAM の受信は ancillary の境界で区切る。 |
| 受領済み | 受信側が非公開の FD 枠へ import した後、unixd が消費と所有者変更を確定。操作 ID ごとに結果を保存し、再試行しても再配送しない。 |

受信の確定前に失敗した内部処理は準備分を破棄する。確定後は巻き戻さず、同じプロセス内の回復処理が FD 公開を完了する。プロセス全体が死亡したらその所有参照を閉じる。未受領 FD が socket 同士を循環参照する場合は、unixd が実際の所有者から到達できないキュー群を回収する。

**制御バッファ不足・FD 上限超過は「全体を未消費に戻す」扱いにしない。** 収まる FD を渡し、余剰を閉じて MSG_CTRUNC を返す。通常 read のように制御情報を受け取らない場合も余剰を破棄する。PEEK は元のデータ・参照を残して受信側の参照だけを複製する。MSG_CMSG_CLOEXEC は受信 FD に適用する。[Linux の規則](https://man7.org/linux/man-pages/man7/unix.7.html)

## 同期・通知・回収

- socketごとの待機登録はthread内で再利用する。保持数はpoll graphの既存容量までに制限し、使用中の登録は追い出さない。I/O終了時は共有slotとactive参照だけを解除し、登録の解除RPCはidle登録の追出し時だけにする。idle登録はsocket所有参照やFD pinを保持しない。同じbroker socketをclose後に再受領する場合は、ローカルsocket状態の非再利用identityで区別し再登録する。threadの通知受信口を閉じれば全登録を回収する。cache内に収まる接続の通常I/O・pollでは登録RPCは発生しない。
- 通知の送受信 capability は PRIVATE|CLOEXEC。受信口が閉じた場合も unixd が登録を回収する。通知先の SEND capability が他プロセスに残る場合に備え、LPR は unixd session の WAIT/POLL 権限で broker 自体の HANGUP も監視する。通知 queue だけを broker の生存判定に使わない。

- 同じ socket 側の send 同士、recv 同士をそれぞれ直列化する。共有操作記録に所有 thread・世代、予約位置、確定位置を持ち、公開位置／消費位置の atomic 更新を通常 I/O の確定点にする。コピー中は相手がスロットを再利用できない。
- unixd は所有 process/thread の終了を監視する。所有者死亡後、送信は未公開分だけを破棄し、公開済み分は残す。受信は消費確定前ならキューに残し、確定後なら戻さない。操作世代を進めてロックを解放し、公開後・通知前の死亡も再通知して回収する。時間切れだけで生存中の所有者を追い出さない。
- 待機者ごとに通知 channel を持ち、同じ待機者の channel は再利用する。共有 slot には unixd 発行の通知 ID と待機世代を登録する。更新側は初回だけ unixd に socket 間の関係を検査させて SEND 専用 capability を取得し、以後は cache から直接通知する。native FD の確保に失敗する場合は unixd に通知を委譲し、通知を黙って捨てない。受信口は private・dup/transfer 不可とする。登録は空き slot を埋めて active を公開し、解除は世代を変える。待機者は「自分の通知を drain → 状態確認 → 登録を公開 → 状態・変化番号を再確認 → wait」の順。更新側は状態と変化番号を公開してから登録済み全待機者へ通知する。この順序は sequentially consistent な atomic 操作で結ぶ。初回の capability 取得中も登録を維持するため、事前に全更新者へ配布・ACK する段階は不要。
- 一人が通知を消しても他者の通知は残る。channel 満杯なら既に通知ありとして集約し、復帰後は必ず状態を再確認する。レベル型 poll は状態を返す。epoll ET は公開・消費の共有世代を現在のreadinessより先に読み、観測の間に別プロセスが起こした非ready→readyも検出する。IN/OUT/ERRを分け、待機登録直前にも観測済み世代と比較する。ONESHOT は通知後に disarm、再登録時に状態を再評価する。死亡処理も同じ通知経路を通す。[epoll の規則](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html)

## ABI と実装範囲

完了には QEMU 上での実動作確認を含める。UNIX 通信・fork/exec・FD 転送に加え、XFCE の起動、画面表示・更新、キーボード／マウス入力、主要アプリ（端末・ファイルマネージャー）の起動と操作を確認する。不具合は修正し、起動ログだけで正常とは判定しない。検証ログと画面の証拠は `.artifacts/` に残す。

接続結果を「socket ID・世代・方向別 VMO・通知／所有 capability」に変更する。制御操作は接続／所有権／配送・FD 転送の順に番号を振り直す。操作 ID、確定状態、配送順序、資格情報世代を wire に追加し、netd・unixd・LPR・supervisor・起動側の対象 ABI を一括更新する。理由は通信先と状態・所有権の保持先が変わるためで、旧 ABI は残さない。

既存の VMO 権限、private FD、thread 終了検知を使う。現在の futex は process と仮想アドレスをキーにしているため、プロセス間の共有ロック待ちには使わず通知 channel を使う。SCM_RIGHTS 本体は unixd が保持し、片側死亡でキューを即回収しない現状の channel に預けない。kernel の編集は汎用機構の不足が判明した場合に限り、userland で対応できない理由と変更箇所をサブエージェントに審査させ、承認を得てから行う。実施済みの承認・変更は [実装記録](netd-implementation-status.md) に記す。[FD 権限](../kernel/abi/fd_abi.zig)・[継承](../kernel/src/state/process.zig)・[待機](../kernel/src/syscall/runtime.zig)

FD・キュー・待機登録の上限は確保前に検査し、CLOSE と回収用の受信余力を予約する。確認対象は、送信者・宛先間の領域分離、FD 切詰め・PEEK、資格情報偽装、各確定点の直前／直後での thread・process 死亡、複数待機者、netd 停止中の通信。診断には socket・操作 ID、所有者、確定位置、待機理由を出す。これは設計であり、速度・競合耐性・Linux 互換性の実証は実装後に行う。

## 実機での完了確認

- QEMU の XFCE でマウス・端末・Thunar の操作に加え、LibreOffice Writer で文書の編集・保存・再オープンを確認する。起動や mapped window の検査だけで完了としない。
- UNIX socket の往復遅延を測る。socketpair のスレッド間と pathname 接続のプロセス間を区別し、payload・反復数・CPU数・QEMU設定・計時方法・ばらつきを記録する。旧実装やホストとの比較は条件の違いを明記する。同条件の旧実装の数値がなければ高速化率を推測しない。追加のスループット最適化は今回の完了条件に加えない。
