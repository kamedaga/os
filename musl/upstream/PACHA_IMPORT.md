# musl upstream import

この directory は musl upstream source tree のコピーである。

## Imported Source

- version: 1.2.6
- source archive: https://musl.libc.org/releases/musl-1.2.6.tar.gz
- sha256: `d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a`
- official cgit tag: https://git.musl-libc.org/cgit/musl/tag/?h=v1.2.6
- imported on: 2026-06-17

## Local Policy

- `COPYRIGHT` は upstream の license 正本としてそのまま保持する。
- `README`, `VERSION`, `INSTALL`, `Makefile`, `configure`, `arch/`, `crt/`, `include/`, `src/`, `ldso/`, `tools/` は upstream source tree として扱う。
- PachaOS 固有 glue は原則 `../pachaos/` に置く。
- upstream tree に直接差分を入れる場合は、差分理由を `../pachaos/README.md` または `../pachaos/patches/` に記録する。
