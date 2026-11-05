# klogudp

`userland/klogudp/build.sh` で native worker と Linux 起動用ランチャーをビルドする。
配置用成果物は `.artifacts/epiphany-emfile/klogudp-payload/`。
`FILES.tsv` は rootfs 相対パス、mode、SHA256。rootfs へのコピーは行わない。
検証は `userland/klogudp/test.sh`（host モック、ELF 検査）。実機・QEMU では未検証。

## SSH の bash から起動

`/cmd/klogudp.elf` と `/cmd/klogudp-start` が配置済みの環境で、先に受信側を起動する。
Windows の LAN IPv4 を例として `192.168.3.8`、ポートを `5000` とすると:

```sh
/cmd/klogudp-start 192.168.3.8 5000
# 起動時点より前の ring も必要な場合:
/cmd/klogudp-start 192.168.3.8 5000 --from-start
```

通常は一方だけ起動する。既定は起動時の末尾から。ランチャーの正常終了だけでは
worker の起動完了を保証しないので、受信側で `alive seq=0 kernel_ms=...` を確認する。
worker は native `EXEC_PATH` で別プロセスとして spawn され、ランチャーは process/thread
観測 FD を閉じて終了する。LPR supervisor には登録せず、SSH の pipe や bash の親子関係を
継承しない。`nohup` やバックグラウンド化は不要。実行中に SSH 接続を維持する必要もない。

ランチャーだけが起動時に filed を使用する。worker に渡すサービス能力は netd client FD 241 のみ。
worker は logd と同じ `KERNEL_LOG_READ` と cursor を使用し、最大 1024 bytes/UDP datagram、
1周期最大64読取、250ms sleep。各周期の先頭で連番、kernel monotonic ms、送信試行数、
送信エラー数、cursor を送る。overrun/dropped と ring エラーも送信する。
送信失敗時はその ring chunk の cursor を進めず次周期に再試行する。
起動後の open、filed RPC、Linux syscall はなく、libc の使用は文字列操作と snprintf のみ。

既存 klogtail の直接起動は native ELF を LPR 経路でロードするため不正。
LPR は ELF の syscall 命令を Linux syscall dispatch に書き換える。
native musl の TLS 初期化は native nr=18 (`THREAD_SET_FS_BASE`) を使うが、
Linux nr=18 は pwrite64 なので TLS が設定されない。これは rc139 と整合する起動 ABI 不一致。
当該 rc139 の fault PC が保存されていないため、最終的な fault 命令自体は未特定。
新ランチャーは Linux musl の静的 ELF とし、ELF 書換え後に生成する3-byte native gate
(`syscall; ret`) で既存 native IPC ABI を明示的に呼ぶ。native wrapper の生成物は build/bridge に保存。
RWX anonymous mapping を起動時だけ使用する。新しい syscall、ABI、バージョンは追加しない。
ランチャーも worker も **host Linux 上では実行しない**（host では番号の意味が異なる）。

## 受信

WSL または同一 LAN の Linux host で、リポジトリから:

```sh
mkdir -p .artifacts/epiphany-emfile/klogudp-capture
python3 -u userland/klogudp/host/receive.py --port 5000 \
  > .artifacts/epiphany-emfile/klogudp-capture/ring.log
# OpenBSD nc を使う場合（nc の種類で引数は異なる）:
# nc -u -l -k 5000 > .artifacts/epiphany-emfile/klogudp-capture/ring.log
```

UDP は到達・順序・保存を保証しない。heartbeat の seq/tx/cursor の飛びを併せて確認する。
netd と native scheduler/kernel 自体が停止した場合は転送も止まる。
netd RPC が reply を返さなくなった場合も停止する。commit 後の timeout で共有 page を
再利用すると netd と競合するため、無理な timeout/retry はしない。

## WSL2 NAT: Windows 側で UDP relay

実機の宛先は WSL の 172.x アドレスではなく Windows の LAN IPv4。
`netsh interface portproxy` は TCP 用なので、この UDP 転送には使わない。
WSL 側は上の receiver を `0.0.0.0:5000` に bind し、WSL firewall を使用中なら UDP 5000 を許可。
Windows の PowerShell で WSL の現在の IP を `wsl.exe -d Ubuntu hostname -I` で確認する。
管理者 PowerShell で受信 firewall rule を設定する（必要な LAN に限定）:

```powershell
New-NetFirewallRule -DisplayName "CapabilityOS klogudp" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 5000 -RemoteAddress 192.168.3.0/24
```

通常の Windows PowerShell で relay を起動する（ディストリ名と WSL IP は置き換える）:

```powershell
powershell.exe -NoProfile -File "\\wsl.localhost\Ubuntu\home\kamer\os\userland\klogudp\host\relay.ps1" -WslIp 172.20.12.34 -Port 5000
```

relay は Windows UDP 5000 を bind し、各 datagram を WSL IP の UDP 5000 に送る。
Ctrl+C で終了。WSL 再起動後は IP を再確認。Windows 側で他の UDP 5000 receiver を同時起動しない。
終了後に不要なら `Get-NetFirewallRule -DisplayName "CapabilityOS klogudp" | Remove-NetFirewallRule`。

Windows 11 22H2 以降で mirrored networking を利用できる場合は、`.wslconfig` の
`[wsl2]` に `networkingMode=mirrored` を設定して WSL を再起動すると、relay を省ける構成もある。
既存作業を止める設定変更なので本作業では実施していない。Hyper-V firewall も UDP 5000 を許可する:

```powershell
New-NetFirewallHyperVRule -Name "CapOS-klogudp" -DisplayName "CapOS klogudp" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 5000
```

[Microsoft の WSL networking 資料](https://learn.microsoft.com/en-us/windows/wsl/networking) を参照。
netd の libuinet UDP send は sockaddr の IPv4/port をそのまま使用するため、実機の設定が
192.168.3.0/24 に属し通常の ARP/routing が動けば、同一 subnet の別 host を指定できる。
本作業では実機の IP/netmask、AP の client isolation、Windows/WSL firewall は未確認。
疎通しない場合はそれらを確認し、同一 LAN の別 Linux host で直接受信する方法も使える。
