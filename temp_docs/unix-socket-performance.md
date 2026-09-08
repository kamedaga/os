# UNIX socket 高速化（2026-09-09）

続くDGRAM経路・RPC制御ページ再利用は [unix-dgram-cache.md](unix-dgram-cache.md) に記録。

netd分離の完了後に開始した性能改善。アカウント管理・一般的なOS機能の追加は対象外。

## 今回の変更

- 同じsocket・wait bank・通知ID・arm試行への成功済みSENDを重複させない。相手の共有領域には書かず、送信側の既存16枠キャッシュに記録する。出所も照合し、相手が自分のbankに書いた偽の将来tokenで別bankの正規通知を抑制できないようにする。新しいarm、キャッシュ退去、送信失敗では再送する。満杯によるEAGAINは成功済みとして記録しない。
- blocking I/Oの待機入口で共有状態を先に再確認する。通知drainの前後に二重実行していたHANGUP pollは後段の一回にする。arm後の再確認・変更番号の比較・HANGUP検査・bounded flood対応は維持。
- UNIX I/Oが毎回使うスレッドレコード取得を、GETTID二回・リスト検索二回・ロック二回から、それぞれ一回にまとめる。TLSキャッシュ等の新しい寿命管理は追加しない。

kernel、Linux ABI、unixd wire version 7は変更なし。配送・資格情報・SCM_RIGHTSの契約は維持。

## 測定条件・現状

4 vCPU / 2 GiB / KVM、console-shell、CPU pinningなし。計測中のビルドなし。
既存benchで64 B STREAM RTTを1024往復×5試行、bulkを16 MiB・16 KiB/sendおよび1 MiB・64 B/sendで各3試行、STREAM/SEQPACKET/DGRAMを測る。bulkは受信内容を検証する。CLOCK_MONOTONICの総時間は1 ms粒度。旧netdとの比較ではない。

- 変更前: `.artifacts/test-results/unixd-perf-before/`。RTT中央値 socketpair 135.742 µs / pathname 138.671 µs。16 KiB bulk中央値 STREAM 205.128 / SEQPACKET 183.908 / DGRAM 22.567 MiB/s。
- 通知・待機の2点のみ変更後: `.artifacts/test-results/unixd-perf-notify/`。RTT中央値 134.765 / 133.789 µs。QEMU全ソケットprobe完走、exit 0。小容量bulkは試行間の差が大きく、高速化率を確定できない。
- 出所別の重複判定・スレッド検索も含む最終版: `.artifacts/test-results/unixd-perf-scoped/`。全benchと全probeが完走し、`PERF_SCOPED_EXIT status=0`。kernel / unixd / supervisor / seed0rootは変更前と同一、LPRのみ変更。ハッシュを同ディレクトリに保存。

| 指標（各試行の中央値） | 変更前 | 最終版 |
| --- | ---: | ---: |
| 64 B RTT・socketpair | 135.742 µs | 126.953 µs |
| 64 B RTT・pathname | 138.671 µs | 125.976 µs |
| 16 KiB bulk・STREAM | 205.128 MiB/s | 205.128 MiB/s |
| 16 KiB bulk・SEQPACKET | 183.908 MiB/s | 205.128 MiB/s |
| 16 KiB bulk・DGRAM | 22.567 MiB/s | 23.188 MiB/s |
| 64 B bulk・STREAM | 1.658 MiB/s | 8.547 MiB/s |
| 64 B bulk・SEQPACKET | 6.494 MiB/s | 8.547 MiB/s |
| 64 B bulk・DGRAM | 0.090 MiB/s | 0.092 MiB/s |

64 B bulkの変更前STREAMは0.965〜6.667 MiB/s、SEQPACKETは1.062〜6.579 MiB/sと変動が大きい。最終版ではSTREAMが8.475〜8.621、SEQPACKETが8.547だったが、これを一般的な倍率保証とはしない。中間ビルドでも大きく揺れた試行があり、pinningなしの別起動比較である。大容量STREAMは横ばい、DGRAMの主な費用は残っている。

ホストの補助検査では同一armのSENDが二回目以降ゼロ、新しいarm・別bank・別socketではSENDされること、FD満杯・失敗時relay、実際の通知キャッシュを使うRO分離の別process間50,000メッセージ、スレッド取得1,000回がGETTIDちょうど1,000回であることを確認。QEMU全probeにはepollの別process再エッジ、資格情報、SCM_RIGHTS、fork/exec、SIGKILL後の所有者回収を含む。

## 最終版のXFCE / Writer確認

`.artifacts/test-results/unixd-perf-xfce/`。前掲最終版と同じバイナリのハッシュ一致を確認。XFCE ready 17.848秒、5アプリ表示PASS、手動確認後のharness exit 0 / classification=ready。全体319.2秒は操作時間を含み、起動時間ではない。

- GUI端末からWriterを起動し、`Unix socket performance verified`を入力して `/root/unix-perf.odt` に保存。
- Writerを終了して端末promptへ戻ったことを確認し、新しいWriterプロセスで再オープン。本文が保持されていることを確認。
- 初回Tipダイアログをマウスで閉じ、`Reopened and saved`を追記・再保存・終了。
- ODTだけをconsole経由で取り出し、ZIP全entryの整合性とcontent.xmlの二つの段落を確認。
- Thunarで `/usr` 表示後、マウスで `/root` に移動して文書を確認。Applicationsメニューもマウスで表示。

画像・ODT・console/serial・result.json・ハッシュを同ディレクトリに保存。自動input_checkはnot-runで、入力の根拠はこの手動操作と保存文書。PAGE FAULT / GENERAL PROTECTION / session-exitなし。QEMUは正常終了済み。DPMSはこの検証セッションだけ `xset -dpms; xset s off` で無効化し、設定や既存のOS未実装機能は変更していない。今回のGUI中にはRTTを再計測しておらず、性能表はconsole-shellの測定。

最初のGUI起動は通知の出所照合追加のため手動で中断した（`unixd-perf-xfce-interrupted/`）。起動・5アプリ表示は通ったが、harnessはmanual-incomplete/exit 1であり完了証拠にはしない。

## 次の独立した改善候補

DGRAMは毎パケット、RO/RWを分離した同じTX/RX VMOを受け渡し・map・unmapしている。世代付きマッピング再利用が有力。ただし現在の呼び出し内寿命から拡張するには、並行fork、thread終了、宛先変更、FD圧迫時の扱いを一緒に設計する必要がある。今回の3点には混ぜていない。配送FIFO・宛先全体の容量・資格情報の判断を勝手にクライアントへ移さない。
