#!/bin/sh
# Disposable Linux comparison VM only: all downloads and results stay in RAM.
set -eu
base=${DEQP_URL:?set the host test artifact URL}
mkdir -p /linux/tmp/deqp
wget -q -O /linux/tmp/deqp.tar.gz "$base/deqp.tar.gz"
tar -xzf /linux/tmp/deqp.tar.gz -C /linux/tmp/deqp
# CTS opens development-library names; keep aliases private to the test.
ln -sf /usr/lib/libEGL.so.1 /linux/tmp/deqp/lib/libEGL.so
ln -sf /usr/lib/libGLESv2.so.2 /linux/tmp/deqp/lib/libGLESv2.so
wget -q -O /linux/tmp/deqp/repro.txt "$base/repro.txt"
export DISPLAY=:0
mounted=0
trap 'if test "$mounted" = 1; then umount /linux/usr/lib/libgallium-25.1.9.so; fi' EXIT
for variant in baseline patched; do
    wget -q -O "/cow/$variant.so" "$base/$variant.so"
    sha256sum "/cow/$variant.so" >"/linux/tmp/deqp/$variant.sha256"
    mount --bind "/cow/$variant.so" /linux/usr/lib/libgallium-25.1.9.so
    mounted=1
    for suite in info repro buffer egl; do
        case "$suite" in
            info) binary=deqp-gles31; selection=--deqp-case=dEQP-GLES31.info.* ;;
            repro) binary=deqp-gles31; selection=--deqp-caselist-file=repro.txt ;;
            buffer) binary=deqp-gles31; selection=--deqp-case=dEQP-GLES31.functional.image_load_store.buffer.* ;;
            egl) binary=deqp-egl; selection=--deqp-case=dEQP-EGL.functional.color_clears.single_context.gles2.* ;;
        esac
        name=$variant-$suite
        echo "DEQP_BEGIN $name"
        rc=0
        timeout 180 chroot /linux /bin/sh -c \
            'cd /tmp/deqp; export LD_LIBRARY_PATH=/tmp/deqp/lib; exec "./$1" "$2" --deqp-surface-width=128 --deqp-surface-height=128 --deqp-log-images=disable --deqp-log-filename="$3.qpa"' \
            sh "$binary" "$selection" "$name" \
            >"/linux/tmp/deqp/$name.stdout" 2>&1 || rc=$?
        echo "$rc" >"/linux/tmp/deqp/$name.exit"
        tail -12 "/linux/tmp/deqp/$name.stdout"
        echo "DEQP_END $name rc=$rc"
    done
    umount /linux/usr/lib/libgallium-25.1.9.so
    mounted=0
done
cp /linux/var/log/Xorg.0.log /linux/tmp/deqp/Xorg.0.log
cd /linux/tmp/deqp
tar -czf ../results.tar.gz \
    baseline.sha256 patched.sha256 Xorg.0.log \
    baseline-*.qpa baseline-*.stdout baseline-*.exit \
    patched-*.qpa patched-*.stdout patched-*.exit
# BusyBox wget treats POST files as strings, so encode the binary archive.
base64 ../results.tar.gz >../results.b64
wget -q -O /dev/null --post-file=/linux/tmp/results.b64 "$base/results.tar.gz"
echo DEQP_FINISHED
