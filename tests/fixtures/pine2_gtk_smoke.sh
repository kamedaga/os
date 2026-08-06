#!/bin/bash

set -u

runtime=/run/user/0
config="$runtime/pine2-gtk-smoke.conf"
result="$runtime/pine2-gtk-smoke.status"
runs_result="$runtime/pine2-gtk-smoke.runs"

if [[ ${1:-} == --pine-child ]]; then
    exec env PINE2_GTK_SMOKE_TEST=1 /usr/bin/pine2-gtk
fi

repeat=${PINE2_REPEAT:-1}
if [[ ! $repeat =~ ^[1-9][0-9]*$ ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=repeat value=%s\n' "$repeat"
    exit 1
fi

rm -f "$config" "$result" "$runs_result"
while IFS= read -r line; do
    printf '%s\n' "$line"
done </cmd/phase4_gui_benchmark.conf >"$config"
if [[ ${PINE2_VIA_FOOT:-0} == 1 ]]; then
    printf '%s\n' \
        "exec /bin/bash -c 'code=0; run=0; while [ \$run -lt $repeat ]; do /usr/bin/foot /bin/bash /cmd/pine2_gtk_smoke.sh --pine-child || { code=\$?; break; }; run=\$((run + 1)); done; printf \"%s\\n\" \"\$code\" >$result; printf \"%s\\n\" \"\$run\" >$runs_result; /usr/bin/swaymsg exit'" \
        >>"$config"
else
    printf '%s\n' \
        "exec /bin/bash -c 'code=0; run=0; while [ \$run -lt $repeat ]; do PINE2_GTK_SMOKE_TEST=1 /usr/bin/pine2-gtk || { code=\$?; break; }; run=\$((run + 1)); done; printf \"%s\\n\" \"\$code\" >$result; printf \"%s\\n\" \"\$run\" >$runs_result; /usr/bin/swaymsg exit'" \
        >>"$config"
fi

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
runs=
if [[ -r $runs_result ]]; then
    IFS= read -r runs <"$runs_result" || true
fi
if [[ $sway_status -ne 0 ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=sway-exit status=%d\n' "$sway_status"
    exit 1
fi
if [[ $app_status != 0 ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=app-exit status=%s\n' "${app_status:-missing}"
    exit 1
fi
if [[ $runs != "$repeat" ]]; then
    printf 'PINE2_GTK_SMOKE_FAIL stage=runs completed=%s expected=%s\n' "${runs:-missing}" "$repeat"
    exit 1
fi

printf 'PINE2_GTK_SMOKE_PASS gtk=1 markdown=1 exit=0 via_foot=%s runs=%s\n' \
    "${PINE2_VIA_FOOT:-0}" "$runs"
