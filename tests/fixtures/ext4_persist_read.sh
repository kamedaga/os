# PachaOS ext4 persistence smoke (検証フェーズ) — run via: bash /cmd/ext4_r.sh
# 2 回目のブートで、前ブートの /cmd/ext4_w.sh が書いた内容の永続化を検証し、掃除する。

got=$(/cmd/busybox cat /p 2>/dev/null)
if [ "x$got" = "xp" ]; then
  echo "EXT4R_FILE=OK"
else
  echo "EXT4R_FILE=FAIL got=[$got]"
fi
if [ -d /kame ]; then
  echo "EXT4R_DIR=OK"
else
  echo "EXT4R_DIR=FAIL"
fi

/cmd/busybox rm -f /p 2>/dev/null
/cmd/busybox rmdir /kame 2>/dev/null
if [ ! -e /p ] && [ ! -d /kame ]; then
  echo "EXT4R_CLEAN=OK"
else
  echo "EXT4R_CLEAN=FAIL"
fi
/bin/sync
echo "EXT4R_SYNC=OK"
echo "EXT4R_DONE"
