#!/bin/bash

set -u

runtime=/run/user/0
config="$runtime/pine2-gtk-smoke.conf"
result="$runtime/pine2-gtk-smoke.status"

rm -f "$config" "$result"
while IFS= read -r line; do
    printf '%s\n' "$line"
done </cmd/phase4_gui_benchmark.conf >"$config"
printf '%s\n' \
    "exec /bin/bash -c 'PINE2_GTK_SMOKE_TEST=1 /usr/bin/pine2-gtk; code=\$?; printf \"%s\\n\" \"\$code\" >$result; /usr/bin/swaymsg exit'" \
    >>"$config"

if [[ ${XDG_RUNTIME_DIR:-} != "$runtime" ||
      ${LIBSEAT_BACKEND:-} != seatd ||
      ${SEATD_SOCK:-} != "$runtime/seatd.sock" ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=session-environment\n'
    exit 1
fi

unset WAYLAND_DISPLAY SWAYSOCK
/usr/bin/timeout -s KILL 120s /usr/bin/sway -c "$config"
sway_status=$?

app_status=
if [[ -r $result ]]; then
    IFS= read -r app_status <"$result" || true
fi
if [[ $sway_status -ne 0 ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=sway-exit status=%d\n' "$sway_status"
    exit 1
fi
if [[ $app_status != 0 ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=app-exit status=%s\n' "${app_status:-missing}"
    exit 1
fi

printf 'PINE2_GTK_SMOKE_PASS gtk=1 markdown=1 exit=0\n'
