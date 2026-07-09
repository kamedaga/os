# PachaOS pipe stress fixture — run via: bash /cmd/pipe_stress.sh [iterations]
#
# ハーネス設計 (tty 入力遅延対策, pacha_docs/refactor-plan.md T3.0):
# - テスト本体は rootfs 同梱のこのスクリプト。コンソールへは起動 1 行だけ送る
# - 期待マーカーは送信コマンドに含まれない文字列にする (tty エコー誤マッチ防止)
# - 各ケースは busybox timeout -s KILL でラップし、1 ケースのハングを最大 3 秒に閉じ込める
# - 結果はコマンド置換でなくファイル経由で回収する (孫プロセス残留でも読み取りが
#   ブロックしない)
# - パイプラインは busybox applet で構成する (ユーザー指示: 実バイナリで検証する。
#   自作 fixture ではなく busybox が基準)
# 出力: I<iter>_CASE<n>=BEGIN / =OK / =FAIL want=[..] got=[..]、全完了で PIPE_STRESS_DONE

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

# ケース本体を timeout 付き busybox sh で実行し、結果をファイルから回収する
run_case() {
  name="$1"; want="$2"; cmd="$3"
  echo "${name}=BEGIN"
  rm -f "$out" 2>/dev/null
  $bb timeout -s KILL 3 $bb sh -c "$cmd" > "$out" 2>/dev/null
  got=$($bb cat "$out" 2>/dev/null)
  # 数値比較の空白差異 (wc 実装差) を吸収
  case "$want" in
    *[!0-9]*) : ;;
    *) got=$(( ${got:-999999} + 0 )) ;;
  esac
  check "$name" "$want" "$got"
}

run_iter() {
  it="$1"
  b=$bb
  run_case "I${it}_CASE1" "2"     "$b printf 'a\nb\nab\n' | $b grep a | $b wc -l"
  run_case "I${it}_CASE2" "3"     "$b printf '1\n2\n3\n4\n' | $b grep -v 2 | $b wc -l"
  run_case "I${it}_CASE3" "65536" "$b yes A | $b head -c 65536 | $b wc -c"
  run_case "I${it}_CASE4" "3"     "$b yes B | $b head -n 3 | $b wc -l"
  run_case "I${it}_CASE5" "done"  "$b echo done | $b cat"
  run_case "I${it}_CASE6" "2"     "$b echo r1 > /tmp/ps_f; $b echo r2 >> /tmp/ps_f; $b cat /tmp/ps_f | $b wc -l; $b rm -f /tmp/ps_f"
  run_case "I${it}_CASE7" "2"     "( $b echo sub1; $b echo sub2 ) | $b wc -l"
  run_case "I${it}_CASE8" "5"     "bash -c '$b yes C | $b head -n 5 | $b wc -l'"
}

i=1
while [ "$i" -le "$iters" ]; do
  echo "PIPE_STRESS_ITER_${i}_BEGIN"
  run_iter "$i"
  i=$(( i + 1 ))
done
echo "PIPE_STRESS_DONE"
