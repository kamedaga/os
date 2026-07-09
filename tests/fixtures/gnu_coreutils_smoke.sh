# PachaOS GNU coreutils smoke fixture — run via: bash /cmd/gnu_smoke.sh
# ハーネス設計は pipe_stress.sh と同じ: 本体は rootfs 同梱、tty へは起動 1 行のみ、
# マーカーは送信文字列に含まれない形式、各ケースは busybox timeout でラップし
# 1 ケースのハングを最大 3 秒に閉じ込め、結果はファイル経由で回収する。
# 出力: GNUCU_CASE<n>=BEGIN / =OK / =FAIL、全完了で GNUCU_DONE。

bb=/cmd/busybox
out=/tmp/gnucu_out

check() {
  name="$1"; want="$2"; got="$3"
  if [ "x$got" = "x$want" ]; then
    echo "${name}=OK"
  else
    echo "${name}=FAIL want=[$want] got=[$got]"
  fi
}

run_case() {
  name="$1"; want="$2"; cmd="$3"
  echo "${name}=BEGIN"
  rm -f "$out" 2>/dev/null
  $bb timeout -s KILL 3 bash -c "$cmd" > "$out" 2>/dev/null
  got=$($bb cat "$out" 2>/dev/null)
  case "$want" in
    *[!0-9]*) : ;;
    *) got=$(( ${got:-999999} + 0 )) ;;
  esac
  check "$name" "$want" "$got"
}

run_case "GNUCU_CASE1" "1"   "/bin/coreutils --version | /usr/bin/head -n 1 | /usr/bin/wc -l"
run_case "GNUCU_CASE2" "ok"  "/bin/ls --version | /usr/bin/head -n 1 | /bin/grep -q 'GNU coreutils' && /bin/echo ok"
run_case "GNUCU_CASE3" "gnu" "/bin/echo gnu | /bin/cat"
run_case "GNUCU_CASE4" "3"   "/usr/bin/printf abc | /usr/bin/wc -c"
run_case "GNUCU_CASE5" "2"   "/usr/bin/printf 'a\nb\nc\n' | /usr/bin/head -n 2 | /usr/bin/wc -l"
run_case "GNUCU_CASE6" "2"   "/usr/bin/printf 'a\nb\nc\n' | /usr/bin/tail -n 2 | /usr/bin/wc -l"
run_case "GNUCU_CASE7" "a"   "/usr/bin/printf 'z\na\n' | /usr/bin/sort | /usr/bin/head -n 1"
echo "GNUCU_DONE"
