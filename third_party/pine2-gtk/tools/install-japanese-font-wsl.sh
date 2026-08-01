#!/usr/bin/env bash
set -euo pipefail

user_home="$(getent passwd "$(id -u)" | cut -d: -f6)"
if [[ -z "$user_home" || ! -d "$user_home" ]]; then
  echo "Current user's home directory could not be resolved" >&2
  exit 1
fi

data_dir="${XDG_DATA_HOME:-$user_home/.local/share}"
font_dir="$data_dir/fonts/pine2"
task_tmp="$(mktemp -d)"

cleanup() {
  rm -rf -- "$task_tmp"
}
trap cleanup EXIT

mkdir -p "$font_dir"
cd "$task_tmp"
apt-get download fonts-noto-cjk
package_file="$(find "$task_tmp" -maxdepth 1 -type f -name 'fonts-noto-cjk_*.deb' -print -quit)"

if [[ -z "$package_file" ]]; then
  echo "fonts-noto-cjk package download failed" >&2
  exit 1
fi

mkdir extracted
dpkg-deb -x "$package_file" extracted
font_file="$(find extracted/usr/share/fonts -type f -name 'NotoSansCJK-Regular.ttc' -print -quit)"

if [[ -z "$font_file" ]]; then
  echo "NotoSansCJK-Regular.ttc was not found" >&2
  exit 1
fi

install -m 0644 "$font_file" "$font_dir/NotoSansCJK-Regular.ttc"
fc-cache -f "$font_dir"
fc-match 'Noto Sans CJK JP'
