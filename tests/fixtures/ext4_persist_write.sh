# PachaOS ext4 persistence smoke (書き込みフェーズ) — run via: bash /cmd/ext4_w.sh
# 1 回目のブートで実行。冪等: 前回の残骸を先に掃除する。
# 2 回目のブートで /cmd/ext4_r.sh が永続化を検証する。

/cmd/busybox rm -f /p 2>/dev/null
/cmd/busybox rmdir /kame 2>/dev/null

/cmd/busybox sh -c 'echo p > /p'
/cmd/busybox mkdir /kame
/bin/sync

got=$(/cmd/busybox cat /p 2>/dev/null)
if [ "x$got" = "xp" ]; then
  echo "EXT4W_FILE=OK"
else
  echo "EXT4W_FILE=FAIL got=[$got]"
fi
if [ -d /kame ]; then
  echo "EXT4W_DIR=OK"
else
  echo "EXT4W_DIR=FAIL"
fi
echo "EXT4W_DONE"
