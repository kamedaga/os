#!/bin/sh
# Compare the exact slow IANA subresources independently of WebKit.
# Run with LD_LIBRARY_PATH=/opt/curl/lib on the guest. No profile changes.
label=${1:-guest}
base=https://www.iana.org
paths='/static/css/iana_website.0feeb53883fa.css
/static/js/jquery.a8e7cabd4d49.js
/static/js/dtable.46ee921d4414.js
/static/js/relative-time.79f0e30be3b8.js
/static/img/iana-logo-header.426b3ac01d35.svg
/static/fonts/NotoSans-Latin.b72e420edb95.ttf'
fetch()
{
    curl -sS -o /dev/null --connect-timeout 10 --max-time 25 \
        -w "RESOURCE_TIMING label=$label mode=$mode path=$1 status=%{http_code} dns=%{time_namelookup} connect=%{time_connect} tls=%{time_appconnect} first=%{time_starttransfer} total=%{time_total} bytes=%{size_download}\n" \
        "$base$1"
}
mode=serial
for path in $paths; do fetch "$path"; done
mode=parallel
for path in $paths; do fetch "$path" & done
wait
