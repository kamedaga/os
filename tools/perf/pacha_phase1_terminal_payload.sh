#!/bin/sh
set -eu
marker=$1
input=$2
cat "$input"
: > "$marker"
# Keep the final frame mapped long enough for the controller to observe it.
sleep 1
