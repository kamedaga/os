# unixd 実測（途中記録）

2026-09-08。実装段階・負荷ごとの測定記録。旧netd比の高速化率を示すものではない。

## UNIX socket 往復

QEMU 8.2.2 / KVM、q35、`-cpu host -smp 4 -m 2G`。ホストが報告するCPUは Intel Core Ultra 9 285K。CPU pinning なし。XFCE を起動しない console-shell の実ゲストで測定。LPR の UNIX 診断は無効。

既存 `lpr_futex_pingpong_bench` を 1,024 往復 × 5 回実行。要求64B＋応答64B、STREAM、sendmsg/recvmsg、blocking。socketpair は同一プロセスの2スレッド、pathname は fork した別プロセス同士。作成・接続・join/wait は計時外。専用の warm-up はなく、最初の反復の初期化を含む。各送受信の計時と集計の overhead も含む。

| 試行 | socketpair 平均 RTT (µs) | pathname 平均 RTT (µs) |
| --- | ---: | ---: |
| 1 | 535.156 | 539.062 |
| 2 | 550.781 | 537.109 |
| 3 | 534.179 | 546.875 |
| 4 | 535.156 | 540.039 |
| 5 | 538.085 | 544.921 |
| 5平均値の中央値 | 535.156 | 540.039 |

`CLOCK_MONOTONIC` の総経過時間を反復数で割った値。今回の総時間は1ms単位なので、表の小数は計算結果であり、その分解能を持つ個別 RTT の測定ではない。個別 RTT の p95/p99 や飽和スループットは未測定。同じ実行の futex 往復平均の中央値は10.742µsだが、payload転送・socket semanticsが異なるため代替性能や高速化率として比較しない。同条件の旧 netd 実装の計測値は未確保。

再実行:

```sh
.artifacts/bin/pacgo qemu-test --cpus 4 --timeout 90s \
  --send '/bin/bash /cmd/lpr_futex_pingpong_bench.sh 1024 5' \
  --expect LPR_FUTEX_PINGPONG_DONE
```

証拠: `.artifacts/test-results/unixd-rtt-64b/` の console.log / serial.log / host-time.log / run.log。全5回×全3方式が終了し、DONE を確認。

この段階ではblocking I/OごとにWAIT_REGISTER/WAIT_REMOVEのunixd RPCが残っていた。後述の登録再利用でこれを変更した。

### 待機登録の再利用：変更前後

VM修正後の同一kernel、4vCPU/2GiB/KVM、console-shell、同一benchの64B・1024往復×5、通常LPR・診断無効。測定中にビルドを並行していない。変更前は `.artifacts/test-results/unixd-wait-cache-before/`、変更後は `unixd-wait-cache-after/`。各console.logに全試行とDONE、binaries.sha256にkernel/LPRの識別値を保存。変更したLPRと拡張probeの配置のためrootfsは一度再生成しており、guest内の前回GUI作業データの有無まで同一ではない。

| 試行平均の中央値 | 変更前 (µs) | 変更後 (µs) | RTT短縮 |
| --- | ---: | ---: | ---: |
| socketpair・2 thread | 579.101 | 134.765 | 76.7% |
| pathname・別process | 578.125 | 133.789 | 76.9% |

これは新unixdの登録再利用前後の比較であり、旧netdからの高速化率ではない。スループットや個別往復のp95/p99の測定ではない。`.artifacts/test-results/unixd-wait-cache-lifetime/`の実QEMUでは、warm poll後に同じsocketをSCM_RIGHTSへ預けて最終ownerをclose→再受領→遅延送信の起床、同一FD重複poll、300組の待機・close後EOF、既存PTY/SCM/poll/epoll/fork/execを完走した。

### XFCE起動中の追加測定

`.artifacts/test-results/unixd-writer-final-vm/console.log`。最終VM修正版kernel、同じ4vCPU/2GiB/KVM、同じ64B・1024往復×5。XFCE上で全5アプリ表示・Writer編集保存再起動を行い、Writerを終了した後、GUI端末から同じbenchを実行した。前表とはkernelとセッション負荷が異なるため、差を一要因に帰属させない。

| 試行 | socketpair 平均 RTT (µs) | pathname 平均 RTT (µs) |
| --- | ---: | ---: |
| 1 | 991.210 | 1254.882 |
| 2 | 1033.203 | 1299.804 |
| 3 | 1011.718 | 1283.203 |
| 4 | 992.187 | 1263.671 |
| 5 | 993.164 | 1290.039 |
| 5平均値の中央値 | 993.164 | 1283.203 |

同じ実行のfutex中央値12.695µs。全方式の全試行とDONEを確認。低負荷console測定より遅く、現行実装を高速化達成とは扱わない。

### 登録再利用後のXFCE起動中

`.artifacts/test-results/unixd-wait-cache-xfce/console.log`。同じ4vCPU/2GiB/KVM、全5アプリ表示、Writerの編集・保存・新プロセス再オープン後にWriterを終了し、GUI端末で同じ64B/1024往復×5を実行。

| 試行 | socketpair 平均 RTT (µs) | pathname 平均 RTT (µs) |
| --- | ---: | ---: |
| 1 | 128.906 | 140.625 |
| 2 | 147.460 | 141.601 |
| 3 | 140.625 | 148.437 |
| 4 | 139.648 | 142.578 |
| 5 | 135.742 | 139.648 |
| 5平均値の中央値 | 139.648 | 141.601 |

futex中央値11.718µs。全試行とDONEを確認。前のXFCE測定より短いが、別起動・別GUI作業データであり、厳密な負荷再現ではない。

## GUI

### version 4・DGRAM境界動作追加後（最新）

`.artifacts/test-results/unixd-boundary-xfce/console.log`。同じkernel・4vCPU/2GiB/KVM、通常LPR、STREAMの64B/1024往復×5、全5アプリとWriter保存・再オープン後にWriterを閉じてGUI端末から計測。今回のbenchは配置済みの従来版で、後から追加した `--bulk` モードではない。計測中のビルドなし。

| 試行平均RTT (µs) | 1 | 2 | 3 | 4 | 5 | 中央値 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| STREAM socketpair | 142.578 | 138.671 | 128.906 | 139.648 | 143.554 | 139.648 |
| STREAM pathname | 143.554 | 143.554 | 138.671 | 129.882 | 141.601 | 141.601 |

futex中央値10.742µs、全試行とDONEを確認。**このGUI実行全体はmanual-incomplete/exit 1**。操作とRTT取得の後にこちらの終了通知が遅れ、600秒の手動期限に達したためで、成功したharnessとは扱わない。保存したserial/consoleにPAGE FAULT/GENERAL PROTECTION/session-exitはない。

再検証の `.artifacts/test-results/unixd-boundary-xfce-final/` はrootfs更新なしで再起動し、全5表示、ODTの再起動をまたぐ本文保持、追記・再保存・新Writerプロセスでの再オープン、マウスメニュー操作が成功、harness exit 0。ready 17.516秒、全体432.2秒（手動確認時間込み）。取り出した単一ODT `unixd-shutdown.odt` のZIP整合性と `Unixd shutdown verified` / `Reboot verified` の本文を確認。初回起動・再起動ともDesktop assertionは解消、system bus等の警告は残る。この再検証ではRTTは再計測していない。

### DGRAM接続後の回帰検査

`.artifacts/test-results/unixd-dgram-xfce/`。同じkernel・4vCPU/2GiB/KVM、通常LPR、64B要求＋64B応答・1024往復×5。今回はDGRAM Linux I/Oを追加したLPRと再生成したXFCE overlayを配置した別起動。全5アプリ表示、Writer編集・保存・終了・再オープン後、Writerを閉じてGUI端末から計測。測定中のビルドなし。

| 試行平均RTT (µs) | 1 | 2 | 3 | 4 | 5 | 中央値 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| STREAM socketpair | 143.554 | 136.718 | 140.625 | 139.648 | 142.578 | 140.625 |
| STREAM pathname | 125.000 | 142.578 | 134.765 | 139.648 | 144.531 | 139.648 |

futex中央値11.718µs。全試行とDONEを確認。前回GUIの139.648µs / 141.601µsから大きな退行は見られないが、厳密なGUI負荷再現ではない。**この表はSTREAMの回帰検査であり、DGRAMの速度ではない。** DGRAMのRTT/飽和スループット、個別RTTのp95/p99は未測定。

- 同起動で `/cmd/lpr_unixd_pair_probe.elf` を再実行し、従来のSTREAM/SEQPACKET/SCM/poll/fork/execとDGRAMのPAIR/NAMEDまで全markerとDONEを確認。
- Writerで `Unixd datagram integration verified` を `/root/unixd-datagram.odt` に保存し、終了後に新プロセスで同ファイルを開き本文保持を確認。writer-open/saved/exited/reopened.png を保存。
- guestのODT content.xmlと単一ファイルとして取り出した `unixd-datagram.odt` の本文が一致、host unzip -tも成功。最後にApplicationsメニューをマウスで開く操作も確認。harness exit 0、ready 20.335秒、全体545.3秒（手動確認時間込み）。保存したserial/consoleにPAGE FAULT/GENERAL PROTECTION/session-exitなし。resultのinput_checkはnot-runで、手動入力の証拠はscreenshots/manual-doneを参照。
- 初回Desktopのassertion、system bus不在等の警告は残る。Desktopのbuild-timeディレクトリはmanifestに入っていなかったため、`pack.yaml` の既存rootfsDirsへ追加し生成結果 `/root/Desktop=@dir` を確認した。この配置定義は今回のQEMU起動後の修正で、新規rootfsへの反映・初回起動確認は次回のまとめた配置で行う。

- `.artifacts/test-results/unixd-wait-cache-xfce/`: 待機登録再利用後も全5アプリ表示PASS。Writerで`Unixd cached waits verified`を入力し`/root/unixd-cached.odt`へ保存→アプリ終了→新プロセスで再オープン、本文保持を目視確認（writer-saved.png / writer-reopened.png）。ゲストでODT content.xmlも読んで一致を確認。端末から`thunar /usr`を実行し、フォルダ一覧を確認（thunar-usr.png）。初回Desktop作成のassertionとsystem bus警告は残る。
- `.artifacts/test-results/unixd-writer-final-vm/`: 最終のMREMAP/munmap修正を含むkernelで再起動。rootfs再生成なしで前回のODTを開き、本文のディスク永続化を確認（writer-reboot-persisted.png）。さらに`Latest VM verified.`を追記・再保存し、Writer終了→新しいプロセスで再オープンして2段落の保持を確認（writer-resaved.png / writer-reopened.png）。同起動でThunar/Pine2/About/GTK demo/terminalの全5表示検査もPASS。冷起動時のWriter文書表示は遅く、system bus不在等の警告も残るため、GUI機能の実操作成功と性能・全体正常化を区別する。
- `.artifacts/test-results/unixd-writer-manual/`: PT回収修正版kernelの実QEMUで、Writer起動→本文入力→`/root/unixd-writer-verified.odt`保存→Ctrl+Q終了・端末prompt復帰→新しいWriterプロセスで同ODT再起動→本文表示を目視確認。writer-edited/saved/exited/reopened.pngを保存。再度Writerを終了し、ゲストの`/bin/busybox unzip -p ... content.xml`で本文`Unixd Writer save and reopen`がODT内にあることもconsoleログで確認。新規文書→保存・アプリ再起動の操作は成功。system bus不在等のXFCE警告は残る。これは後続のMREMAP source lazy-hole許容修正を配置する前の結果。
- `.artifacts/test-results/unixd-xfce-pty-all/`: 全5アプリ表示 PASS、端末の pwd、Thunar の `/usr` 移動、Home / File System のデスクトップアイコンを画面で確認。
- `.artifacts/test-results/unixd-xfce-libreoffice/terminal-command.png`: LibreOffice Writer の新規文書編集画面を確認。これだけでは編集・保存・再オープンの完了としない。
- `.artifacts/test-results/unixd-xfce-writer-save/`: **失敗**。再起動して編集・保存を試す前後に X セッションが終了し、画像は display inactive。serial.log:1846 に user PAGE FAULT、後続に GENERAL PROTECTION、console.log に SESSION_EXIT。旧 harness は起動時の ready を維持し exit 0 / result ready としたが、これは誤判定で、この実行を成功の証拠にしない。harness は操作中・終了時の session-exit と serial fault を失敗にするよう修正した。保存・再オープン、スループット、性能改善は未完了。
