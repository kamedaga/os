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
/cmd/busybox.elf cat /tmp/ds > /tmp/bc
echo busybox-cat-cmd-returned
read bbcat < /tmp/bc
[ "$bbcat" = file-ok ] && echo busybox-cat-ok || echo busybox-cat-bad
cd /cmd
echo fat-ok > /share/f
read fz < /share/f
[ "$fz" = fat-ok ] && echo fat-rw-ok || echo fat-rw-bad
/cmd/busybox.elf cat /share/f > /tmp/bf
read bbfat < /tmp/bf
[ "$bbfat" = fat-ok ] && echo busybox-fat-cat-ok || echo busybox-fat-cat-bad
/cmd/busybox.elf true && echo busybox-true-ok || echo busybox-true-bad
/cmd/busybox.elf false || echo busybox-false-ok
/cmd/zstd.elf --version >/dev/null && echo zstd-version-ok || echo zstd-version-bad
/cmd/zstd.elf -q -f -T4 /tmp/ds -o /tmp/zstd.zst && echo zstd-compress-ok || echo zstd-compress-bad
/cmd/zstd.elf -q -d -f /tmp/zstd.zst -o /tmp/zstd.out && echo zstd-decompress-ok || echo zstd-decompress-bad
read zs < /tmp/zstd.out
[ "$zs" = file-ok ] && echo zstd-roundtrip-ok || echo zstd-roundtrip-bad
/cmd/musl_smoke.elf argv-smoke && echo musl-smoke-ok || echo musl-smoke-bad
/cmd/musl_smoke.elf 4-pthread-smoke && echo four-pthread-ok || echo four-pthread-bad
/cmd/musl_smoke.elf exit-group-smoke && echo exit-group-ok || echo exit-group-bad
echo pipe-ok | while read x; do echo $x; done
echo dash-smoke-done
