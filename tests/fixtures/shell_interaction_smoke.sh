# PachaOS interactive-shell regression smoke.
# Each case emits an independent marker and failures do not stop later cases.

bb=/cmd/busybox
fixture=/cmd/shell_interaction_smoke.sh
out=/tmp/shell_interaction_out
failures=0

record_case() {
  name="$1"
  shift
  echo "${name}=BEGIN"
  "$@"
  status=$?
  if [ "$status" -eq 0 ]; then
    echo "${name}=OK"
  else
    echo "${name}=FAIL status=$status"
    failures=$((failures + 1))
  fi
}

path_busybox_help() {
  (cd /cmd && PATH=/cmd:/bin:/usr/bin busybox >/dev/null 2>&1)
}

path_busybox_help_console() {
  PATH=/cmd:/bin:/usr/bin busybox
}

absolute_busybox_help_console() {
  "$bb"
}

absolute_busybox_help() {
  "$bb" >/dev/null 2>&1
}

relative_busybox_help() {
  (cd /cmd && ./busybox >/dev/null 2>&1)
}

cwd_ls() {
  cd /cmd || return 1
  PATH=/cmd:/bin:/usr/bin ls
}

cwd_ls_quiet() {
  cd /cmd || return 1
  PATH=/cmd:/bin:/usr/bin ls >"$out" 2>/dev/null
}

history_cat() {
  "$bb" cat "$fixture" >/dev/null 2>&1
}

history_pwd() {
  pwd >/dev/null
}

standalone_true() {
  /bin/true
}

busybox_true() {
  "$bb" true
}

glob_case() {
  (cd /cmd && /bin/ls *.sh >"$out" 2>/dev/null && "$bb" grep -q 'shell_interaction_smoke.sh' "$out")
}

redirection_case() {
  echo shell-redirection >"$out" || return 1
  got=$("$bb" cat "$out" 2>/dev/null)
  [ "x$got" = xshell-redirection ]
}

exit_status_case() {
  /bin/false
  status=$?
  echo "$status" >"$out"
  got=$("$bb" cat "$out" 2>/dev/null)
  [ "x$got" = x1 ]
}

shebang_case() {
  "$bb" printf '#!/bin/sh\necho SHELL_SHEBANG_PAYLOAD\n' > /tmp/shell_shebang.sh || return 1
  "$bb" chmod +x /tmp/shell_shebang.sh || return 1
  /tmp/shell_shebang.sh >"$out" 2>/dev/null || return 1
  "$bb" grep -q '^SHELL_SHEBANG_PAYLOAD$' "$out"
}

environment_case() {
  FOO=bar "$bb" env | "$bb" grep -q '^FOO=bar$'
}

repeat_command_case() {
  command_name="$1"
  i=0
  while [ "$i" -lt 5 ]; do
    case "$command_name" in
      busybox) busybox_true || return 1 ;;
      cat) history_cat || return 1 ;;
      pwd) history_pwd || return 1 ;;
      ls) cwd_ls_quiet || return 1 ;;
      *) return 1 ;;
    esac
    i=$((i + 1))
  done
}

ls_then_exec_case() {
  i=0
  while [ "$i" -lt 5 ]; do
    (cd /cmd && /bin/ls >"$out" 2>/dev/null) || return 1
    i=$((i + 1))
  done
  absolute_busybox_help
}

large_ls_then_exec_case() {
  i=0
  while [ "$i" -lt 5 ]; do
    /bin/ls /bin >"$out" 2>/dev/null || return 1
    i=$((i + 1))
  done
  absolute_busybox_help
}

echo SHELL_INTERACTION_START

# Exact stateful reproducer: A -> cd /cmd -> A -> GNU ls -> A.
record_case SHELL_ABA_BUSYBOX_1 absolute_busybox_help_console
record_case SHELL_ABA_CD cd /cmd
record_case SHELL_ABA_BUSYBOX_2 path_busybox_help_console
record_case SHELL_ABA_LS cwd_ls
record_case SHELL_ABA_BUSYBOX_3 path_busybox_help_console
echo SHELL_ABA_DONE

# A longer mixed history keeps exercising the same daemon state.
record_case SHELL_HISTORY_CAT_1 history_cat
record_case SHELL_HISTORY_PWD_1 history_pwd
record_case SHELL_HISTORY_LS_1 cwd_ls_quiet
record_case SHELL_HISTORY_BUSYBOX_1 path_busybox_help
record_case SHELL_HISTORY_CAT_2 history_cat
record_case SHELL_HISTORY_PWD_2 history_pwd
record_case SHELL_HISTORY_LS_2 cwd_ls_quiet
record_case SHELL_HISTORY_BUSYBOX_2 path_busybox_help

record_case SHELL_NOARG_BUSYBOX absolute_busybox_help
record_case SHELL_NOARG_LS cwd_ls_quiet
record_case SHELL_NOARG_TRUE standalone_true
record_case SHELL_PATH_BUSYBOX path_busybox_help
record_case SHELL_ABSOLUTE_BUSYBOX absolute_busybox_help
record_case SHELL_RELATIVE_BUSYBOX relative_busybox_help
record_case SHELL_CD_GLOB glob_case
record_case SHELL_REDIRECTION redirection_case
record_case SHELL_EXIT_STATUS exit_status_case
record_case SHELL_SHEBANG shebang_case
record_case SHELL_ENVIRONMENT environment_case

record_case SHELL_REPEAT_BUSYBOX repeat_command_case busybox
record_case SHELL_REPEAT_CAT repeat_command_case cat
record_case SHELL_REPEAT_PWD repeat_command_case pwd
record_case SHELL_REPEAT_LS repeat_command_case ls
record_case SHELL_LS_THEN_EXEC ls_then_exec_case
record_case SHELL_LARGE_LS_THEN_EXEC large_ls_then_exec_case

rm -f "$out" /tmp/shell_shebang.sh 2>/dev/null
echo "SHELL_INTERACTION_DONE failures=$failures"
[ "$failures" -eq 0 ]
