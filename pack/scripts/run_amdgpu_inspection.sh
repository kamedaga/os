#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# One attended physical-GPU inspection. The guest checks its live sensor;
# this independent host deadline requests stop and then full system poweroff.
set -euo pipefail

usage() {
    echo "usage: $0 root@host ssh-identity [package-dir] [--render-smoke ELF | --mesa-render-smoke ELF RUNTIME.TAR.GZ]" >&2
    exit 2
}

[[ $# -ge 2 && $# -le 6 ]] || usage
target="$1"
identity="$2"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
package_dir="$repo_root/.artifacts/kobox2-amdgpu-package"
if [[ $# -ge 3 && "$3" != --render-smoke && "$3" != --mesa-render-smoke ]]; then
    package_dir="$3"
    shift 3
else
    shift 2
fi
render_smoke=''
mesa_archive=''
mesa_render_limit="${CAPOS_MESA_RENDER_LIMIT_SECONDS:-25}"
if ! [[ "$mesa_render_limit" =~ ^[1-9][0-9]*$ ]] ||
   (( mesa_render_limit < 5 || mesa_render_limit > 60 )); then
    echo 'CAPOS_MESA_RENDER_LIMIT_SECONDS must be 5..60' >&2
    exit 2
fi
if [[ $# -gt 0 ]]; then
    if [[ $# -eq 2 && "$1" == --render-smoke ]]; then
        render_smoke="$2"
    elif [[ $# -eq 3 && "$1" == --mesa-render-smoke ]]; then
        render_smoke="$2"
        mesa_archive="$3"
    else
        usage
    fi
fi
[[ "$target" == root@* && -f "$identity" &&
   -f "$package_dir/owner-policy.conf" ]] || usage
[[ -z "$render_smoke" || -f "$render_smoke" ]] || usage
[[ -z "$mesa_archive" || -f "$mesa_archive" ]] || usage
interpreter=/lib/linux/ld-musl-x86_64.so.1
if [[ -n "$mesa_archive" ]]; then interpreter=/lib/ld-musl-x86_64.so.1; fi
if [[ -n "$render_smoke" ]] &&
   ! readelf -l "$render_smoke" | grep -q \
       "Requesting program interpreter: $interpreter"; then
    echo 'Render smoke must be a Linux/musl LPR executable.' >&2
    exit 2
fi

mkdir -p "$repo_root/.artifacts/amdgpu-inspections"
session_dir="$(mktemp -d "$repo_root/.artifacts/amdgpu-inspections/run-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
ssh_options=(-i "$identity" -o BatchMode=yes -o IdentitiesOnly=yes
    -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new
    -o "UserKnownHostsFile=$session_dir/known_hosts" -o LogLevel=ERROR)
probe_sent=0
smoke_pid=0
last_service_seq=0
remote() { timeout 10s ssh "${ssh_options[@]}" "$target" "$1"; }

# service.log keeps only a recent window. Retain each observed sequence number
# on the host while the GPU is active, so a noisy stop path cannot erase the
# ioctl/cache stage that preceded a render timeout.
record_service_lines() {
    local line number
    while IFS= read -r line; do
        [[ "$line" =~ ^#([0-9]+)[[:space:]] ]] || continue
        number="${BASH_REMATCH[1]}"
        if (( number <= last_service_seq )); then continue; fi
        if (( last_service_seq != 0 && number > last_service_seq + 1 )); then
            printf 'SERVICE_LOG_GAP after=%s before=%s\n' \
                "$last_service_seq" "$number" >> "$session_dir/service-live.log"
        fi
        printf '%s\n' "$line" >> "$session_dir/service-live.log"
        last_service_seq="$number"
    done <<< "$1"
}

cleanup() {
    local original_status=$? lines stopped=0
    trap - EXIT
    if ((smoke_pid)); then
        kill "$smoke_pid" >/dev/null 2>&1 || :
        wait "$smoke_pid" >/dev/null 2>&1 || :
    fi
    if ((probe_sent)); then
        # A failed temperature read or lost log connection cannot leave the
        # physical GPU under test indefinitely. The RAM command is idempotent.
        timeout 3s ssh "${ssh_options[@]}" "$target" \
            "busybox printf 'stop\\n' > /run/pacha/gpu-control" >/dev/null 2>&1 || :
        for ((attempt = 0; attempt < 3; ++attempt)); do
            lines="$(timeout 2s ssh "${ssh_options[@]}" "$target" \
                'busybox tail -n 100 /run/pacha/service.log' 2>/dev/null || :)"
            if [[ -n "$lines" ]]; then
                record_service_lines "$lines"
                printf '%s\n' "$lines" > "$session_dir/service-stop.log"
            fi
            if [[ "$lines" == *"PCI DRM probe stopped status="* ||
                  "$lines" == *"startup process stop status=0"* ]]; then
                printf '%s\n' "$lines" |
                    rg 'kobox-drm: inspection (abort|sensor unavailable|time limit reached)|PCI DRM probe stopped status=|startup process stop status=' |
                    tail -n 5 >&2
                stopped=1
                break
            fi
            sleep 1
        done
        if ((stopped)); then
            echo 'GPU sandbox stop recorded; requesting ACPI poweroff.' >&2
            if [[ "$lines" != *"PCI DRM probe stopped status=0"* &&
                  "$lines" != *"startup process stop status=0"* ]]; then
                # A passed draw is not a passed inspection if Linux failed
                # to release the PCI device before the host revokes DMA.
                echo 'GPU sandbox stop failed; render result is retained separately.' >&2
                original_status=1
            fi
        else
            echo 'GPU stop not confirmed promptly; requesting ACPI poweroff now.' >&2
            original_status=1
        fi
        # The debug kernel ring is RAM-only. Capture it after the sandbox has
        # stopped but before ACPI poweroff, independently of service.log.
        if timeout 2s ssh "${ssh_options[@]}" "$target" \
            'busybox cat /run/pacha/kernel.log' \
            > "$session_dir/kernel.log" 2>/dev/null; then
            echo "Kernel ring captured: $session_dir/kernel.log" >&2
        fi
        timeout 3s ssh "${ssh_options[@]}" "$target" \
            'busybox poweroff -f' >/dev/null 2>&1 || :
        echo 'Poweroff requested. If the sub PC remains on, power it off locally and report the failure.' >&2
    fi
    echo "Inspection host logs: $session_dir" >&2
    return "$original_status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Reuse the SHA-checked RAM transfer; do not rewrite the USB or start a probe
# until the package and its separate owner policy have both been verified.
bash "$repo_root/pack/scripts/upload_amdgpu_probe.sh" \
    "$target" "$identity" "$package_dir"

if [[ -n "$mesa_archive" ]]; then
    # The optional bootfs carries the Mesa closure. Verify its exact bytes
    # against the host package before expanding them into the writable root.
    archive_sha256="$(sha256sum "$mesa_archive" | cut -d' ' -f1)"
    archive_guest=/srv/amdgpu-mesa-runtime.tar.gz
    [[ "$(timeout 120s ssh "${ssh_options[@]}" "$target" "busybox sha256sum $archive_guest" | cut -d' ' -f1)" == "$archive_sha256" ]] || {
        echo 'Bootfs Mesa archive digest mismatch; refusing probe.' >&2
        exit 1
    }
    timeout 300s ssh "${ssh_options[@]}" "$target" \
        "busybox tar -xzf $archive_guest -C /" >/dev/null
    echo "Mesa closure verified and unpacked in RAM: sha256=$archive_sha256"
fi

if [[ -n "$render_smoke" ]]; then
    local_digest="$(sha256sum "$render_smoke" | cut -d' ' -f1)"
    timeout 30s ssh "${ssh_options[@]}" "$target" \
        'busybox dd of=/run/pacha/amdgpu-render-smoke.elf bs=1048576' \
        < "$render_smoke" >/dev/null 2>&1
    remote 'busybox chmod 700 /run/pacha/amdgpu-render-smoke.elf'
    remote_digest="$(remote 'busybox sha256sum /run/pacha/amdgpu-render-smoke.elf' |
        cut -d' ' -f1)"
    [[ "$remote_digest" == "$local_digest" ]] || {
        echo 'RAM render smoke digest mismatch; refusing probe.' >&2
        exit 1
    }
    # Exercise the exact executable/SSH path before touching the GPU. A
    # fresh live boot has no render node, so only render-open may fail.
    preflight_status=0
    if [[ -n "$mesa_archive" ]]; then
        timeout 10s ssh "${ssh_options[@]}" "$target" \
            "busybox sh -c 'LD_LIBRARY_PATH=/usr/lib:/lib LIBGL_DRIVERS_PATH=/usr/lib/dri GBM_BACKENDS_PATH=/usr/lib/gbm /run/pacha/amdgpu-render-smoke.elf > /run/pacha/mesa-render.log 2>&1; result=\$?; busybox cat /run/pacha/mesa-render.log; exit \$result'" \
            > "$session_dir/render-preflight.log" 2>&1 || preflight_status=$?
    else
        timeout 10s ssh "${ssh_options[@]}" "$target" \
            '/run/pacha/amdgpu-render-smoke.elf' \
            > "$session_dir/render-preflight.log" 2>&1 || preflight_status=$?
    fi
    if ((preflight_status != 1)) ||
       ! rg -q "$([[ -n "$mesa_archive" ]] && echo '^MESA_RENDER_FAIL stage=render-open' || echo '^fail render-open errno=')" "$session_dir/render-preflight.log"; then
        echo "Render executable preflight failed, ssh status=$preflight_status:" >&2
        sed -n '1,30p' "$session_dir/render-preflight.log" >&2
        exit 1
    fi
fi

before="$(remote 'busybox tail -n 256 /run/pacha/service.log')"
record_service_lines "$before"
if [[ "$before" != *"PCI DRM inspection armed; awaiting RAM command"* ||
      "$before" == *"manual PCI DRM probe requested"* ]] ||
   ! remote 'busybox test ! -e /run/pacha/gpu-control'; then
    echo 'GPU inspection is not armed for a fresh one-shot probe.' >&2
    exit 1
fi

# Mark the run before sending: a broken SSH response must still enter cleanup.
probe_sent=1
started_at="$(date +%s)"
remote "busybox printf 'probe\\n' > /run/pacha/gpu-control"
echo 'Probe requested; monitoring live sensor log (guest limit: 70 C / 120 s).'
last_sample=''
smoke_started=0
while :; do
    lines="$(remote 'busybox tail -n 100 /run/pacha/service.log' 2>/dev/null || :)"
    if [[ -z "$lines" ]]; then
        echo 'Lost diagnostic log over SSH; ending this inspection.' >&2
        exit 1
    fi
    record_service_lines "$lines"
    sample="$(printf '%s\n' "$lines" | rg 'kobox-drm: inspection sample=' | tail -n 1 || :)"
    if [[ -n "$sample" && "$sample" != "$last_sample" ]]; then
        echo "$sample"
        last_sample="$sample"
    fi
    if [[ "$lines" == *"PCI DRM probe stopped status="* ]]; then
        if [[ -z "$render_smoke" ]]; then
            echo 'GPU inspection stopped.'
            break
        fi
        echo 'GPU inspection stopped before render test completed.' >&2
        # A transport failure can retire the sandbox before the SSH child
        # returns. Preserve both diagnostics before the poweroff trap runs.
        printf '%s\n' "$lines" > "$session_dir/service-failure.log"
        timeout 2s ssh "${ssh_options[@]}" "$target" \
            'busybox cat /run/pacha/mesa-render.log' \
            > "$session_dir/mesa-render-guest.log" 2>/dev/null || :
        exit 1
    fi
    if [[ "$lines" == *"sandbox startup failed status="* ]]; then
        printf '%s\n' "$lines" > "$session_dir/service-failure.log"
        printf '%s\n' "$lines" |
            rg 'cache transition stage=|sandbox exit state=|sandbox exit observation failed|startup process stop status=|sandbox startup failed status=' |
            tail -n 12 >&2 || :
        echo 'GPU sandbox failed before render readiness.' >&2
        exit 1
    fi
    if [[ "$lines" == *"kobox-drm: inspection abort"* ||
          "$lines" == *"kobox-drm: inspection sensor unavailable"* ||
          "$lines" == *"kobox-drm: inspection time limit reached"* ]]; then
        echo 'Guest inspection limit reached; ending this inspection.' >&2
        break
    fi
    if (( $(date +%s) - started_at >= 150 )); then
        echo 'Independent 150-second host deadline reached.' >&2
        exit 1
    fi
    # Do not dispatch work until the guest has produced at least one live
    # temperature sample; otherwise a ready message can race sensor startup.
    if [[ -n "$render_smoke" && $smoke_started -eq 0 &&
          -n "$sample" && "$lines" == *"PCI DRM probe ready status=0"* ]]; then
        smoke_started=1
        if [[ -n "$mesa_archive" ]]; then
            timeout "$((mesa_render_limit + 5))s" ssh "${ssh_options[@]}" "$target" \
                "busybox sh -c 'LD_LIBRARY_PATH=/usr/lib:/lib LIBGL_DRIVERS_PATH=/usr/lib/dri GBM_BACKENDS_PATH=/usr/lib/gbm busybox timeout -s KILL $mesa_render_limit /run/pacha/amdgpu-render-smoke.elf > /run/pacha/mesa-render.log 2>&1; result=\$?; busybox cat /run/pacha/mesa-render.log; exit \$result'" \
                > "$session_dir/render-job.log" 2>&1 &
        else
            timeout 20s ssh "${ssh_options[@]}" "$target" \
                '/run/pacha/amdgpu-render-smoke.elf' \
                > "$session_dir/render-job.log" 2>&1 &
        fi
        smoke_pid=$!
        echo "Render-node job running under independent $([[ -n "$mesa_archive" ]] && echo "$((mesa_render_limit + 5))" || echo 20)-second host deadline."
    fi
    if ((smoke_pid)) && ! kill -0 "$smoke_pid" >/dev/null 2>&1; then
        smoke_status=0
        wait "$smoke_pid" || smoke_status=$?
        smoke_pid=0
        sed -n '1,100p' "$session_dir/render-job.log"
        pass_marker='^PASS sequence='
        if [[ -n "$mesa_archive" ]]; then pass_marker='^MESA_RENDER_PASS renderer='; fi
        if ((smoke_status)) ||
           ! rg -q "$pass_marker" "$session_dir/render-job.log"; then
            echo "Render job failed, ssh status=$smoke_status" >&2
            if [[ -n "$mesa_archive" ]]; then
                timeout 3s ssh "${ssh_options[@]}" "$target" \
                    'busybox cat /run/pacha/mesa-render.log' \
                    > "$session_dir/mesa-render-guest.log" 2>/dev/null || :
                sed -n '1,100p' "$session_dir/mesa-render-guest.log" >&2
            fi
            timeout 2s ssh "${ssh_options[@]}" "$target" \
                'busybox cat /run/pacha/service.log' \
                > "$session_dir/service-failure.log" 2>/dev/null || :
            rg 'DRM ioctl failed|inspection sample=|PCI DRM probe ready' \
                "$session_dir/service-failure.log" | tail -n 50 >&2 || :
            exit 1
        fi
        echo 'Render job passed; stopping GPU now.'
        break
    fi
    if [[ -n "$render_smoke" && $smoke_started -eq 0 ]] &&
       (( $(date +%s) - started_at >= 45 )); then
        echo 'Render node did not become ready in 45 seconds.' >&2
        exit 1
    fi
    sleep 1
done
