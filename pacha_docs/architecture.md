# PachaOS の設計思想

PachaOS は、Linux を小さな別世界として載せる OS ではありません。
native application も Linux application も、kernel から見れば同じ
**PachaOS process** です。process は必要な FD capability だけを受け取り、
その範囲内で動きます。

## Linux application も普通の PachaOS process

Linux application の address space には Linux ELF と LPR (Linux
Personality Runtime) がロードされます。

```text
PachaOS process
  ├─ application のコードとデータ
  ├─ 動的リンクされた LPR
  └─ 必要最小限の native FD capabilities
```

Linux 専用の process type、Linux syscall mode、特権的な Linux kernel
server はありません。LPR は zpoline で Linux の `syscall` 命令を process
内の関数呼び出しへ変換し、native syscall と userland service を使って
Linux ABI を実装する、非特権の userland binary です。

LPR は kernel の一部でも、kernel に特別扱いされる trusted monitor でも
ありません。application が LPR を壊したり、LPR を通らず native syscall
を直接呼んだりしても、新しい権限は得られません。kernel が操作ごとに
native FD capability の rights を検査するからです。

この分離により、Linux ABI の正しさと OS の権限境界を同じものとして
扱わずに済みます。LPR は交換可能な syscall 抽象化であり、authority の
根は常に kernel の FD capability です。

## 三つの FD / object 層

Linux compatibility の内部では、似て見える三つのものを意図的に分けて
います。

| 層 | 役割 |
|---|---|
| LPR logical FD / OFD | Linux の fd 番号、`dup`、共有 offset、flags を表す |
| native FD capability | process が OS object に対して行使できる権限を表す |
| service の opaque handle / lease | filed、termd、netd などが所有する object の実体と寿命を表す |

logical FD の番号を知ること、native FD を直接操作すること、service handle
を持つことは同じではありません。capability の複製・移送では rights を
増やせず、service 内部の object は opaque handle と lease を通して管理
されます。

## 分離するのは「機能」ではなく「所有と回復の単位」

PachaOS は、機能名が違うという理由だけで全てを別 process にしません。
process 境界を置く基準は次の二点です。

1. 状態と capability の所有者が異なるか。
2. 片方だけを停止・再起動・存続させることに実際の意味があるか。

TTY、network、display、input は、それぞれ独立した device capability、
状態、object lifetime を持つため、`termd`、`netd`、`drmd`、`inputd` に
分けます。一方、通常の rootfs 経路は次の依存鎖です。

```text
filed の VFS / exec
  → filesystem backend
    → block layer
      → device driver
```

filesystem と block/driver が失われた状態で filed だけを残しても、rootfs
service は継続できません。handle の復元や transaction recovery を持たずに
process だけを分けても、障害耐性は増えず、IPC と同期だけが増えます。
そのため現在の storage 用 Kobox runtime は `filed.elf` にリンクされて
います。

ただし、これは責務を融合したという意味ではありません。

- filed は namespace、vnode、open-file-description、path policy、exec policy
  を所有します。
- Kobox storage runtime は Linux filesystem/block/driver を動かし、backend
  object を提供します。
- filed core に Linux の `inode`、`dentry`、`file` を漏らさず、両者は
  backend interface で分けます。

つまり **component の境界** と **process の配置** は別の判断です。将来、
独立再起動や状態復元に意味を持たせられるなら、同じ interface を process
境界として使うこともできます。

## kernel に policy を入れない

kernel は process、thread、memory、IPC、wait、FD capability などの普遍的な
機構を提供します。VFS、pathname、Linux fd、Linux syscall、filesystem、
driver の policy は userland に置きます。

`filed` の exec は標準の pathname-based exec policy ですが、kernel 固有の
唯一の exec 機構ではありません。別の loader や namespace は、同じ native
process / memory / FD primitives を使って独立に実装できます。同様に LPR
以外の ABI personality も、kernel に新しい process 種別を追加せず作れます。

PachaOS の要点は、次の一文にまとまります。

> 互換性や driver policy を特権化せず、最小 FD capability を持つ普遍的な
> PachaOS process と userland component の組み合わせとして構成する。

## 関連文書

- [filed VFS design](./filed-vfs-design.md)
- [LPR state design](./lpr-state-design.md)
- [LPR FD operations and lifecycle](./fd-ops-design.md)
- [Userland service ABI](./userland-service-abi.md)

by codex5.6 Sol
悪くない説明..!🤗