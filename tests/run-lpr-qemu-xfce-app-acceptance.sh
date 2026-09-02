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

wait_for_xfce() {
  local attempt wm_property
  for ((attempt = 0; attempt < 30; ++attempt)); do
    wm_property="$(
      x_query /usr/bin/xprop -root _NET_SUPPORTING_WM_CHECK
    )"
    if [[ "${wm_property}" == *'window id'* ]]; then
      printf 'XFCE_APP_ACCEPTANCE_DESKTOP_READY display=%s\n' "${DISPLAY}"
      return 0
    fi
    /bin/busybox sleep 0.5
  done
  printf 'XFCE_APP_ACCEPTANCE_DESKTOP_FAIL reason=window-manager-timeout\n'
  return 1
}

find_new_mapped_window() {
  local baseline=$1 class_regex=$2 title_regex=$3 work_prefix=$4
  local attempt line line_lower window window_info properties
  mapped_window=
  mapped_pid=

  for ((attempt = 0; attempt < 20; ++attempt)); do
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
      if [[ "${properties}" =~ _NET_WM_PID[^=]*=[[:space:]]*([0-9]+) ]]; then
        mapped_pid=${BASH_REMATCH[1]}
      fi
      printf '%s\n' "${properties}" >"${work_prefix}.properties"
      return 0
    done <"${work_prefix}.tree"
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
    printf 'XFCE_APP_RESULT app=%s iteration=%s status=PASS window=%s pid=%s command=%s\n' \
      "${app}" "${iteration}" "${mapped_window}" "${mapped_pid:-unknown}" \
      "${command_name}"
    stop_test_window "${launch_pid}" "${app}" "${close_file}"
    return 0
  fi

  printf 'XFCE_APP_RESULT app=%s iteration=%s status=FAIL reason=no-mapped-window command=%s\n' \
    "${app}" "${iteration}" "${command_name}"
  window_tree | /bin/busybox tail -n 80 || true
  /bin/busybox tail -n 80 "${log}" 2>/dev/null || true
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

  if ! wait_for_xfce; then
    printf 'XFCE_APP_ACCEPTANCE_DONE status=FAIL reason=window-manager-timeout\n'
    return 1
  fi

  local iteration app pass_count=0 fail_count=0
  local -a apps=(Thunar pine2-gtk about gtk3-demo terminal)
  if [[ -n "${XFCE_APP_ONLY:-}" ]]; then
    case "${XFCE_APP_ONLY}" in
      Thunar|pine2-gtk|about|gtk3-demo|terminal)
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
  exec /usr/bin/startx /bin/bash "${script_guest}" --x-session -- :0
}

dump_startup_process_tree() {
  local reason=${1:-unknown}
  printf 'XFCE_STARTUP_PROCESS_TREE_BEGIN reason=%s\n' "${reason}"
  /bin/busybox ps -o pid,ppid,stat,wchan,etime,args 2>&1 || true
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
  local diagnostic_pid=$! status
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
  /bin/busybox kill "${diagnostic_pid}" 2>/dev/null || true
  wait "${diagnostic_pid}" 2>/dev/null || true
  return "${status}"
}

run_host() {
  local repo_root repeat timeout_seconds selected_app
  local -a expected_apps expect_args
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  repeat=${XFCE_APP_REPEAT:-1}
  timeout_seconds=${XFCE_APP_TIMEOUT_SECONDS:-240}
  selected_app=${XFCE_APP_ONLY:-}
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
    Thunar|pine2-gtk|about|gtk3-demo|terminal)
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

  .artifacts/bin/pacgo qemu-test \
    --console-shell \
    --cpus "${XFCE_APP_CPUS:-4}" \
    --timeout "${timeout_seconds}s" \
    --graphics 2d \
    --input-profile keyboard-tablet \
    --send "XFCE_APP_REPEAT=${repeat} XFCE_APP_ONLY=${selected_app} /bin/bash ${script_guest} --guest-controller" \
    "${expect_args[@]}" \
    --expect 'XFCE_APP_ACCEPTANCE_DONE status='

  if ! grep -Fq 'XFCE_APP_ACCEPTANCE_DONE status=PASS' \
      .artifacts/console-tty-test.log; then
    printf 'Xfce app acceptance reported a failure; see %s\n' \
      "${repo_root}/.artifacts/console-tty-test.log" >&2
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
