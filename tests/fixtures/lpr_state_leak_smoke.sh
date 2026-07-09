# PachaOS LPR state leak smoke — run via: bash /cmd/lpr_state_leak.sh

bb=/cmd/busybox

echo LPR_STATE_LEAK_BEFORE
$bb ls /bin >/dev/null

i=0
while [ "$i" -lt 50 ]; do
  if ! $bb ls /bin >/dev/null; then
    echo "LPR_STATE_LEAK_FAIL iteration=$i"
    exit 1
  fi
  i=$((i + 1))
done

echo LPR_STATE_LEAK_AFTER
$bb ls /bin >/dev/null
echo LPR_STATE_LEAK_DONE
