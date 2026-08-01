#!/usr/bin/env bash

# Remove an image only while holding the same permanent sidecar lock used by
# pacgo. This deliberately fails rather than killing an unrelated QEMU that
# still owns the disk image.
pacgo_remove_image_locked() {
  local image="$1"
  local canonical lock_dir lock_file lock_fd
  canonical="$(realpath -m -- "$image")"
  lock_dir="$(dirname -- "$canonical")"
  lock_file="${canonical}.pacgo.lock"
  mkdir -p -- "$lock_dir"
  exec {lock_fd}>"$lock_file"
  if ! flock -n "$lock_fd"; then
    echo "image is busy: $canonical (held by another pacgo image operation)" >&2
    exec {lock_fd}>&-
    return 1
  fi
  rm -f -- "$canonical"
  flock -u "$lock_fd"
  exec {lock_fd}>&-
}
