#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
alias_count="${CAPOS_CACHE_ALIAS_COUNT:-1}"
if ! [[ "$alias_count" =~ ^[1-9][0-9]*$ ]] || (( alias_count > 64 )); then
  echo "CAPOS_CACHE_ALIAS_COUNT must be between 1 and 64" >&2
  exit 2
fi
clone_count="${CAPOS_CACHE_CLONE_COUNT:-0}"
if ! [[ "$clone_count" =~ ^(0|[1-9][0-9]*)$ ]] || (( clone_count > 20 )); then
  echo "CAPOS_CACHE_CLONE_COUNT must be between 0 and 20" >&2
  exit 2
fi
active_readers="${CAPOS_CACHE_ACTIVE_READERS:-0}"
if [[ "$active_readers" != 0 && "$active_readers" != 1 ]]; then
  echo "CAPOS_CACHE_ACTIVE_READERS must be 0 or 1" >&2
  exit 2
fi
scatter_span_pages="${CAPOS_CACHE_SCATTER_SPAN_PAGES:-0}"
if ! [[ "$scatter_span_pages" =~ ^(0|[1-9][0-9]*)$ ]] || (( scatter_span_pages > 128 )); then
  echo "CAPOS_CACHE_SCATTER_SPAN_PAGES must be between 0 and 128" >&2
  exit 2
fi
out="$repo_root/.artifacts/tests/native-ram-cache-policy"
if (( alias_count != 1 )); then out+="-alias${alias_count}"; fi
if (( clone_count != 0 )); then out+="-clone${clone_count}"; fi
if (( active_readers != 0 )); then out+="-readers"; fi
if (( scatter_span_pages != 0 )); then out+="-scatter${scatter_span_pages}"; fi
cpu_model=host
case "${CAPOS_QEMU_APIC_MODE:-host}" in
  host) ;;
  xapic) cpu_model=host,-x2apic; out+="-xapic" ;;
  *) echo "CAPOS_QEMU_APIC_MODE must be host or xapic" >&2; exit 2 ;;
esac
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone \
  -O2 -Wall -Wextra -Werror -DCACHE_POLICY_ALIAS_COUNT="$alias_count" \
  -DCACHE_POLICY_CLONE_COUNT="$clone_count" \
  -DCACHE_POLICY_ACTIVE_READERS="$active_readers" \
  -DCACHE_POLICY_SCATTER_SPAN_PAGES="$scatter_span_pages" \
  -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/libipc/include" "$repo_root/tests/native_ram_cache_policy.c" \
  "$repo_root/tests/native_test_init.c" "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-e,_start,-z,noexecstack -o "$out/native_ram_cache_policy.elf"
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$repo_root/tests/native-ram-cache-policy.bootfs" --output "$out/bootfs.img")
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/native_ram_cache_policy.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
: >"$out/serial.log"
firmware_args=()
if [[ "${CAPOS_QEMU_FIRMWARE:-uefi}" == uefi ]]; then
  cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/OVMF_VARS.fd"
  firmware_args=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
                 -drive "if=pflash,format=raw,file=$out/OVMF_VARS.fd")
fi
iommu_device="${CAPOS_QEMU_IOMMU:-intel-iommu,intremap=off,aw-bits=48}"
case "$iommu_device" in
  intel-iommu,*|amd-iommu) ;;
  *) echo "Unsupported QEMU IOMMU fixture: $iommu_device" >&2; exit 2 ;;
esac
qemu-system-x86_64 -machine q35 -cpu "$cpu_model" -enable-kvm -m "${CAPOS_QEMU_RAM:-32G}" -smp 24 \
  -device "$iommu_device" \
  "${firmware_args[@]}" \
  -drive "file=$out/boot.img,format=raw,if=ide" -display none \
  -serial "file:$out/serial.log" -monitor none -no-reboot -net none >"$out/qemu.log" 2>&1 &
qemu_pid=$!
stop_qemu() {
  if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi
  wait "$qemu_pid" || true
}
trap stop_qemu EXIT
deadline=$((SECONDS + 90))
while kill -0 "$qemu_pid" 2>/dev/null && [[ "$SECONDS" -lt "$deadline" ]]; do
  if rg -q 'RAM_CACHE_POLICY=(PASS|FAIL)|ACTION=terminate|PANIC|panic|init ELF load failed|allocLowPhysicalPage for init user map failed|bootfs staging page unavailable' "$out/serial.log"; then break; fi
  sleep 0.2
done
stop_qemu
trap - EXIT
if ! rg -q 'RAM_CACHE_POLICY=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
printf 'Native 2 MiB RAM cache policy passed: %s\n' "$out/serial.log"
