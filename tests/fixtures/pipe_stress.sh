# PachaOS pipe stress fixture — run via: bash /cmd/pipe_stress.sh [iterations]
#
# ハーネス設計 (tty 入力遅延対策, pacha_docs/refactor-plan.md T3.0):
# - テスト本体は rootfs 同梱のこのスクリプト。コンソールへは起動 1 行だけ送る
# - 期待マーカーは送信コマンドに含まれない文字列にする (tty エコー誤マッチ防止)
# - 各ケースは busybox timeout -s KILL でラップし、1 ケースのハングを最大 3 秒に閉じ込める
# - 結果はコマンド置換でなくファイル経由で回収する (孫プロセス残留でも読み取りが
#   ブロックしない)
# 出力: I<iter>_CASE<n>=OK / =FAIL want=[..] got=[..]、全完了で PIPE_STRESS_DONE

bb=/cmd/busybox
iters="${1:-5}"
out=/tmp/pipe_stress_out

check() {
  name="$1"; want="$2"; got="$3"
  if [ "x$got" = "x$want" ]; then
    echo "${name}=OK"
  else
    echo "${name}=FAIL want=[$want] got=[$got]"
  fi
}

# ケース本体を timeout 付き bash で実行し、結果をファイルから回収する
run_case() {
  name="$1"; want="$2"; cmd="$3"
  echo "${name}=BEGIN"
  rm -f "$out" 2>/dev/null
  $bb timeout -s KILL 3 bash -c "$cmd" > "$out" 2>/dev/null
  got=$(cat "$out" 2>/dev/null)
  # 数値比較の空白差異 (wc 実装差) を吸収
  case "$want" in
    *[!0-9]*) : ;;
    *) got=$(( ${got:-999999} + 0 )) ;;
  esac
  check "$name" "$want" "$got"
}

run_iter() {
  it="$1"
  run_case "I${it}_CASE1" "2"     "printf 'a\nb\nab\n' | grep a | wc -l"
  run_case "I${it}_CASE2" "3"     "printf '1\n2\n3\n4\n' | grep -v 2 | wc -l"
  run_case "I${it}_CASE3" "65536" "yes A | head -c 65536 | wc -c"
  run_case "I${it}_CASE4" "3"     "yes B | head -n 3 | wc -l"
  run_case "I${it}_CASE5" "done"  "echo done | cat"
  run_case "I${it}_CASE6" "2"     "echo r1 > /tmp/ps_f; echo r2 >> /tmp/ps_f; cat /tmp/ps_f | wc -l; rm -f /tmp/ps_f"
  run_case "I${it}_CASE7" "2"     "( echo sub1; echo sub2 ) | wc -l"
  run_case "I${it}_CASE8" "5"     "$bb sh -c 'yes C | head -n 5 | wc -l'"
}

i=1
while [ "$i" -le "$iters" ]; do
  echo "PIPE_STRESS_ITER_${i}_BEGIN"
  run_iter "$i"
  i=$(( i + 1 ))
done
echo "PIPE_STRESS_DONE"
