#!/bin/sh
# Read-only transport baseline, separate from WebKit rendering and subresources.
# Optional label distinguishes host/guest runs. No credentials or profiles read.
# PachaOS's separately staged curl requires its matching private libraries:
# LD_LIBRARY_PATH=/opt/curl/lib sh /root/network-timing.sh guest
label=${1:-guest}
version=$(curl --version) || exit $?
printf '%s\n' "$version"
for pass in 1 2; do
    for url in https://www.iana.org/domains/reserved https://www.wikipedia.org/ https://www.python.org/ https://www.google.com/ https://www.gnu.org/; do
        printf 'NETWORK_TIMING label=%s pass=%s url=%s ' "$label" "$pass" "$url"
        curl -L -sS -o /dev/null --connect-timeout 10 --max-time 25 \
            -w 'status=%{http_code} dns=%{time_namelookup} connect=%{time_connect} tls=%{time_appconnect} first=%{time_starttransfer} total=%{time_total} bytes=%{size_download}\n' "$url"
        printf 'NETWORK_EXIT=%s\n' "$?"
    done
done
