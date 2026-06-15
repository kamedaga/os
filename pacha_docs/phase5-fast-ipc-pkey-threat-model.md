---
tags:
  - pachaos
  - phase5
  - ipc
  - pkey
  - security
---

# Phase 5 Fast IPC / pkey Threat Model

Phase 5 の目的は fast IPC を入れることだが、pkey を security boundary として扱うかは別問題である。

結論。

```text
untrusted peer 間 IPC:
  OK: fd rights + VMO/VMA/PTE permission を境界にする
  OK: pkey を local fast-path guard として併用する
  NG: pkey だけを権限境界にする

untrusted code を同一 address space に置く IPC:
  NG: raw pkey だけでは不可
  条件付き OK: WRPKRU / XRSTOR / sigreturn / syscall surface を制御する confined runtime がある場合のみ
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

したがって pkey は高速な permission switch としては良いが、untrusted code が任意命令を実行できる場合、pkey 自体は authority にならない。

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

## untrusted peer 間で成立する形

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

この構成では pkey がなくても untrusted peer 間 IPC として成立する。pkey は以下のために使う。

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

成立させるには confined runtime が必要。

必要条件。

- untrusted binary / JIT code から `WRPKRU` を除去または trap する
- `XRSTOR` など PKRU を変更できる state restore 経路を制御する
- signal / exception return が PKRU を任意復元できない
- kernel の copy / debug / inspect syscall が pkey-protected region を bypass しない
- code generation を W^X にする
- call gate 以外で pkey window を開けない
- pkey state を thread migration / context switch / fork / exec で正しく初期化する

PachaOS の Phase 5 では、この confined runtime を必須にしない。

したがって Phase 5 で対象にするのは **別 process 間の untrusted peer IPC** であり、同一 process 内 untrusted sandbox は後続設計とする。

## Phase 5 の設計判断

Phase 5 の実装順。

1. pkey なし shared VMO ring
2. fd rights / VMA permission による directional mapping
3. normal IPC による setup / teardown / fallback
4. `libipc` backend abstraction
5. pkey opt-in backend
6. confined runtime の検討

最初に作る syscall / ABI は pkey 非依存にする。

```text
ipc_fast_channel_create(channel_fd, flags) -> fast_channel_fd
ipc_fast_channel_get_info(fast_channel_fd, out_info)
ipc_fast_channel_doorbell(fast_channel_fd, flags)
```

fast channel の setup は normal IPC で行う。fd passing は control plane のまま維持する。

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
pkey_ring backend
```

`pkey_ring` は次の条件を満たす時だけ選ぶ。

- CPU が PKU / OSPKE を持つ
- kernel が PKRU save/restore を実装している
- process が pkey mapping を opt-in している
- fallback backend が常にある

pkey unavailable の場合は `shared_vmo_ring` か `normal` に落ちる。

## 決定

PachaOS では、pkey を untrusted IPC の主境界にしない。

untrusted IPC は fd rights / VMO fd / VMA/PTE permission で成立させる。

pkey は fast IPC の local optimization としてのみ導入する。

同一 address space の untrusted confinement は、Phase 5 本体から切り離して別途 `confined runtime` として扱う。

## 参考

- [Linux kernel documentation: Memory Protection Keys](https://www.kernel.org/doc/html/v6.5/core-api/protection-keys.html)
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): PKU / PKRU / WRPKRU
- [Park et al., "Memory Protection Keys: Facts, Key Extension Perspectives, and Discussions"](https://gts3.org/assets/papers/2023/park%3Ampkfacts.pdf)
