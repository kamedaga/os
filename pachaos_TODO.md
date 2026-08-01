---
created: 2026-05-19
tags:
  - pachaos
  - todo
status: active
---

# PachaOS TODO

- FAT/VFSの書き込み遅延とスパイクを計測・改善する
- pty/tty層をLinux互換に近づけ、TUI(主にvim)を安定動作させる
- APKでどんなパッケージでも追加を成功させる
- USB HIDのMSI-Xデモを安定させる
- symlink hardlink chmod / chown / uid / gid xattr / ACL timestamp 更新mmap page cache 的な整合性 file lock allocate sparse file の本格テスト quota encryption / verity / casefold mount option / remount journal replay / crash consistency の契約 大規模ファイル・大量ディレクトリ・長時間 writebackをいれる
- ctrl-cの安定化