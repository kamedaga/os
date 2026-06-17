echo basic-ok
x=var-ok
echo $x
for x in for-a for-b; do echo $x; done
case word in word) echo case-ok;; *) echo case-bad;; esac
f(){ echo func-ok; }
f
cd /cmd && pwd
y=$(echo subst-ok)
if [ "$y" = subst-ok ]; then echo subst-ok; else echo subst-bad; fi
true && echo and-ok || echo and-bad
false || echo or-ok
[ x = x ] && echo test-ok || echo test-bad
(echo subshell-ok)
gok=0
for g in /cmd/*.elf; do case $g in /cmd/dash.elf) gok=1;; esac; done
[ "$gok" = 1 ] && echo glob-ok || echo glob-bad
echo hidden > /dev/null && echo redirect-ok || echo redirect-bad
echo file-ok > /tmp/ds
read z < /tmp/ds
[ "$z" = file-ok ] && echo file-rw-ok || echo file-rw-bad
cd /tmp && [ "$(pwd)" = /tmp ] && echo cwd-ok || echo cwd-bad
echo rel-ok > r
read rz < r
[ "$rz" = rel-ok ] && echo rel-rw-ok || echo rel-rw-bad
cat /tmp/ds > /tmp/bc
echo uutils-cat-cmd-returned
read ucat < /tmp/bc
[ "$ucat" = file-ok ] && echo uutils-cat-ok || echo uutils-cat-bad
cd /cmd
echo fat-ok > /share/f
read fz < /share/f
[ "$fz" = fat-ok ] && echo fat-rw-ok || echo fat-rw-bad
cat /share/f > /tmp/bf
read ufat < /tmp/bf
[ "$ufat" = fat-ok ] && echo uutils-fat-cat-ok || echo uutils-fat-cat-bad
true && echo uutils-true-ok || echo uutils-true-bad
false || echo uutils-false-ok
/cmd/zstd.elf --version >/dev/null && echo zstd-version-ok || echo zstd-version-bad
/cmd/zstd.elf -q -f -T4 /tmp/ds -o /tmp/zstd.zst && echo zstd-compress-ok || echo zstd-compress-bad
/cmd/zstd.elf -q -d -f /tmp/zstd.zst -o /tmp/zstd.out && echo zstd-decompress-ok || echo zstd-decompress-bad
read zs < /tmp/zstd.out
[ "$zs" = file-ok ] && echo zstd-roundtrip-ok || echo zstd-roundtrip-bad
/cmd/musl_smoke.elf argv-smoke && echo musl-smoke-ok || echo musl-smoke-bad
/cmd/musl_smoke.elf 4-pthread-smoke && echo four-pthread-ok || echo four-pthread-bad
/cmd/musl_smoke.elf exit-group-smoke && echo exit-group-ok || echo exit-group-bad
/cmd/capsule_demo.elf && echo capsule-demo-ok || echo capsule-demo-bad
if [ -n "${KOBOX_PACHAOS_DEVICE_FD:-}" ]; then
  /cmd/capsule_demo.elf env-query && echo capsule-device-query-ok || echo capsule-device-query-bad
  /cmd/kobox-ls-devices.elf pachaos && echo kobox-pachaos-device-ok || echo kobox-pachaos-device-bad
  if [ -r /usr/lib/kobox/nvme-auth.ko ] && [ -r /usr/lib/kobox/nvme-core.ko ] && [ -r /usr/lib/kobox/nvme.ko ]; then
    KOBOX_NVME_IO_SMOKE=1 /cmd/kobox-run.elf --backend=pachaos --dep=/usr/lib/kobox/nvme-auth.ko --dep=/usr/lib/kobox/nvme-core.ko run /usr/lib/kobox/nvme.ko > /tmp/kobox-nvme-rw.log 2>&1
    cat /tmp/kobox-nvme-rw.log
    if grep -q 'kobox nvme io smoke: cases=.*block-requests=ok' /tmp/kobox-nvme-rw.log && ! grep -q 'init_module returned -' /tmp/kobox-nvme-rw.log; then
      echo kobox-nvme-rw-ok
    else
      echo kobox-nvme-rw-bad
    fi
  fi
fi
echo pipe-ok | while read x; do echo $x; done
echo dash-smoke-done
