# Phase 4 — frame / input critical-path breakdown

## 測定条件

- 2026-07-18、4 CPU、direct `/usr/bin/sway`
- 640x480 full-damage Wayland animationを5秒
- keyboard + relative mouseをQMPから6 event投入
- 全timestampはguest `CLOCK_MONOTONIC`
- frameは`wp_presentation`のDRM sequence、inputはIRQ由来event timeで相関
- hot pathではringに記録し、scenario終了時だけ集計

baseline raw logは`.artifacts/phase4-critical-path/4cpu/`に保存した。

## 1 frame

frame intervalはp50 113 ms、p99 / max 135 / 135 ms、8.190 FPSだった。
内訳はpresentation sequenceで一致した末尾32 frameを対象とする。

| 区間 | p50 ms | p95 ms | p99 ms | max ms |
|---|---:|---:|---:|---:|
| client draw | 1 | 2 | 2 | 2 |
| surface attach / damage / feedback setup | 1 | 1 | 1 | 2 |
| `wl_surface.commit` call | 1 | 1 | 1 | 1 |
| client flush + Sway + wlroots render | 31 | 54 | 55 | 55 |
| drmd acquire fence | <1 | <1 | <1 | <1 |
| drmd module / array prepare | <1 | <1 | <1 | 1 |
| virtio `transfer_2d` | <1 | 1 | 1 | 1 |
| virtio `set_scanout` | <1 | 1 | 1 | 1 |
| virtio flush | <1 | 1 | 1 | 1 |
| virtio notify | <1 | <1 | 1 | 1 |
| synthetic flip queue | <1 | <1 | <1 | <1 |
| DRM event queue -> wlroots read | <1 | <1 | <1 | <1 |
| wlroots read -> presentation feedback受信 | 73 | 77 | 77 | 79 |
| draw -> presentation feedback受信 | 109 | 133 | 133 | 136 |
| commit -> frame callback受信（並行枝） | 69 | 97 | 99 | 100 |

各区間のpercentileは別々に計算するため、表のp50の単純和はtotal p50と厳密には一致しない。

現行drmdのflip timestampはvirtio command notify直後に作るsynthetic eventであり、実device completionや物理scanoutは観測していない。したがって表の終点はWayland presentation feedbackで、実表示完了ではない。

## Keyboard / pointer input

最新runの6 eventをIRQ順とWayland受信順で1対1に対応付けた。

| 区間 | p50 ms | p95 ms | p99 ms | max ms |
|---|---:|---:|---:|---:|
| inputd IRQ-ready -> `SYN_REPORT` publish | 20 | 35 | 35 | 35 |
| publish -> libinput read | 10 | 218 | 218 | 218 |
| libinput read -> Wayland client受信 | 205 | 207 | 207 | 254 |
| IRQ-ready -> Wayland client受信 | 252 | 266 | 266 | 299 |

virtio-input eventはhardware press timestampを持たないため、物理押下/QMP投入からinputd IRQ-readyまでの区間は未観測である。

同じ実装の直前runはIRQ-readyからclientまでp50 / p99 / maxが97 / 101 / 140 msだった。最新runは252 / 266 / 299 msで、入力経路には大きなrun間変動がある。

## 結論

DRM fenceと各virtio submit commandは各p50 1 ms未満で、現在の主因ではない。

共通する最大区間は次の二つである。

1. frame: drmd eventをwlrootsが読んでからpresentation feedbackがclientへ届くまでp50 73 ms
2. input: libinputが読んでからWayland input eventがclientへ届くまでp50 205 ms

さらにinput publishからlibinput readはmax 218 msのtailを持つ。次の改善対象はDRM commandやAVX decoderではなく、Sway processのevent-loop scheduling、wait/wake、Wayland socket deliveryである。

## 最適化1 — netd wait結果の再利用

netdはgeneric wait setでHANGUPを待った直後、結果を捨てて全active socket / leaseへ個別のnative pollを再実行していた。wait setの`revents`をHANGUP reapへ直接渡し、重複pollを除去した。socketのreadable通知、POLL判定、rearm semanticsは変更していない。

同じ4 CPU・5秒scenarioを1回実行した。raw logは`.artifacts/phase4-critical-path/4cpu-after-netd-revents/`に保存する。

| 指標 | before | after | 差 |
|---|---:|---:|---:|
| animation FPS | 8.190 | 8.545 | +4.3% |
| frame interval p50 | 113 ms | 108 ms | -5 ms |
| frame interval p99 / max | 135 / 135 ms | 132 / 132 ms | -3 / -3 ms |
| client flush + Sway + wlroots render p50 / p99 | 31 / 55 ms | 42 / 52 ms | +11 / -3 ms |
| wlroots read -> presentation feedback p50 / p99 | 73 / 77 ms | 70 / 76 ms | -3 / -1 ms |
| draw -> feedback p50 / p99 | 109 / 133 ms | 116 / 129 ms | +7 / -4 ms |
| commit -> frame callback p50 / p99 | 69 / 99 ms | 77 / 87 ms | +8 / -12 ms |
| IRQ-ready -> publish p50 / p99 | 20 / 35 ms | 17 / 17 ms | -3 / -18 ms |
| publish -> libinput read p50 / p99 | 10 / 218 ms | 50 / 50 ms | +40 / -168 ms |
| libinput read -> Wayland client p50 / p99 | 205 / 207 ms | 184 / 186 ms | -21 / -21 ms |
| IRQ-ready -> Wayland client p50 / p99 / max | 252 / 266 / 299 ms | 251 / 253 / 288 ms | -1 / -13 / -11 ms |

frame tailと入力tailは改善したが、p50の一部は悪化した。入力は同じ実装でもrun間変動が大きいため、1 runだけで個別区間の効果を断定しない。最大区間は引き続きSway内の同期処理と、Wayland event enqueue後のsocket flush / client deliveryである。

## 最適化2 — LPR Unix socket RPC fast path

通常の`epoll_wait`は全socketをauthoritative pollしてからwaitし、wake後にも同じ全件pollを繰り返していた。初回pollはcorrectnessのため維持し、generic waitでreadyと確認したUnix socketだけをwake後の再scanへ渡した。`timeout=0`とfinite timeoutの初回確認も従来どおりである。

さらに、LPRは再利用中のRPC pageをSEND / RECV / POLLごとに64 KiB全消去し、固定netd endpointへ毎回`FD_GET_INFO`していた。初期化を実際に読むrequest headerへ限定し、endpointはprocess初回だけ検証するようにした。fork childでは継承cacheを明示破棄し、別thread由来の`page_busy`をslow pathへ持ち越さない。kernelと番号付きABIは変更していない。

同じ4 CPU・5秒scenarioを1回実行した。raw logは`.artifacts/phase4-critical-path/4cpu-after-lpr-socket-hotpath/`に保存した。

| 指標 | 最適化1後 | 最適化2後 | 差 |
|---|---:|---:|---:|
| animation FPS | 8.545 | 10.412 | +21.8% |
| frame interval p50 | 108 ms | 95 ms | -13 ms |
| frame interval p99 / max | 132 / 132 ms | 106 / 107 ms | -26 / -25 ms |
| client flush + Sway + wlroots render p50 / p99 | 42 / 52 ms | 35 / 39 ms | -7 / -13 ms |
| wlroots read -> presentation feedback p50 / p99 | 70 / 76 ms | 56 / 62 ms | -14 / -14 ms |
| draw -> feedback p50 / p99 | 116 / 129 ms | 93 / 104 ms | -23 / -25 ms |
| commit -> frame callback p50 / p99 | 77 / 87 ms | 62 / 71 ms | -15 / -16 ms |
| IRQ-ready -> publish p50 / p99 | 17 / 17 ms | 12 / 12 ms | -5 / -5 ms |
| publish -> libinput read p50 / p99 | 50 / 50 ms | 8 / 23 ms | -42 / -27 ms |
| libinput read -> Wayland client p50 / p99 | 184 / 186 ms | 46 / 48 ms | -138 / -138 ms |
| IRQ-ready -> Wayland client p50 / p99 / max | 251 / 253 / 288 ms | 68 / 73 / 99 ms | -183 / -180 / -189 ms |

入力はrun間変動があるため最適化1後だけとの比較では過大評価になり得る。ただし旧実装の良好run 97 / 101 / 140 msと比較しても、最適化2後のp50 / p99 / maxは68 / 73 / 99 msまで縮んだ。frame側もp99が132 msから106 msへ縮み、socket RPC固定費がframeとinputの両経路に入っていたことを確認できた。

## 最適化3 — netd RPC pageのpersistent mapping

LPRは64 KiB pageをprocess内で再利用していたが、各SEND / RECV / POLLで同じpage FDをnetdへ複製していた。netdはRPCごとにpageをmapし、完了後にunmap / closeしていた。

`PAGE_ATTACH`をHELLO直後へ挿入し、後続opを一括renumberした。LPRはpage VMOとleaseをprocessごとに一度だけ渡し、通常RPCはattachment IDだけを渡す。netdは動的recordへmappingを保持し、lease HUP時にunmap / closeする。fork childは親attachmentを破棄してlazy reattachする。kernelは変更していない。

ABI切替時にbootfsのnetdとinputd、rootfsのLPRを同時更新する必要がある。inputdの`UEVENT_PUBLISH`も同じABIのproducerなので、`pack.yaml`へnetd protocol headerのrebuild依存を追加した。互換層は残していない。

同じ4 CPU・5秒scenarioを1回実行した。raw logは`.artifacts/phase4-critical-path/4cpu-after-netd-page-attach/`に保存した。

| 指標 | 最適化2後 | 最適化3後 | 差 |
|---|---:|---:|---:|
| animation FPS | 10.412 | 11.086 | +6.5% |
| frame interval p50 | 95 ms | 88 ms | -7 ms |
| frame interval p99 / max | 106 / 107 ms | 97 / 98 ms | -9 / -9 ms |
| client flush + Sway + wlroots render p50 / p99 | 35 / 39 ms | 33 / 39 ms | -2 / 0 ms |
| wlroots read -> presentation feedback p50 / p99 | 56 / 62 ms | 51 / 54 ms | -5 / -8 ms |
| draw -> feedback p50 / p99 | 93 / 104 ms | 86 / 95 ms | -7 / -9 ms |
| commit -> frame callback p50 / p99 | 62 / 71 ms | 59 / 71 ms | -3 / 0 ms |
| IRQ-ready -> publish p50 / p99 | 12 / 12 ms | 8 / 8 ms | -4 / -4 ms |
| publish -> libinput read p50 / p99 | 8 / 23 ms | 43 / 43 ms | +35 / +20 ms |
| libinput read -> Wayland client p50 / p99 | 46 / 48 ms | 179 / 181 ms | +133 / +133 ms |
| IRQ-ready -> Wayland client p50 / p99 / max | 68 / 73 / 99 ms | 230 / 232 / 263 ms | +162 / +159 / +164 ms |

frameはFPSとtailがともに改善し、netd側map / FD churnがframe経路の固定費だったことを確認した。入力は同じrunでlibinput timer lag warningが出て大幅に悪化したため、最適化3による入力改善とは判定しない。

## 最適化4 — Unix socket poll RPCのnative readiness化

Waylandのepoll初回scanは、各Unix socketについてnetdへ`POLL` RPCを発行していた。emptyからreadableへの通知FDはlevel-readyを保持するため、初回scanも非破壊native pollで判定できる。AF_UNIXだけをcached readinessへ切り替え、AF_INET / NETLINKは引き続きnetdを権威とした。待機直前とのraceはgeneric wait graphが拾い、read側はdrain前にhintをclearする。

同じ4 CPU・5秒scenarioを1回実行した。raw logは`.artifacts/phase4-critical-path/4cpu-after-native-unix-poll/`に保存した。libinput lag、Broken pipe、scheduler unavailableは出ていない。

| 指標 | 最適化3後 | 最適化4後 | 差 |
|---|---:|---:|---:|
| animation FPS | 11.086 | 15.323 | +38.2% |
| frame interval p50 | 88 ms | 66 ms | -22 ms |
| frame interval p99 / max | 97 / 98 ms | 77 / 78 ms | -20 / -20 ms |
| client flush + Sway + wlroots render p50 / p99 | 33 / 39 ms | 21 / 25 ms | -12 / -14 ms |
| wlroots read -> presentation feedback p50 / p99 | 51 / 54 ms | 40 / 43 ms | -11 / -11 ms |
| draw -> feedback p50 / p99 | 86 / 95 ms | 65 / 71 ms | -21 / -24 ms |
| commit -> frame callback p50 / p99 | 59 / 71 ms | 47 / 52 ms | -12 / -19 ms |
| IRQ-ready -> publish p50 / p99 | 8 / 8 ms | 11 / 16 ms | +3 / +8 ms |
| publish -> libinput read p50 / p99 | 43 / 43 ms | 32 / 32 ms | -11 / -11 ms |
| libinput read -> Wayland client p50 / p99 | 179 / 181 ms | 42 / 43 ms | -137 / -138 ms |
| IRQ-ready -> Wayland client p50 / p99 / max | 230 / 232 / 263 ms | 85 / 86 / 107 ms | -145 / -146 / -156 ms |

frameとinputの両方が同時に縮んだため、権威確認そのものではなく「通知済みUnix socketに対する重複RPC」が共通固定費だったと判断する。入力tailは初期目標p99 32 msをまだ54 ms超過しているので、改善済みとはするが完了条件には達していない。

## 現在の残区間

| 区間 | p50 ms | p99 ms | 次の確認点 |
|---|---:|---:|---|
| client flush + Sway + wlroots render | 21 | 25 | LPR SEND、netd enqueue、Sway renderを分離する |
| wlroots read -> presentation feedback | 40 | 43 | Wayland enqueue、server flush、client RECVを分離する |
| publish -> libinput read | 32 | 32 | inputd reply、libinput dispatchを分離する |
| libinput read -> Wayland client | 42 | 43 | Sway handler、Wayland enqueue、server flushを分離する |

DRM command群は引き続き各1 ms以下である。次は小変更をまとめ、netd Unix data pathのO(32) handle探索、active全slot走査、partial-read `memmove`を一括で除去する。その後も40 ms台が残る場合だけ、wlroots内部境界を追加計測する。
