# License Tracking

この directory は `musl/` 配下の license 境界を明確にするために置く。

## musl upstream

musl upstream は MIT license で配布されている。
musl upstream の正本は upstream source tree の `COPYRIGHT` であり、`musl/upstream/` に source をコピーするときは `COPYRIGHT` を必ず保持する。

現在の import:

- version: 1.2.6
- source archive: https://musl.libc.org/releases/musl-1.2.6.tar.gz

参照:

- https://musl.libc.org/
- https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT

## PachaOS port glue

`musl/pachaos/` に置く PachaOS 固有の glue code は、repository root の `LICENSE` と同じ MIT license として扱う。

## Import Checklist

- `musl/upstream/COPYRIGHT` が存在する
- `musl/upstream/VERSION` が存在する
- import した upstream version または commit を記録する
- PachaOS 固有差分が `musl/pachaos/` または `musl/pachaos/patches/` に分離されている
