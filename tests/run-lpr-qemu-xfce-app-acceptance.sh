#!/usr/bin/env bash
set -euo pipefail

script_guest=/cmd/xfce_app_acceptance.sh
startup_diag_request=XFCE_STARTUP_DUMP_PROCESS_TREE

x_query() {
  /bin/busybox timeout -k 1 3 "$@" 2>/dev/null || true
}

window_tree() {
  x_query /usr/bin/xwininfo -root -tree
}

window_ids() {
  local line
  while IFS= read -r line; do
    if [[ "${line}" =~ ^[[:space:]]*(0x[[:xdigit:]]+) ]]; then
      printf '%s\n' "${BASH_REMATCH[1],,}"
    fi
  done
}

wait_for_window_manager() {
  local attempt wm_property
  for ((attempt = 0; attempt < 30; ++attempt)); do
    wm_property="$(
      x_query /usr/bin/xprop -root _NET_SUPPORTING_WM_CHECK
    )"
    if [[ "${wm_property}" == *'window id'* ]]; then
      printf 'XFCE_APP_ACCEPTANCE_WM_READY display=%s\n' "${DISPLAY}"
      return 0
    fi
    /bin/busybox sleep 0.5
  done
  printf 'XFCE_APP_ACCEPTANCE_WM_FAIL reason=window-manager-timeout\n'
  return 1
}

xfce_window_candidate() {
  local needle=$1 min_width=$2 min_height=$3
  local line window width height area best_area=0 best_window=
  while IFS= read -r line; do
    [[ "${line}" == *"${needle}"* ]] || continue
    [[ "${line}" =~ ^[[:space:]]*(0x[[:xdigit:]]+) ]] || continue
    window=${BASH_REMATCH[1],,}
    [[ "${line}" =~ [[:space:]]([0-9]+)x([0-9]+)[+-] ]] || continue
    width=${BASH_REMATCH[1]}
    height=${BASH_REMATCH[2]}
    ((width >= min_width && height >= min_height)) || continue
    area=$((width * height))
    if ((area > best_area)); then
      best_area=${area}
      best_window=${window}
    fi
  done
  [[ -n "${best_window}" ]] || return 1
  printf '%s\n' "${best_window}"
}

window_is_viewable() {
  local window_info
  [[ -n "${1:-}" ]] || return 1
  window_info="$(x_query /usr/bin/xwininfo -id "$1")"
  [[ "${window_info}" == *'Map State: IsViewable'* ]]
}

wait_for_xfce_ui() {
  local attempt tree desktop_window panel_window
  local attempts=${XFCE_DESKTOP_ATTEMPTS:-12}
  for ((attempt = 0; attempt < attempts; ++attempt)); do
    tree="$(window_tree)"
    desktop_window="$(
      printf '%s\n' "${tree}" | xfce_window_candidate \
        '"Desktop": ("xfdesktop" "Xfdesktop")' 100 100 || true
    )"
    panel_window="$(
      printf '%s\n' "${tree}" | xfce_window_candidate \
        '"xfce4-panel": ("xfce4-panel" "Xfce4-panel")' 100 10 || true
    )"
    if window_is_viewable "${desktop_window}" &&
        window_is_viewable "${panel_window}"; then
      printf 'XFCE_APP_ACCEPTANCE_DESKTOP_READY display=%s desktop=%s panel=%s\n' \
        "${DISPLAY}" "${desktop_window}" "${panel_window}"
      return 0
    fi
    /bin/busybox sleep 0.5
  done
  printf 'XFCE_APP_ACCEPTANCE_DESKTOP_FAIL reason=desktop-ui-timeout desktop=%s panel=%s\n' \
    "${desktop_window:-missing}" "${panel_window:-missing}"
  if [[ -n "${desktop_window:-}" ]]; then
    x_query /usr/bin/xwininfo -id "${desktop_window}" | \
      /bin/busybox grep -E 'Window id:|Width:|Height:|Map State:' || true
  fi
  if [[ -n "${panel_window:-}" ]]; then
    x_query /usr/bin/xwininfo -id "${panel_window}" | \
      /bin/busybox grep -E 'Window id:|Width:|Height:|Map State:' || true
  fi
  /bin/busybox printf '%s\n' "${tree}" | /bin/busybox tail -n 120
  return 1
}

verify_xorg_gpu() {
  local log=/var/log/Xorg.0.log renderer
  if [[ ! -s "${log}" ]]; then
    printf 'XFCE_GPU_XORG_FAIL reason=missing-log path=%s\n' "${log}"
    return 1
  fi
  if ! /bin/busybox grep -Fq \
      'Loading /usr/lib/xorg/modules/drivers/modesetting_drv.so' "${log}"; then
    printf 'XFCE_GPU_XORG_FAIL reason=modesetting-driver\n'
    return 1
  fi
  renderer="$({
    /bin/busybox grep -F \
      'glamor X acceleration enabled on virgl (D3D12 (' "${log}" || true
  } | /bin/busybox tail -n 1)"
  if [[ -z "${renderer}" ]]; then
    printf 'XFCE_GPU_XORG_FAIL reason=glamor-renderer\n'
    /bin/busybox grep -i 'glamor' "${log}" || true
    return 1
  fi
  local extension
  for extension in COMPOSITE Present DRI3; do
    if ! /bin/busybox grep -Fq \
        "Initializing extension ${extension}" "${log}"; then
      printf 'XFCE_GPU_XORG_FAIL reason=extension extension=%s\n' \
        "${extension}"
      return 1
    fi
  done
  if ! /bin/busybox grep -Fq 'Damage tracking initialized' "${log}"; then
    printf 'XFCE_GPU_XORG_FAIL reason=damage-tracking\n'
    return 1
  fi
  if /bin/busybox grep -Eiq \
      'Refusing to try glamor|glamor initialization failed|glamor disabled' \
      "${log}"; then
    printf 'XFCE_GPU_XORG_FAIL reason=software-fallback\n'
    return 1
  fi
  renderer=${renderer#*glamor X acceleration enabled on }
  printf 'XFCE_GPU_XORG_INIT_PASS renderer=%s extensions=COMPOSITE,Present,DRI3 damage=initialized\n' \
    "${renderer}"
}

activate_xorg_output() {
  local attempt monitor state
  monitor=
  state=
  for ((attempt = 0; attempt < 20; ++attempt)); do
    if state="$(/bin/busybox timeout -k 1 10 /usr/bin/xrandr --query)"; then
      monitor="$(/bin/busybox printf '%s\n' "${state}" | /bin/busybox sed -n \
        's/^\([^ ]*\) connected.*/\1/p' | /bin/busybox head -n 1)"
      [[ -z "${monitor}" ]] || break
    fi
    /bin/busybox sleep 0.25
  done
  printf 'XFCE_GPU_XRANDR_PROBE_BEGIN\n%s\nXFCE_GPU_XRANDR_PROBE_END\n' \
    "${state}"
  if [[ -z "${monitor}" ]]; then
    printf 'XFCE_GPU_OUTPUT_FAIL reason=no-connected-output\n'
    return 1
  fi

  # Force a real modeset.  Xorg's initial RandR state can already say active
  # even when no SETCRTC reached pachagpu, in which case --auto alone is a
  # no-op.  Do this before xfsettingsd snapshots the output state.
  if ! /bin/busybox timeout -k 1 15 /usr/bin/xrandr \
      --output "${monitor}" --off; then
    printf 'XFCE_GPU_OUTPUT_FAIL reason=output-disable output=%s\n' "${monitor}"
    return 1
  fi
  if ! /bin/busybox timeout -k 1 15 /usr/bin/xrandr \
      --output "${monitor}" --auto --primary --pos 0x0; then
    printf 'XFCE_GPU_OUTPUT_FAIL reason=output-enable output=%s\n' "${monitor}"
    return 1
  fi
  state="$(/bin/busybox timeout -k 1 10 /usr/bin/xrandr --query)"
  printf 'XFCE_GPU_XRANDR_STATE_BEGIN\n%s\nXFCE_GPU_XRANDR_STATE_END\n' "${state}"
  if ! /bin/busybox printf '%s\n' "${state}" | /bin/busybox grep -Eq \
      "^${monitor} connected( primary)? [0-9]+x[0-9]+\\+"; then
    printf 'XFCE_GPU_OUTPUT_FAIL reason=output-inactive output=%s\n' "${monitor}"
    return 1
  fi
  printf 'XFCE_GPU_OUTPUT_ACTIVE output=%s\n' "${monitor}"
}

# One dump after the wait gives a single frame of a moving picture, and three
# such frames disagreed about which process was stuck.  Sample the interesting
# ones while the wait is still running so the sequence is visible.
sample_watched_processes() {
  local tag=$1 pid comm
  for pid in /proc/[0-9]*; do
    pid=${pid#/proc/}
    comm=$(/bin/busybox cat "/proc/${pid}/comm" 2>/dev/null) || continue
    case "${comm}" in
      Xorg|geany|gtk3-demo|xfce4-terminal|xfce4-about|pine2-gtk|thunar|Thunar|dbus-daemon|at-spi-bus-launcher|at-spi2-registryd) ;;
      *) continue ;;
    esac
    printf 'XFCE_APP_SAMPLE at=%s pid=%s comm=%s syscall=%s\n' \
      "${tag}" "${pid}" "${comm}" \
      "$(/bin/busybox cat "/proc/${pid}/syscall" 2>/dev/null | /bin/busybox tr -d '\n')"
  done
}

request_message_bus_snapshot() {
  # This peer intentionally does not exist.  unix_socket.c recognizes the
  # connect path and snapshots without depending on the possibly stalled
  # session bus to finish a D-Bus handshake.
  /bin/busybox timeout -k 1 3 /usr/bin/dbus-send \
    --peer=unix:path=/tmp/.pacha-atspi-snapshot \
    /org/pacha/Atspi org.pacha.Atspi.Snapshot \
    >/dev/null 2>&1 || true
}

find_new_mapped_window() {
  local baseline=$1 class_regex=$2 title_regex=$3 work_prefix=$4
  local attempt line line_lower window window_info properties
  local attempts=${XFCE_APP_WINDOW_ATTEMPTS:-20}
  mapped_window=
  mapped_pid=
  mapped_attempt=

  for ((attempt = 0; attempt < attempts; ++attempt)); do
    window_tree >"${work_prefix}.tree"
    while IFS= read -r line; do
      line_lower=${line,,}
      [[ "${line_lower}" =~ ${class_regex} ]] || continue
      [[ "${line}" =~ ${title_regex} ]] || continue
      [[ "${line}" =~ ^[[:space:]]*(0x[[:xdigit:]]+) ]] || continue
      window=${BASH_REMATCH[1],,}
      if /bin/busybox grep -Fqx "${window}" "${baseline}"; then
        continue
      fi

      window_info="$(x_query /usr/bin/xwininfo -id "${window}")"
      [[ "${window_info}" == *'Map State: IsViewable'* ]] || continue

      properties="$(
        x_query /usr/bin/xprop -id "${window}" \
          WM_CLASS WM_NAME _NET_WM_NAME _NET_WM_PID _NET_WM_WINDOW_TYPE
      )"

      mapped_window=${window}
      mapped_attempt=${attempt}
      if [[ "${properties}" =~ _NET_WM_PID[^=]*=[[:space:]]*([0-9]+) ]]; then
        mapped_pid=${BASH_REMATCH[1]}
      fi
      printf '%s\n' "${properties}" >"${work_prefix}.properties"
      return 0
    done <"${work_prefix}.tree"
    if (( attempt % 5 == 0 )); then
      sample_watched_processes "wait-${attempt}"
    fi
    if (( attempt == 8 )); then
      request_message_bus_snapshot
    fi
    /bin/busybox sleep 1
  done
  return 1
}

stop_test_window() {
  local launch_pid=$1 app=$2 close_file=${3:-}
  local attempt window_info window_viewable=0
  if [[ "${app}" == terminal && -n "${close_file}" ]]; then
    : >"${close_file}"
  elif [[ "${app}" == Thunar ]]; then
    /usr/bin/thunar --quit >/dev/null 2>&1 &
  elif [[ "${mapped_pid:-}" =~ ^[0-9]+$ ]] && ((mapped_pid > 1)); then
    /bin/busybox kill -TERM "${mapped_pid}" 2>/dev/null || true
  fi
  if [[ "${launch_pid}" =~ ^[0-9]+$ ]] && ((launch_pid > 1)); then
    /bin/busybox kill -TERM "${launch_pid}" 2>/dev/null || true
  fi
  for ((attempt = 0; attempt < 20; ++attempt)); do
    if [[ -z "${mapped_window:-}" ]] ||
        [[ "$(x_query /usr/bin/xwininfo -id "${mapped_window}")" \
          != *'Map State: IsViewable'* ]]; then
      break
    fi
    /bin/busybox sleep 0.1
  done
  window_info="$(
    if [[ -n "${mapped_window:-}" ]]; then
      x_query /usr/bin/xwininfo -id "${mapped_window}"
    fi
  )"
  if [[ "${window_info}" == *'Map State: IsViewable'* ]]; then
    window_viewable=1
  fi

  # A wedged GUI must not prevent the remaining applications from being
  # tested.  TERM gets a short grace period; KILL makes cleanup bounded.
  if ((window_viewable != 0)) &&
      [[ "${mapped_pid:-}" =~ ^[0-9]+$ ]] && ((mapped_pid > 1)); then
    /bin/busybox kill -KILL "${mapped_pid}" 2>/dev/null || true
  fi
  if [[ "${launch_pid}" =~ ^[0-9]+$ ]] && ((launch_pid > 1)) &&
      /bin/busybox kill -0 "${launch_pid}" 2>/dev/null; then
    /bin/busybox kill -KILL "${launch_pid}" 2>/dev/null || true
  fi
  wait "${launch_pid}" 2>/dev/null || true
}

run_app() {
  local app=$1 iteration=$2 command_name class_regex title_regex
  local work_prefix baseline close_file= log launch_pid
  work_prefix="${XFCE_APP_WORK_DIR}/iteration-${iteration}-${app}"
  baseline="${work_prefix}.baseline"
  log="${work_prefix}.log"
  : >"${log}"
  printf 'XFCE_APP_INSTRUMENT run-begin app=%s iteration=%s\n' \
    "${app}" "${iteration}"
  window_tree >"${work_prefix}.baseline-tree"
  window_ids <"${work_prefix}.baseline-tree" >"${baseline}"
  printf 'XFCE_APP_INSTRUMENT baseline-ready app=%s iteration=%s\n' \
    "${app}" "${iteration}"

  case "${app}" in
    about)
      command_name=/usr/bin/xfce4-about
      class_regex='xfce4-about'
      title_regex='About the Xfce Desktop Environment'
      /usr/bin/xfce4-about >"${log}" 2>&1 &
      ;;
    geany)
      # Not part of the default set: geany ships in no image, so this case
      # installs it the way a user does and then runs that same binary.  It
      # covers the report that an apk-installed GUI app hangs while the ones
      # already in the image do not.
      command_name=/usr/bin/geany
      class_regex='geany'
      title_regex='Geany|untitled'
      if [[ ! -x /usr/bin/geany ]]; then
        printf 'XFCE_APP_INSTRUMENT apk-add app=%s iteration=%s\n' \
          "${app}" "${iteration}"
        if ! /sbin/apk --no-progress add geany >"${work_prefix}.apk" 2>&1; then
          printf 'XFCE_APP_RESULT app=%s iteration=%s status=FAIL reason=apk-add\n' \
            "${app}" "${iteration}"
          /bin/busybox tail -n 40 "${work_prefix}.apk" 2>/dev/null || true
          return 1
        fi
        printf 'XFCE_APP_INSTRUMENT apk-add-done app=%s iteration=%s\n' \
          "${app}" "${iteration}"
      fi
      # XFCE_APP_GEANY_ENV exists to bisect, not to configure: setting
      # NO_AT_BRIDGE=1 here answers whether the hang lives in the
      # accessibility path without pretending that disabling it is a fix.
      if [[ -n "${XFCE_APP_GEANY_ENV:-}" ]]; then
        env ${XFCE_APP_GEANY_ENV} /usr/bin/geany >"${log}" 2>&1 &
      else
        env -u NO_AT_BRIDGE /usr/bin/geany >"${log}" 2>&1 &
      fi
      ;;
    gtk3-demo)
      command_name=/usr/bin/gtk3-demo
      class_regex='gtk3-demo'
      title_regex='Application Class|GTK\+ Code Demos'
      /usr/bin/gtk3-demo >"${log}" 2>&1 &
      ;;
    pine2-gtk)
      command_name=/usr/bin/pine2-gtk
      class_regex='pine2|app\.pine2'
      title_regex='Pine2'
      /usr/bin/pine2-gtk >"${log}" 2>&1 &
      ;;
    terminal)
      command_name=/usr/bin/xfce4-terminal
      class_regex='xfce4-terminal'
      title_regex='XFCE Acceptance Terminal|Terminal|root@|pachaos'
      close_file="${work_prefix}.close"
      /bin/busybox rm -f "${close_file}"
      /usr/bin/xfce4-terminal --disable-server \
        --title='XFCE Acceptance Terminal' \
        --command="/bin/bash ${script_guest} --terminal-child ${close_file}" \
        >"${log}" 2>&1 &
      ;;
    Thunar)
      command_name=/usr/bin/thunar
      class_regex='thunar'
      title_regex='Thunar|File Manager|File System|root'
      /usr/bin/thunar /root </dev/tty >"${log}" 2>&1 &
      ;;
    *)
      printf 'XFCE_APP_RESULT app=%s iteration=%s status=FAIL reason=unknown-app\n' \
        "${app}" "${iteration}"
      return 1
      ;;
  esac
  launch_pid=$!
  printf 'XFCE_APP_INSTRUMENT launch-return app=%s iteration=%s pid=%s\n' \
    "${app}" "${iteration}" "${launch_pid}"

  if find_new_mapped_window \
      "${baseline}" "${class_regex}" "${title_regex}" "${work_prefix}"; then
    printf 'XFCE_APP_RESULT app=%s iteration=%s status=PASS window=%s pid=%s attempt=%s command=%s\n' \
      "${app}" "${iteration}" "${mapped_window}" "${mapped_pid:-unknown}" \
      "${mapped_attempt:-unknown}" "${command_name}"
    stop_test_window "${launch_pid}" "${app}" "${close_file}"
    return 0
  fi

  printf 'XFCE_APP_RESULT app=%s iteration=%s status=FAIL reason=no-mapped-window command=%s\n' \
    "${app}" "${iteration}" "${command_name}"
  window_tree | /bin/busybox tail -n 80 || true
  /bin/busybox tail -n 80 "${log}" 2>/dev/null || true
  # The app is still alive at this point, so record where it is sleeping
  # before stop_test_window kills it.  Without this the only evidence left is
  # that no window appeared, which does not say what the app was waiting on.
  dump_startup_process_tree "app-${app}-no-window"
  stop_test_window "${launch_pid}" "${app}" "${close_file}"
  return 1
}

run_x_session() {
  export HOME=/root
  export USER=root
  export LOGNAME=root
  export SHELL=/bin/bash
  export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/cmd
  export XDG_RUNTIME_DIR=/run/user/0
  export LANG=C.UTF-8
  export LC_ALL=C.UTF-8
  export XFCE_APP_WORK_DIR=${XFCE_APP_WORK_DIR:-/root/.xfce-app-acceptance/work}
  /bin/busybox mkdir -p "${XFCE_APP_WORK_DIR}"

  if ! wait_for_window_manager; then
    printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL reason=window-manager-timeout\n'
    return 1
  fi
  if ! wait_for_xfce_ui; then
    printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL reason=desktop-ui-timeout\n'
    return 1
  fi
  local iteration app pass_count=0 fail_count=0
  local -a apps=(Thunar pine2-gtk about gtk3-demo terminal)
  if [[ -n "${XFCE_APP_ONLY:-}" ]]; then
    case "${XFCE_APP_ONLY}" in
      xorg)
        apps=()
        ;;
      Thunar|pine2-gtk|about|gtk3-demo|terminal|geany)
        apps=("${XFCE_APP_ONLY}")
        ;;
      *)
        printf 'XFCE_APP_ACCEPTANCE_FAIL reason=invalid-app value=%s\n' \
          "${XFCE_APP_ONLY}"
        return 2
        ;;
    esac
  fi
  printf 'XFCE_APP_INSTRUMENT plan repeat=%s apps=%s\n' \
    "${XFCE_APP_REPEAT}" "${apps[*]}"
  for ((iteration = 1; iteration <= XFCE_APP_REPEAT; ++iteration)); do
    for app in "${apps[@]}"; do
      if run_app "${app}" "${iteration}"; then
        pass_count=$((pass_count + 1))
      else
        fail_count=$((fail_count + 1))
      fi
    done
  done

  if ((fail_count == 0)); then
    printf 'XFCE_APP_ACCEPTANCE_PASS total=%s repeat=%s failures=0\n' \
      "${pass_count}" "${XFCE_APP_REPEAT}"
    printf 'XFCE_APP_ACCEPTANCE_DONE status=PASS\n'
    return 0
  fi
  printf 'XFCE_APP_ACCEPTANCE_FAIL passed=%s failed=%s repeat=%s\n' \
    "${pass_count}" "${fail_count}" "${XFCE_APP_REPEAT}"
  printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL\n'
  return 1
}

launch_xfce() {
  local acceptance_root=/root/.xfce-app-acceptance
  local acceptance_config="${acceptance_root}/config"
  local acceptance_cache="${acceptance_root}/cache"
  local acceptance_work="${acceptance_root}/work"
  local acceptance_tmp="${acceptance_root}/tmp"
  local autostart_dir="${acceptance_config}/autostart"
  /bin/busybox rm -rf "${acceptance_root}"
  /bin/busybox mkdir -p \
    "${autostart_dir}" "${acceptance_cache}" "${acceptance_work}" \
    "${acceptance_tmp}"
  printf '%s\n' \
    '[Desktop Entry]' \
    'Type=Application' \
    'Name=PachaOS Xfce App Acceptance' \
    "Exec=/bin/bash ${script_guest} --run-tests" \
    'OnlyShowIn=XFCE;' \
    'X-GNOME-Autostart-enabled=true' \
    >"${autostart_dir}/pacha-xfce-app-acceptance.desktop"
  export XDG_CONFIG_HOME=${acceptance_config}
  export XDG_CACHE_HOME=${acceptance_cache}
  export XFCE_APP_WORK_DIR=${acceptance_work}
  export TMPDIR=${acceptance_tmp}
  exec /usr/bin/startxfce4
}

run_guest_controller() {
  export XFCE_APP_REPEAT=${XFCE_APP_REPEAT:-1}
  export XFCE_APP_WINDOW_ATTEMPTS=${XFCE_APP_WINDOW_ATTEMPTS:-20}
  export XFCE_APP_GEANY_ENV=${XFCE_APP_GEANY_ENV:-}
  export HOME=/root
  export USER=root
  export LOGNAME=root
  export SHELL=/bin/bash
  export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/cmd
  export XDG_RUNTIME_DIR=/run/user/0
  export LANG=C.UTF-8
  export LC_ALL=C.UTF-8

  [[ "${XFCE_APP_REPEAT}" =~ ^[1-9][0-9]*$ ]] || {
    printf 'XFCE_APP_ACCEPTANCE_FAIL reason=invalid-repeat value=%s\n' \
      "${XFCE_APP_REPEAT}"
    return 2
  }
  /bin/busybox mkdir -p /run/user/0
  /bin/busybox chmod 0700 /run/user/0
  printf 'XFCE_APP_ACCEPTANCE_START repeat=%s\n' "${XFCE_APP_REPEAT}"
  printf 'XFCE_APP_INSTRUMENT controller repeat=%s only=%s\n' \
    "${XFCE_APP_REPEAT}" "${XFCE_APP_ONLY:-}"
  local status
  if /usr/bin/startx /bin/bash "${script_guest}" --x-session -- :0; then
    status=0
  else
    status=$?
  fi
  printf 'XFCE_APP_XORG_EXIT status=%s\n' "${status}"
  if ((status != 0)); then
    /bin/busybox sed -n '1,240p' /var/log/Xorg.0.log 2>/dev/null || true
    printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL reason=xorg-exit code=%s\n' \
      "${status}"
  fi
  return "${status}"
}

dump_startup_process_tree() {
  local reason=${1:-unknown} pid state
  printf 'XFCE_STARTUP_PROCESS_TREE_BEGIN reason=%s\n' "${reason}"
  # busybox ps rejects the whole -o list if one field is unsupported, and it
  # has no wchan.  Read blocking state first so a slow ps/proc enumeration
  # cannot hide the per-process evidence this diagnostic exists to collect.
  for pid in /proc/[0-9]*; do
    pid=${pid#/proc/}
    if ! state=$(/bin/busybox timeout -k 1 1 /bin/busybox sed -n \
        's/^State:[[:space:]]*//p' "/proc/${pid}/status" 2>/dev/null); then
      printf 'XFCE_STARTUP_PROC pid=%s query=timeout\n' "${pid}"
      continue
    fi
    [[ -n "${state}" ]] || continue
    printf 'XFCE_STARTUP_PROC pid=%s state=%s wchan=%s syscall=%s comm=%s\n' \
      "${pid}" \
      "${state}" \
      "$(/bin/busybox timeout -k 1 1 /bin/busybox cat "/proc/${pid}/wchan" 2>/dev/null || printf '?')" \
      "$(/bin/busybox timeout -k 1 1 /bin/busybox cat "/proc/${pid}/syscall" 2>/dev/null || printf '?')" \
      "$(/bin/busybox timeout -k 1 1 /bin/busybox cat "/proc/${pid}/comm" 2>/dev/null || printf '?')"
  done
  /bin/busybox ps -o pid,ppid,stat,etime,args 2>&1 || true
  printf 'XFCE_STARTUP_PROCESS_TREE_END reason=%s\n' "${reason}"
}

run_startup_diagnostic_listener() {
  local request
  while IFS= read -r request </dev/tty; do
    if [[ "${request}" == "${startup_diag_request}" ]]; then
      dump_startup_process_tree host-timeout
    fi
  done
}

run_startup_controller() {
  export XFCE_APP_REPEAT=1
  export HOME=/root
  export USER=root
  export LOGNAME=root
  export SHELL=/bin/bash
  export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/cmd
  export XDG_RUNTIME_DIR=/run/user/0
  export LANG=C.UTF-8
  export LC_ALL=C.UTF-8

  /bin/busybox mkdir -p /run/user/0
  /bin/busybox chmod 0700 /run/user/0
  printf 'XFCE_STARTUP_CONTROLLER_START\n'

  run_startup_diagnostic_listener &
  local diagnostic_pid=$! auto_diagnostic_pid= status
  if [[ -n "${XFCE_STARTUP_AUTO_DUMP_SECONDS:-}" ]]; then
    if [[ ! "${XFCE_STARTUP_AUTO_DUMP_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
      printf 'XFCE_STARTUP_AUTO_DUMP_SECONDS must be a positive integer: %s\n' \
        "${XFCE_STARTUP_AUTO_DUMP_SECONDS}"
      return 2
    fi
    (
      /bin/busybox sleep "${XFCE_STARTUP_AUTO_DUMP_SECONDS}"
      dump_startup_process_tree auto-timeout
    ) &
    auto_diagnostic_pid=$!
  fi
  if /usr/bin/startx /bin/bash "${script_guest}" --x-session -- :0; then
    status=0
  else
    status=$?
  fi

  # A return from startx before the autostart readiness marker is itself one
  # of the failures this controller must preserve.  Dump while this script is
  # still the foreground tty job so no new shell command can race the dump.
  printf 'XFCE_STARTUP_SESSION_EXIT status=%s\n' "${status}"
  dump_startup_process_tree session-exit
  if [[ -n "${auto_diagnostic_pid}" ]]; then
    /bin/busybox kill "${auto_diagnostic_pid}" 2>/dev/null || true
    wait "${auto_diagnostic_pid}" 2>/dev/null || true
  fi
  /bin/busybox kill "${diagnostic_pid}" 2>/dev/null || true
  wait "${diagnostic_pid}" 2>/dev/null || true
  return "${status}"
}

run_host() {
  local repo_root repeat timeout_seconds selected_app
  local -a expected_apps expect_args qemu_args=(
    --qemu-arg=-vga
    --qemu-arg=none
    --qemu-arg=-device
    --qemu-arg=ramfb
  )
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  repeat=${XFCE_APP_REPEAT:-1}
  timeout_seconds=${XFCE_APP_TIMEOUT_SECONDS:-240}
  selected_app=${XFCE_APP_ONLY:-}
  if [[ -n ${XFCE_APP_QMP_SOCKET:-} ]]; then
    qemu_args+=(--qemu-arg=-qmp --qemu-arg="unix:${XFCE_APP_QMP_SOCKET},server=on,wait=off")
  fi
  [[ "${repeat}" =~ ^[1-9][0-9]*$ ]] || {
    printf 'XFCE_APP_REPEAT must be a positive integer: %s\n' "${repeat}" >&2
    return 2
  }
  [[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || {
    printf 'XFCE_APP_TIMEOUT_SECONDS must be a positive integer: %s\n' \
      "${timeout_seconds}" >&2
    return 2
  }
  case "${selected_app}" in
    '')
      expected_apps=(Thunar pine2-gtk about gtk3-demo terminal)
      ;;
    xorg)
      expected_apps=()
      ;;
    Thunar|pine2-gtk|about|gtk3-demo|terminal|geany)
      expected_apps=("${selected_app}")
      ;;
    *)
      printf 'XFCE_APP_ONLY must name one known app: %s\n' \
        "${selected_app}" >&2
      return 2
      ;;
  esac
  expect_args=()
  local app
  for app in "${expected_apps[@]}"; do
    expect_args+=(--expect "XFCE_APP_RESULT app=${app} iteration=1 status=")
  done

  cd "${repo_root}"
  if [[ ${SKIP_SYNC:-0} != 1 ]]; then
    PACGO_PROGRESS=plain .artifacts/bin/pacgo sync rootfs --force
    PACGO_PROGRESS=plain .artifacts/bin/pacgo sync bootfs
  fi

  GALLIUM_DRIVER="${VIRGL_HOST_DRIVER:-d3d12}" \
  .artifacts/bin/pacgo qemu-test \
    --console-shell \
    --console-ready-marker 'bash-5.2# ' \
    --cpus "${XFCE_APP_CPUS:-4}" \
    --timeout "${timeout_seconds}s" \
    --graphics virgl \
    --display gtk \
    --input-profile keyboard-tablet \
    "${qemu_args[@]}" \
    --send "XFCE_APP_REPEAT=${repeat} XFCE_APP_ONLY=${selected_app} XFCE_APP_WINDOW_ATTEMPTS=${XFCE_APP_WINDOW_ATTEMPTS:-20} XFCE_APP_GEANY_ENV=${XFCE_APP_GEANY_ENV:-} /bin/bash ${script_guest} --guest-controller" \
    "${expect_args[@]}" \
    --expect 'XFCE_APP_ACCEPTANCE_DONE status='

  if ! grep -Fq 'XFCE_APP_ACCEPTANCE_DONE status=PASS' \
      .artifacts/console-tty-test.log; then
    printf 'Xfce app acceptance reported a failure; see %s\n' \
      "${repo_root}/.artifacts/console-tty-test.log" >&2
    return 1
  fi
  local marker
  for marker in \
      'XFCE_GPU_XORG_INIT_PASS renderer=virgl (D3D12 (' \
      '[gpud] drm submit generation=1 node=0 ' \
      '[gpud] drm modeset generation=1 node=0 ' \
      'XFCE_GPU_OUTPUT_ACTIVE output=Virtual-1' \
      'XFCE_APP_ACCEPTANCE_DESKTOP_READY'; do
    if ! grep -Fq "${marker}" .artifacts/qemu-tty-host-time.log; then
      printf 'Xfce GPU acceptance marker missing: %s\n' "${marker}" >&2
      return 1
    fi
  done
  if ! grep -Eq \
      '\[gpud\] drm dirtyfb generation=1 node=0 .* clips=0 status=-2' \
      .artifacts/qemu-tty-host-time.log; then
    printf 'Xfce GPU acceptance marker missing: Linux-compatible DIRTYFB probe\n' >&2
    return 1
  fi
  if ! grep -Eq \
      '\[gpud\] drm dirtyfb generation=1 node=0 .* clips=[1-9][0-9]* status=0' \
      .artifacts/qemu-tty-host-time.log; then
    printf 'Xfce GPU acceptance marker missing: successful DIRTYFB update\n' >&2
    return 1
  fi
}

case "${1:-}" in
  --terminal-child)
    shift
    while [[ ! -e "${1:-}" ]]; do
      /bin/busybox sleep 0.1
    done
    ;;
  --guest-controller)
    run_guest_controller
    ;;
  --startup-controller)
    run_startup_controller
    ;;
  --x-session)
    shift
    if ! activate_xorg_output; then
      printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL reason=xorg-output\n'
      exit 1
    fi
    if ! verify_xorg_gpu; then
      /bin/busybox sed -n '1,260p' /var/log/Xorg.0.log 2>/dev/null || true
      printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL reason=xorg-gpu\n'
      exit 1
    fi
    exec /usr/bin/dbus-run-session -- /bin/bash "${script_guest}" --launch-xfce
    ;;
  --launch-xfce)
    launch_xfce
    ;;
  --run-tests)
    run_x_session
    ;;
  *)
    run_host
    ;;
esac
