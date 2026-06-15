---
tags:
  - pachaos
  - abi
  - ipc
  - fd
  - phase0
---

# IPC ABI Spec Draft

## 目的

PachaOS native IPC を fd-based に再設計する。

IPC は fd passing を標準機能として持つ。kernel は message payload の意味を知らない。

## 基本モデル

```text
Endpoint fd = message queue / port
Channel fd  = connected bidirectional session
Reply fd    = one-shot reply authority
Event fd    = wait / signal primitive
```

FS、net、tty、process service などは userland protocol として実装する。

kernel から見ると、それらは endpoint / channel fd に過ぎない。

## 操作

Phase 4 の最小 syscall。

```text
ipc_endpoint_create(rights, flags) -> endpoint_fd
ipc_channel_create(out_pair, rights, flags) -> status
ipc_send(fd, message)
ipc_recv(fd, message_buf)
ipc_call(fd, request) -> reply_fd
ipc_reply(reply_fd, reply)
```

`ipc_call` は one-shot `Reply fd` を作り、receiver に send-only Reply fd を添付する。

## Message Format

C ABI で扱いやすい形にする。

```c
struct pacha_ipc_iov {
    void *base;
    size_t len;
};

struct pacha_ipc_fd {
    int fd;
    uint64_t rights;
    uint32_t flags;
};

struct pacha_ipc_msg {
    uint64_t word0;
    uint64_t word1;
    uint64_t word2;
    uint64_t word3;
    struct pacha_ipc_fd *fds;
    uint64_t fd_count;
    uint64_t fd_capacity;
    uint64_t flags;
};
```

受信時は同じ構造体に受信用 buffer と fd array を渡す。

Phase 4 では inline payload は 4 words に固定する。large payload は VMO fd / fast IPC data plane に載せる。

## Atomicity

IPC は次を atomic に扱う。

- payload copy
- fd transfer
- reply fd creation
- receive queue enqueue

途中失敗した場合、受信側に半端な fd が残ってはいけない。

## fd transfer

fd transfer は rights attenuation を必須にする。

```text
received_rights <= sender_fd_rights
```

transfer flags。

| Flag | 意味 |
|---|---|
| `COPY` | 送信側 fd を残す |
| `MOVE` | 成功時に送信側 fd を close する |
| `CLOEXEC` | 受信 fd に CLOEXEC を付ける |
| `NONBLOCK` | 受信 fd に NONBLOCK を付ける |

`COPY` / `MOVE` の両方で sender fd の `TRANSFER` right が必要。

## Blocking

blocking policy は fd flag と operation flag で制御する。

- fd が `NONBLOCK` なら既定で nonblocking
- operation flag で一回だけ blocking / nonblocking を上書きできる
- timeout は absolute tick か relative ns のどちらかに統一する

Phase 0 では time unit を決める。

## Readiness

`poll` / `wait` のため、object は readiness bit を返す。

```text
READABLE
WRITABLE
HANGUP
ERROR
PEER_CLOSED
SIGNALLED
```

`Channel` は peer close を検出できる必要がある。

## libipc

userland は原則 syscall を直接叩かず、`libipc` を使う。

`libipc` は C で実装し、C ABI として提供する。musl backend、service daemon、kobox backend から同じ header を使う。

public API 案。

```c
typedef int ipc_fd_t;

int ipc_send(
    ipc_fd_t fd,
    const void *data,
    size_t data_len,
    const struct pacha_ipc_fd *fds,
    size_t fd_count,
    uint64_t flags
);

int ipc_recv(
    ipc_fd_t fd,
    void *data,
    size_t data_cap,
    size_t *data_len,
    struct pacha_ipc_fd *fds,
    size_t *fd_count,
    uint64_t flags
);

int ipc_call(
    ipc_fd_t fd,
    const void *req,
    size_t req_len,
    const struct pacha_ipc_fd *send_fds,
    size_t send_fd_count,
    void *reply,
    size_t reply_cap,
    size_t *reply_len,
    struct pacha_ipc_fd *recv_fds,
    size_t *recv_fd_count,
    uint64_t flags
);
```

## Normal IPC と Fast IPC

`libipc` は同じ意味論で複数 backend を選ぶ。

```text
normal IPC = control plane
fast IPC   = data plane
```

normal IPC に寄せるもの。

- 初回接続
- protocol negotiation
- fd passing
- rights transfer
- debug / trace
- rare operation

fast IPC に寄せるもの。

- fixed request / completion ring
- high frequency operation
- fd passing を伴わない data path
- block / net / console / GUI / audio

## pkey Fast IPC

原則。

```text
fd = authority / lifetime / transfer
pkey = fast dataplane access control
```

pkey は authority ではない。

x86 PKU / PKRU は user mode で変更できるため、強い security boundary として扱わない。

untrusted peer 間 IPC では、pkey ではなく fd rights / VMO fd / VMA/PTE permission を境界にする。

詳細は [Phase 5 fast IPC / pkey threat model](./phase5-fast-ipc-pkey-threat-model.md) に固定する。

Fast IPC channel は次を持つ。

```text
channel_fd
  -> shared VMO request ring
  -> shared VMO completion ring
  -> optional data buffer VMO
  -> event / doorbell fd
```

setup は normal IPC で行う。

hot path は `libipc` が pkey access window を開いて ring を操作する。

## Feature Negotiation

接続時に feature を交換する。

```text
IPC_FEATURE_FD_PASSING
IPC_FEATURE_SHARED_VMO
IPC_FEATURE_FAST_RING
IPC_FEATURE_PKEY
IPC_FEATURE_DOORBELL
IPC_FEATURE_TRACE
```

未対応の場合は normal IPC に fallback する。

## Phase 0 決定事項

- Endpoint / Channel / Reply の正確な分離
- `MOVE` transfer に `TRANSFER` right を要求するか
- timeout unit
- max inline payload size
- max fd transfer count
- reply fd の lifetime
- fast IPC setup protocol
- pkey 非対応 CPU での ABI 挙動
