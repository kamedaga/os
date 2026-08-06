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


# セキュア機能

1. CPU・メモリ保護

   - NX
   - user/kernel W^X
   - kernel text/rodata/dataのページ権限分離
   - CR0.WP
   - SMEP/SMAP/UMIP
   - stack/IST guard
   - ASLR/KASLR
   - CPU投機実行脆弱性への対応

2. 暗号学的基盤

   - 正しいentropy収集
   - CSPRNG
   - `getrandom`
   - bootごとのASLR seed
   - token、TLS、stack canary用乱数
   - 鍵を保持するmemoryの消去

3. DMA・device隔離

   - VT-d初期化
   - deviceごとのdomain
   - bus masteringをdomain構築後だけ許可
   - DMA page pin
   - IOVA割当
   - read/write direction
   - FD close、process死亡、device reset時のunmap
   - IOTLB invalidation
   - IOMMU fault処理
   - interrupt remapping
   - PCI config/MMIO権限の制限
   - USB/PCI hotplugと悪性deviceへの対応

4. kernel capabilityモデルの完全化

   - 全syscallでobject kind、owner、rightsを一貫して検査
   - dup/transfer時にrightsが増えない
   - revokeの完全性
   - process/thread/VMO/IPC/DMAのlifetime整合性
   - process終了時の全authority回収
   - FD番号再利用とgenerationの取り違え防止
   - shared VMO/COW/DMA間のrace解消
   - debug、process control、map-into権限の分離
   - resource exhaustionへのprocess別上限

5. userland capabilityモデル

   - filedの整数handleを別clientが流用できないこと
   - client指定rightsが親authorityを越えないこと
   - LPR/DRM/netd等のscalar tokenを偽造・横取りできないこと
   - endpointを知っているだけで管理操作できないこと
   - FD継承と`CLOEXEC`
   - session終了後のhandle、shared memory、transfer状態の失効
   - pid/uid/gidの自己申告を認証根拠にしない

6. サービスの侵害範囲

   - filed侵害で任意DMAできない
   - storage driver侵害でfilesystem authorityを得られない
   - net stack侵害でPCI configや他serviceへ到達できない
   - GPU/input/USB parser侵害を他serviceへ波及させない
   - 現在同居している機能は、実際のauthorityが大きすぎる境界から分離
   - daemon停止時のdevice resetと安全な再起動

7. userlandメモリ安全性

   - stack protector
   - FORTIFY
   - PIE/ASLR
   - RELRO
   - non-executable stack
   - W^X
   - ELF loaderのoverflow/range検査
   - zpoline decoder/patcherの不正入力処理
   - Kobox module loaderのrelocation、symbol、section検証
   - IPC payloadのlength/offset/overflow検査

8. 利用者・filesystem保護

   - uid/gidとsession identity
   - mode bit
   - chmod/chown
   - umask
   - ACL/xattr
   - symlink/hardlink traversal
   - mount境界
   - device node作成権限
   - executable/file mapping権限
   - Unix socket peer identityとFD transfer規則
   - quota
   - journal replayとcrash consistency

9. network境界

   - raw socket権限
   - privileged port/bind/listen権限
   - interface設定権限
   - default firewall
   - packet parserの境界検査
   - TLS用乱数
   - CA store更新
   - 時刻が不正な場合の証明書検証
   - DNS、DHCP、IPv6 control packetの攻撃面
   - socket capabilityのprocess間移譲

10. 起動・実行コードの完全性

   - UEFI Secure Boot
   - bootloader、kernel、init、bootfsの署名連鎖
   - seed0bootが起動するuserland imageの検証
   - `.ko`署名
   - rootfs integrity/verity
   - package署名
   - downgrade/rollback防止
   - 改竄時のrecovery

11. 保存データ

   - disk encryption
   - TPMまたはrecovery key
   - 鍵の分離
   - swap相当領域や一時領域の扱い
   - crash後の機密データ残存
   - backupと復旧
   - 更新時に旧鍵・旧imageへ戻らないこと

12. supply chainとrelease

   - downloadした依存のhash/commit固定
   - module、toolchain、rootfs入力の追跡
   - release artifact署名
   - 既知脆弱性を含むdependency更新
   - debug interfaceや秘密情報をreleaseから除去
   - reproducible buildは可能になった段階で導入
