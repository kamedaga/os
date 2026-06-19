---
tags:
  - pachaos
  - phase5
  - ipc
  - pkey
  - security
---

# Phase 5 Fast IPC / pkey Threat Model

Phase 5 の目的は、pkey / shared memory を使った fast IPC を最初から実装して試すことである。

Phase 4 で normal IPC / fd passing はできているため、Phase 5 ではそれを control plane と fallback に使い、pkey shared-memory backend を攻める。

ただし PoC の目的は raw pkey を untrusted security boundary として正当化することではない。Phase 5 は trusted 別 process の fast IPC に集中し、pkey access window と shared VMO ring の性能・実装形を確かめる。

結論。

```text
untrusted peer 間 IPC:
  OK: fd rights + VMO/VMA/PTE permission を境界にする
  OK: pkey を local fast-path guard として併用する
  NG: pkey だけを権限境界にする

untrusted code を同一 address space に置く IPC:
  Phase 5 では対象外
  理由: byte pattern 検査や loader / verifier など userland runtime の負担が大きい
```

## pkey の性質

x86 PKU / pkey は、page table entry に 4-bit key を付け、thread-local な PKRU で data access を抑制する仕組みである。

重要な性質。

- key は最大 16 domain
- PKRU は thread-local
- user mode が `WRPKRU` で変更できる
- data access に効き、instruction fetch には効かない
- 通常 page permission を緩めるのではなく、追加で制限する
- kernel が PKRU state を保存復元する必要がある

したがって pkey は高速な permission switch としては良いが、untrusted code が任意に `WRPKRU` や PKRU restore path を実行できる場合、pkey 自体は authority にならない。

Phase 5 は trusted 別 process IPC を対象にするため、この限界を前提として受け入れる。untrusted 同一 address space isolation は扱わない。

## 信頼境界の置き場所

PachaOS の原則。

```text
fd rights = authority
VMO fd    = backing object / sharing authority
VMA/PTE   = process address-space enforcement
pkey      = local access window / fast-path guard
```

pkey は fd rights を置き換えない。

pkey は VMA/PTE permission を置き換えない。

pkey は normal IPC の fd passing / rights attenuation を置き換えない。

## 別 process の untrusted peer 間で成立する形

peer が別 process であるなら、security boundary は address space と PTE permission に置く。

fast IPC は次の構成にする。

```text
Channel fd
  -> normal IPC control plane
  -> request ring VMO
  -> completion ring VMO
  -> optional data VMO
  -> optional doorbell/event fd
```

untrusted 構成では writable mapping を分ける。

```text
client process:
  request ring    writable
  completion ring read-only

server process:
  request ring    read-only
  completion ring writable
```

同じ ring page を両 peer に writable map しない。

片側が壊せるのは、自分が producer である ring に限定する。consumer は sequence, length, offset, generation を検証する。

この構成では pkey がなくても untrusted peer 間 IPC として成立する。ここでの pkey は以下のために使う。

- `libipc` が ring access window を短くする
- server 内の他の code path が誤って ring を触るのを抑える
- debug / race 検出
- trusted runtime 内での hot path phase control

## pkey-only ownership transfer は採用しない

次の設計は採用しない。

```text
shared VMO data buffer を両 peer に writable map
ownership transfer を pkey write-disable だけで表現
```

理由。

- untrusted process は自分の PKRU を変更できる
- 同じ process 内の untrusted code は `WRPKRU` を実行できる
- PKRU は process 全体の authority ではなく thread-local state
- sigreturn / extended state restore / debugger / copy syscall 類が設計によって bypass surface になる

ownership transfer を security property にするなら、kernel が page permission / VMA permission / mapping generation を更新する必要がある。

ただし、trusted runtime 内の optimization としては pkey-only handoff を許してよい。

## 同一 address space の untrusted code

同一 address space 内に untrusted plugin / JIT / wasm / driver-like code を置く場合、raw pkey だけでは隔離にならない。

成立させるには loader / verifier / trusted gate が必要になる。

必要条件。

- untrusted binary / JIT code から任意の `WRPKRU` を除去または trap する
- domain switch は trusted gate sequence だけで行う
- `XRSTOR` など PKRU を変更できる state restore 経路を制御する
- signal / exception return が PKRU を任意復元できない
- kernel の copy / debug / inspect syscall が pkey-protected region を bypass しない
- code generation を W^X にする
- call gate 以外で pkey window を開けない
- pkey state を thread migration / context switch / fork / exec で正しく初期化する

この方式は userland runtime の責務が大きく、Phase 5 の本命から外す。

PachaOS の Phase 5 は trusted 別 process 向け `pkey_ring` backend に集中する。

## Phase 5 の設計判断

Phase 5 の実装順。

1. `libipc` に pkey shared-memory backend を追加する
2. shared VMO request/completion ring を実際に動かす
3. pkey access window を hot path に入れる
4. normal IPC を setup / fallback / fd passing control plane として維持する
5. QEMU smoke と簡易 benchmark を作る
6. pkey unavailable / invalid mapping / fallback の negative tests を作る

最初に使う syscall / ABI は、既存 IPC と VMO fd を土台にする。kernel に fast IPC 専用 syscall を増やさず、`libipc` が setup と backend 選択を持つ。

```text
pacha_ipc_fast_channel_offer(control_fd, flags, pkey)
pacha_ipc_fast_channel_accept(control_fd, flags, pkey)
pacha_ipc_fast_channel_init_normal(channel_fd)
pacha_ipc_fast_channel_ready(channel)
pacha_ipc_fast_channel_uses_ring(channel)
pacha_ipc_fast_entry_init(entry, op, offset, len, flags)
pacha_ipc_fast_send(channel, entry)
pacha_ipc_fast_recv(channel, entry)
pacha_ipc_fast_call(channel, request, response)
pacha_ipc_fast_serve_once(channel, handler, ctx)
```

fast channel の setup は normal IPC で行う。fd passing は control plane のまま維持する。

`libipc` は raw syscall wrapper ではなく、backend 選択、pkey access window、request/completion ring の向き、normal fallback、request/reply helper を持つ層にする。

Phase 5 の最初の成果は、pkey shared-memory backend が QEMU 上で動き、normal IPC fallback と同じ `libipc` API から使えることである。

trusted 別 process E2E のため、process builder は suspended child に attenuated fd を渡せる必要がある。

```text
process_builder.transfer_fd_to_process(token, source_fd, min_child_fd, rights, flags)
```

この ABI は fast IPC 専用ではない。child に最初の control channel fd を渡し、その後の fast channel setup は normal IPC の fd passing で進める。
process builder API は `libipc` には入れず、loader / process builder library の境界に置く。

## Ring の最小ルール

ring は data plane であり、authority を持たない。

ring entry は fd を含まない。

ring entry は kernel object を直接参照しない。

ring entry は次を持つ。

```text
op
seq
generation
offset
len
flags
status
```

consumer は必ず検証する。

- `len <= mapped buffer size`
- `offset + len` overflow なし
- `generation` が現在の mapping と一致
- `op` が negotiated protocol 内
- producer が書ける ring から来た entry である

large data は data VMO の offset / len で参照する。

data VMO を untrusted peer に writable share する場合は、破壊されてもよい buffer として扱う。破壊されて困る ownership transfer は kernel-enforced mapping change を必要とする。

## pkey backend の位置づけ

pkey backend は `libipc` の backend であり、ABI の必須条件ではない。

```text
normal backend
shared_vmo_ring backend
```

最初に実装するのは controlled runtime 向けの `pkey_ring` backend である。

```text
pkey_ring backend
```

`pkey_ring` は次の条件を満たす時だけ選ぶ。

- CPU が PKU / OSPKE を持つ
- kernel が CR4.PKE を有効化し、PTE pkey bits を map できる
- process が pkey mapping を opt-in している
- fallback backend が常にある

pkey unavailable の場合は `shared_vmo_ring` か `normal` に落ちる。

kernel は thread context に PKRU を保存し、trap / syscall / timer preemption / AP user entry の user return で復元する。これにより、`libipc` が hot path で開いた `WRPKRU` window はその thread の状態として保存され、別 thread / 別 process へ漏れない。

`libipc` は ring 操作の直前だけ pkey access window を開き、操作後に元の PKRU に戻す。pkey unavailable の場合は `shared_vmo_ring` へ、ring setup が成立しない場合は `normal` backend へ落とす。

## 決定

PachaOS では、pkey を untrusted IPC の主境界にしない。

別 process 間の untrusted IPC は fd rights / VMO fd / VMA/PTE permission で成立させる。

同一 process 内の untrusted isolation は Phase 5 では扱わない。

pkey は `libipc` backend の一つであり、backend 選択時に trust model を満たす場合だけ有効にする。

## 参考

- [Linux kernel documentation: Memory Protection Keys](https://www.kernel.org/doc/html/v6.5/core-api/protection-keys.html)
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): PKU / PKRU / WRPKRU
- [Park et al., "Memory Protection Keys: Facts, Key Extension Perspectives, and Discussions"](https://gts3.org/assets/papers/2023/park%3Ampkfacts.pdf)
