#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
source_config=.artifacts/userland-fixtures/alpine-xfce-root/usr/share/dbus-1/system.conf
out=.artifacts/userland-fixtures/dbus-system.conf
test -f "$source_config"
test "$(grep -c '^[[:space:]]*<user>messagebus</user>[[:space:]]*$' "$source_config")" = 1
# Preserve the distribution's system-bus policy and service configuration.
# The supervisor already assigns messagebus and its groups before execution;
# a second in-daemon setuid/setgroups pass must not require extra authority.
sed -e 's@^[[:space:]]*<user>messagebus</user>[[:space:]]*$@  <!-- Identity is assigned by the CapabilityOS service launcher. -->@' \
    -e 's@<includedir>system.d</includedir>@<includedir>/usr/share/dbus-1/system.d</includedir>@' \
    "$source_config" > "$out"
