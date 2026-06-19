---
tags:
  - pachaos
  - phase4
  - ipc
  - fd
---

# Phase 4 FD Passing IPC

Phase 4 の目的は、normal IPC を fd-based object model に載せること。

fast IPC は Phase 5 に分ける。Phase 4 では control plane と fd passing を安定させる。

## 固定する意味論

kernel object。

| object | 役割 |
|---|---|
| `Endpoint fd` | message queue / service port |
| `Channel fd` | connected pair。片側 send は peer 側 recv queue に入る |
| `Reply fd` | one-shot reply authority。`ipc_call` が作り、receiver に send-only fd として渡す |

kernel は file / socket / path semantics を持たない。message payload は小さい inline words と transferred fd だけを見る。

## syscall

Phase 4 の native IPC syscall range は `0x140..0x145`。

| syscall | number | 意味 |
|---|---:|---|
| `ipc_endpoint_create` | `0x140` | Endpoint fd を作る |
| `ipc_channel_create` | `0x141` | Channel pair fd を作る |
| `ipc_send` | `0x142` | Endpoint / Channel / Reply に message を送る |
| `ipc_recv` | `0x143` | Endpoint / Channel / Reply から message を受ける |
| `ipc_call` | `0x144` | request に one-shot Reply fd を添付して送る |
| `ipc_reply` | `0x145` | Reply fd に one-shot reply を送る |

## message ABI

Phase 4 の normal IPC payload は 4 words に制限する。

```c
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

`fd_count` は send 時は送信 fd 数、recv 時は実際に受け取った fd 数になる。recv 前に `fd_capacity` を設定する。

fd item。

```c
struct pacha_ipc_fd {
    uint64_t fd;
    uint64_t rights;
    uint64_t flags;
    uint64_t transfer_flags;
};
```

## fd passing

fd passing は、受信側 fd table に半端な fd を残さない形で扱う。

- queue に入る fd は `KernelObjectRef + attenuated rights + flags`
- send 時に object ref を retain する
- recv 成功時に受信 process の fd table に install する
- install 後、queue 側の retain を release する
- 途中失敗時、受信側に半端な fd を残さない

sender fd には `TRANSFER` right が必要。これは `COPY` / `MOVE` の両方で必要。

`received_rights <= sender_rights` でなければならない。

## Reply fd

`ipc_call` は caller に recv-only Reply fd を返し、receiver には send-only Reply fd を message の fd array として渡す。

receiver は `ipc_reply(reply_fd, msg)` を一度だけ呼べる。二回目以降は invalid。

caller は返ってきた Reply fd で `ipc_recv(reply_fd, ...)` する。

Phase 4 では blocking wait を libc / `libipc` の retry policy に寄せ、kernel syscall は nonblocking primitive として成立させる。

## Channel fd

`ipc_channel_create` は同じ process に pair を返す。

片側 fd に `ipc_send` すると、peer side の recv queue に入る。

Channel fd を別 process に渡すことで bidirectional session を作る。

## libipc

`libipc` は C で提供する。

Phase 4 の `libipc` は thin wrapper とする。

- syscall number / struct layout を C header に固定する
- normal IPC syscall を直接 wrap する
- retry / blocking / fast backend selection は後続で拡張する

## 旧 IPC との関係

既存の endpoint id / page cap / ipc-buffer cap transfer はすぐには消さない。

ただし新規設計の本命は native IPC fd であり、旧 path は Phase 4 後の移行対象として扱う。

VMO fd passing により、Phase 3 で未完成だった FS server の exec VMO 共有を自然に実装できる土台ができる。

## 完了条件

- `Endpoint` / `Channel` / `Reply` が fd object kind になっている
- native IPC syscall range が ABI に入っている
- fd passing が rights attenuation を強制する
- `MOVE` transfer が送信元 fd を閉じる
- `ipc_call` が one-shot Reply fd を作る
- `libipc` の C header / source がある
- kernel unit test が normal IPC / fd passing / call-reply を検証する
