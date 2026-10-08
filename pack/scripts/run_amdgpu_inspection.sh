#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Attended physical-GPU tests and interactive desktop monitoring. Finite
# smoke tests stop after completion; a desktop has no scheduled stop/poweroff.
set -euo pipefail

usage() {
    echo "usage: $0 root@host ssh-identity [package-dir] [--render-smoke ELF | --kms-smoke ELF | --mesa-render-smoke ELF RUNTIME.TAR.GZ | --mesa-gears ELF RUNTIME.TAR.GZ | --desktop]" >&2
    exit 2
}

[[ $# -ge 2 && $# -le 6 ]] || usage
target="$1"
identity="$2"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
package_dir="$repo_root/.artifacts/kobox2-amdgpu-package"
if [[ $# -ge 3 && "$3" != --render-smoke && "$3" != --kms-smoke &&
      "$3" != --mesa-render-smoke && "$3" != --mesa-gears && "$3" != --desktop ]]; then
    package_dir="$3"
    shift 3
else
    shift 2
fi
render_smoke=''
mesa_archive=''
smoke_kind=render
guest_smoke=/run/pacha/amdgpu-render-smoke.elf
guest_smoke_log=/run/pacha/mesa-render.log
mesa_render_limit="${CAPOS_MESA_RENDER_LIMIT_SECONDS:-25}"
if ! [[ "$mesa_render_limit" =~ ^[1-9][0-9]*$ ]] ||
   (( mesa_render_limit < 5 || mesa_render_limit > 60 )); then
    echo 'CAPOS_MESA_RENDER_LIMIT_SECONDS must be 5..60' >&2
    exit 2
fi
if [[ $# -gt 0 ]]; then
    if [[ $# -eq 2 && "$1" == --render-smoke ]]; then
        render_smoke="$2"
    elif [[ $# -eq 2 && "$1" == --kms-smoke ]]; then
        render_smoke="$2"
        smoke_kind=kms
    elif [[ $# -eq 3 && "$1" == --mesa-render-smoke ]]; then
        render_smoke="$2"
        mesa_archive="$3"
    elif [[ $# -eq 3 && "$1" == --mesa-gears ]]; then
        render_smoke="$2"
        mesa_archive="$3"
        smoke_kind=gears
        guest_smoke=/usr/bin/amdgpu-mesa-gears.elf
        guest_smoke_log=/run/pacha/mesa-gears.log
    elif [[ $# -eq 1 && "$1" == --desktop ]]; then
        smoke_kind=desktop
        render_smoke="$repo_root/pack/scripts/run_live_desktop_session.sh"
        guest_smoke=/run/pacha/live-desktop-session.sh
        guest_smoke_log=/run/pacha/desktop-session.log
    else
        usage
    fi
fi
gears_seconds="${CAPOS_MESA_GEARS_SECONDS:-60}"
if ! [[ "$gears_seconds" =~ ^[1-9][0-9]*$ ]] ||
   (( gears_seconds > 60 )); then
    echo 'CAPOS_MESA_GEARS_SECONDS must be 1..60' >&2
    exit 2
fi
[[ "$target" == root@* && -f "$identity" &&
   -f "$package_dir/owner-policy.conf" ]] || usage
[[ -z "$render_smoke" || -f "$render_smoke" ]] || usage
[[ -z "$mesa_archive" || -f "$mesa_archive" ]] || usage
interpreter=/lib/linux/ld-musl-x86_64.so.1
if [[ -n "$mesa_archive" ]]; then interpreter=/lib/ld-musl-x86_64.so.1; fi
if [[ -n "$render_smoke" && "$smoke_kind" != desktop ]] &&
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
emergency_stop=0
smoke_pid=0
last_service_seq=0
feedback_warning=0
host_identity_changed=0
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

check_host_identity() {
    if rg -q 'REMOTE HOST IDENTIFICATION HAS CHANGED|Host key verification failed' \
            "$session_dir/monitor-ssh-last.log"; then
        # A RAM-only host key changes on a new live boot. The old monitor no
        # longer owns that boot: retries consume unauthenticated SSH slots,
        # and cleanup must not send commands to a replacement session.
        host_identity_changed=1
        echo 'SSH host identity changed; retiring the old monitor without guest commands.' >&2
        return 1
    fi
    return 0
}

feedback_gap() {
    if [[ "$smoke_kind" == desktop ]]; then
        # The sandbox independently owns the sensor and 70 C cutoff. SSH
        # latency is loss of host visibility, not evidence of GPU failure;
        # tearing down here ends a healthy interactive session under load.
        if ((feedback_warning == 0)); then
            echo "Desktop monitoring delayed: $1; retrying, no stop or poweroff requested." >&2
        fi
        feedback_warning=1
        return 0
    fi
    echo "Lost inspection feedback: $1; ending finite test." >&2
    return 1
}

cleanup() {
    local original_status=$? lines stopped=0
    trap - EXIT
    if ((smoke_pid)); then
        kill "$smoke_pid" >/dev/null 2>&1 || :
        wait "$smoke_pid" >/dev/null 2>&1 || :
    fi
    if ((host_identity_changed)); then
        echo "Inspection host logs: $session_dir (host identity changed; no guest cleanup requested)" >&2
        return "$original_status"
    fi
    if ((probe_sent)) && [[ "$smoke_kind" == desktop ]]; then
        for file in desktop-session.log Xorg.0.log desktop-xrandr.log; do
            timeout 2s ssh "${ssh_options[@]}" "$target" \
                "busybox cat /run/pacha/$file" > "$session_dir/$file" 2>/dev/null || :
        done
    fi
    # Leaving the monitor or logging out is not permission to stop a healthy
    # interactive GPU. Only safety failures enter automatic desktop teardown.
    if ((probe_sent)) && [[ "$smoke_kind" != desktop || $emergency_stop -eq 1 ]]; then
        if [[ "$smoke_kind" == desktop ]]; then
            # xinit owns the server and client process groups. Terminate it
            # before revoking the GPU, including host timeout/error exits.
            timeout 4s ssh "${ssh_options[@]}" "$target" \
                'if busybox test -f /run/pacha/desktop-xinit.pid; then p=$(busybox cat /run/pacha/desktop-xinit.pid); case "$p" in ""|*[!0-9]*) exit 1;; esac; busybox kill -TERM "$p" 2>/dev/null || :; fi' >/dev/null 2>&1 || :
        fi
        # A safety failure cannot leave the GPU running without feedback.
        # Finite smoke tests also explicitly own teardown of their probe.
        timeout 3s ssh "${ssh_options[@]}" "$target" \
            "busybox printf 'stop\\n' > /run/pacha/gpu-control" >/dev/null 2>&1 || :
        stop_deadline=$(( $(date +%s) + 12 ))
        while (( $(date +%s) < stop_deadline )); do
            lines="$(timeout 2s ssh "${ssh_options[@]}" "$target" \
                'busybox tail -n 100 /run/pacha/service.log' 2>/dev/null || :)"
            if [[ -n "$lines" ]]; then
                record_service_lines "$lines"
                printf '%s\n' "$lines" > "$session_dir/service-stop.log"
            fi
            if [[ "$lines" == *"PCI DRM probe stopped status="* ||
                  "$lines" == *"startup process stop status=0"* ]]; then
                printf '%s\n' "$lines" |
                    rg 'kobox-drm: inspection (abort|sensor unavailable)|PCI DRM probe stopped status=|startup process stop status=' |
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
    elif ((probe_sent)) && [[ "$smoke_kind" == desktop ]]; then
        echo 'Desktop/GPU left running; no stop or poweroff requested.' >&2
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
    # A corrected client is small; the large Mesa closure remains the exact
    # SHA-checked bootfs copy. Copy the client to RAM even for gears so a
    # diagnostic iteration never silently executes an older embedded ELF.
    timeout 30s ssh "${ssh_options[@]}" "$target" \
        "busybox dd of=$guest_smoke bs=1048576" \
        < "$render_smoke" >/dev/null 2>&1
    remote "busybox chmod 700 $guest_smoke"
    remote_digest="$(remote "busybox sha256sum $guest_smoke" |
        cut -d' ' -f1)"
    [[ "$remote_digest" == "$local_digest" ]] || {
        echo 'GPU test executable digest mismatch; refusing probe.' >&2
        exit 1
    }
    # Exercise the exact executable/SSH path before touching the GPU. A
    # fresh live boot has no render node, so only render-open may fail.
    preflight_status=0
    if [[ "$smoke_kind" == desktop ]]; then
        remote 'busybox test -f /run/pacha/live-desktop.ready && busybox test -x /usr/bin/xinit && busybox test -x /usr/libexec/Xorg && busybox test -x /usr/bin/startxfce4 && busybox test -x /usr/bin/xrandr'
        # Retain root capacity on the host before probe even if later ENOSPC
        # prevents the desktop session from opening its guest-side log file.
        remote 'busybox df -k /; busybox df -i /' > "$session_dir/filesystem-preflight.log"
        preflight_status=1
        printf '%s\n' 'LIVE_DESKTOP_PREFLIGHT ready; GPU remains unprobed' > "$session_dir/render-preflight.log"
    elif [[ -n "$mesa_archive" ]]; then
        timeout 10s ssh "${ssh_options[@]}" "$target" \
            "busybox sh -c 'LD_LIBRARY_PATH=/usr/lib:/lib LIBGL_DRIVERS_PATH=/usr/lib/dri GBM_BACKENDS_PATH=/usr/lib/gbm $guest_smoke > $guest_smoke_log 2>&1; result=\$?; busybox cat $guest_smoke_log; exit \$result'" \
            > "$session_dir/render-preflight.log" 2>&1 || preflight_status=$?
    else
        timeout 10s ssh "${ssh_options[@]}" "$target" \
            "$guest_smoke" \
            > "$session_dir/render-preflight.log" 2>&1 || preflight_status=$?
    fi
    preflight_marker='^fail render-open errno='
    if [[ "$smoke_kind" == desktop ]]; then
        preflight_marker='^LIVE_DESKTOP_PREFLIGHT '
    elif [[ -n "$mesa_archive" ]]; then
        preflight_marker='^MESA_RENDER_FAIL stage=render-open'
        if [[ "$smoke_kind" == gears ]]; then
            preflight_marker='^MESA_GEARS_FAIL stage=card-open'
        fi
    elif [[ "$smoke_kind" == kms ]]; then
        preflight_marker='^KMS_ATOMIC_FAIL stage=card-open'
    fi
    if ((preflight_status != 1)) ||
       ! rg -q "$preflight_marker" "$session_dir/render-preflight.log"; then
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
iommu_events_before=0
if [[ "$smoke_kind" == kms || "$smoke_kind" == gears ]]; then
    iommu_events_before="$(remote "busybox grep -c 'amd-iommu: event unit=' /run/pacha/kernel.log" || :)"
    if ! [[ "$iommu_events_before" =~ ^[0-9]+$ ]]; then
        echo 'Cannot establish the pre-probe IOMMU fault baseline.' >&2
        exit 1
    fi
fi

# Mark the run before sending: a broken SSH response must still enter cleanup.
probe_sent=1
started_at="$(date +%s)"
remote "busybox printf 'probe\\n' > /run/pacha/gpu-control"
echo 'Probe requested; monitoring live sensor log (temperature cutoff: 70 C).'
if [[ "$smoke_kind" == desktop ]]; then
    echo 'Interactive desktop: no duration limit or automatic shutdown.'
fi
last_sample=''
last_sample_at=0
smoke_started=0
ready_seen=0
last_gear_fps=''
next_gear_report=0
desktop_ready_reported=0
while :; do
    lines="$(remote 'busybox tail -n 100 /run/pacha/service.log' 2>"$session_dir/monitor-ssh-last.log" || :)"
    cat "$session_dir/monitor-ssh-last.log" >> "$session_dir/monitor-ssh.log"
    if ! check_host_identity; then exit 1; fi
    if [[ -z "$lines" ]]; then
        if feedback_gap 'SSH log read failed'; then
            sleep 2
            continue
        fi
        emergency_stop=1
        exit 1
    fi
    if [[ "$smoke_kind" == kms || "$smoke_kind" == gears ]]; then
        iommu_events_now="$(remote "busybox grep -c 'amd-iommu: event unit=' /run/pacha/kernel.log" || :)"
        if ! [[ "$iommu_events_now" =~ ^[0-9]+$ ]] ||
           (( iommu_events_now > iommu_events_before )); then
            echo "IOMMU fault changed during KMS inspection: before=$iommu_events_before now=$iommu_events_now" >&2
            exit 1
        fi
    fi
    record_service_lines "$lines"
    sample="$(printf '%s\n' "$lines" | rg 'kobox-drm: inspection sample=' | tail -n 1 || :)"
    if [[ -n "$sample" && "$sample" != "$last_sample" ]]; then
        if ((feedback_warning)); then echo 'Desktop temperature monitoring resumed.'; fi
        feedback_warning=0
        echo "$sample"
        last_sample="$sample"
        last_sample_at="$(date +%s)"
    fi
    if [[ "$lines" == *"PCI DRM probe ready status=0"* ]]; then
        ready_seen=1
    fi
    if [[ "$lines" == *"PCI DRM probe stopped status="* ]]; then
        emergency_stop=1
        if [[ -z "$render_smoke" ]]; then
            echo 'GPU inspection stopped.'
            break
        fi
        echo 'GPU inspection stopped before render test completed.' >&2
        # A transport failure can retire the sandbox before the SSH child
        # returns. Preserve both diagnostics before the poweroff trap runs.
        printf '%s\n' "$lines" > "$session_dir/service-failure.log"
        timeout 2s ssh "${ssh_options[@]}" "$target" \
            "busybox cat $guest_smoke_log" \
            > "$session_dir/mesa-render-guest.log" 2>/dev/null || :
        exit 1
    fi
    if [[ "$lines" == *"sandbox startup failed status="* ||
          "$lines" == *"DRM discovery publication failed status="* ]]; then
        emergency_stop=1
        printf '%s\n' "$lines" > "$session_dir/service-failure.log"
        printf '%s\n' "$lines" |
            rg 'cache transition stage=|sandbox exit state=|sandbox exit observation failed|startup process stop status=|sandbox startup failed status=|DRM discovery publication failed status=' |
            tail -n 12 >&2 || :
        echo 'GPU sandbox failed before render readiness.' >&2
        exit 1
    fi
    if [[ "$lines" == *"kobox-drm: inspection abort"* ||
          "$lines" == *"kobox-drm: inspection sensor unavailable"* ]]; then
        emergency_stop=1
        echo 'Guest inspection limit reached; ending this inspection.' >&2
        break
    fi
    if (( smoke_started && $(date +%s) - last_sample_at > 5 )); then
        if ! feedback_gap 'temperature log sample delayed'; then
            emergency_stop=1
            exit 1
        fi
    fi
    if [[ "$smoke_kind" != desktop ]] && (( $(date +%s) - started_at >= 150 )); then
        echo 'Independent 150-second host deadline reached.' >&2
        exit 1
    fi
    # Do not dispatch work until the guest has produced at least one live
    # temperature sample; otherwise a ready message can race sensor startup.
    if [[ -n "$render_smoke" && $smoke_started -eq 0 &&
          -n "$sample" && $ready_seen -eq 1 ]] &&
       remote 'busybox test -e /dev/dri/card0 && busybox test -e /dev/dri/renderD128' \
           >/dev/null 2>&1; then
        smoke_started=1
        if [[ "$smoke_kind" == desktop ]]; then
            # The desktop must not inherit the lifetime of this SSH monitor.
            # Detach with normal process/session semantics; continue watching
            # the RAM log and thermal telemetry over independent connections.
            remote "busybox setsid busybox sh $guest_smoke > $guest_smoke_log 2>&1 < /dev/null &"
        elif [[ -n "$mesa_archive" ]]; then
            job_limit="$mesa_render_limit"
            job_arguments=''
            if [[ "$smoke_kind" == gears ]]; then
                # Guest monotonic time advanced slower than host time under
                # sustained rendering. Preserve headroom for 60 guest seconds;
                # the independent 150 s host and 70 C thermal limits still apply.
                job_limit="$((gears_seconds + 45))"
                job_arguments="--seconds $gears_seconds"
            fi
            timeout "$((job_limit + 5))s" ssh "${ssh_options[@]}" "$target" \
                "busybox sh -c 'LD_LIBRARY_PATH=/usr/lib:/lib LIBGL_DRIVERS_PATH=/usr/lib/dri GBM_BACKENDS_PATH=/usr/lib/gbm busybox timeout -s KILL $job_limit $guest_smoke $job_arguments > $guest_smoke_log 2>&1; result=\$?; busybox cat $guest_smoke_log; exit \$result'" \
                > "$session_dir/render-job.log" 2>&1 &
        else
            timeout 20s ssh "${ssh_options[@]}" "$target" \
                "$guest_smoke" \
                > "$session_dir/render-job.log" 2>&1 &
        fi
        if [[ "$smoke_kind" != desktop ]]; then smoke_pid=$!; fi
        if [[ "$smoke_kind" == desktop ]]; then
            echo 'Desktop running with live temperature monitoring; no shutdown scheduled.'
        else
            echo 'GPU job running under independent host deadline.'
        fi
    fi
    if [[ "$smoke_kind" == desktop && $smoke_started -eq 1 ]]; then
        desktop_log="$(remote "busybox tail -n 100 $guest_smoke_log" 2>/dev/null || :)"
        printf '%s\n' "$desktop_log" > "$session_dir/render-job.log"
        if [[ "$desktop_log" == *"LIVE_DESKTOP_X11_READY "* &&
              $desktop_ready_reported -eq 0 ]]; then
            echo 'X11 ready; desktop stays running for user interaction.'
            desktop_ready_reported=1
        fi
        if [[ "$desktop_log" == *"LIVE_DESKTOP_FAIL "* ]]; then
            printf '%s\n' "$desktop_log" >&2
            echo 'Desktop startup failed; GPU remains monitored in the guest, no scheduled shutdown.' >&2
            exit 1
        fi
        if [[ "$desktop_log" == *"LIVE_DESKTOP_SESSION_EXIT "* ]]; then
            printf '%s\n' "$desktop_log"
            echo 'Desktop session exited; GPU remains active until an explicit stop.'
            break
        fi
    fi
    if [[ ( "$smoke_kind" == kms || "$smoke_kind" == gears ) &&
          $smoke_started -eq 1 &&
          -f "$session_dir/render-job.log" ]] &&
       rg -q '^(KMS_ATOMIC_FAIL|MESA_GEARS_FAIL) ' "$session_dir/render-job.log"; then
        sed -n '1,30p' "$session_dir/render-job.log" >&2
        echo 'KMS test failed; stopping the GPU without waiting for SSH process teardown.' >&2
        exit 1
    fi
    if [[ "$smoke_kind" == gears && $smoke_started -eq 1 ]] &&
       (( $(date +%s) >= next_gear_report )); then
        next_gear_report=$(( $(date +%s) + 4 ))
        # The SSH job returns its redirected log only on exit. Sample the
        # RAM file while rendering so FPS remains observable on a timeout.
        gear_fps="$(timeout 3s ssh "${ssh_options[@]}" "$target" \
            "busybox tail -n 12 $guest_smoke_log" 2>/dev/null |
            rg '^MESA_GEARS_FPS ' | tail -n 1 || :)"
        if [[ -n "$gear_fps" && "$gear_fps" != "$last_gear_fps" ]]; then
            printf '%s\n' "$gear_fps" | tee -a "$session_dir/render-progress.log"
            last_gear_fps="$gear_fps"
        fi
    fi
    if ((smoke_pid)) && ! kill -0 "$smoke_pid" >/dev/null 2>&1; then
        smoke_status=0
        wait "$smoke_pid" || smoke_status=$?
        smoke_pid=0
        sed -n '1,100p' "$session_dir/render-job.log"
        pass_marker='^PASS sequence='
        if [[ -n "$mesa_archive" ]]; then pass_marker='^MESA_RENDER_PASS renderer='; fi
        if [[ "$smoke_kind" == kms ]]; then pass_marker='^KMS_ATOMIC_DISPLAY_PASS '; fi
        if [[ "$smoke_kind" == gears ]]; then pass_marker='^MESA_GEARS_PASS '; fi
        if [[ "$smoke_kind" == desktop ]]; then pass_marker='^LIVE_DESKTOP_X11_READY '; fi
        if ((smoke_status)) ||
           ! rg -q "$pass_marker" "$session_dir/render-job.log" ||
           { [[ "$smoke_kind" == gears ]] &&
             { ! rg -q '^MESA_GEARS_SCENE gears=3 geometry=extruded ' \
                   "$session_dir/render-job.log" ||
               ! rg -q '^MESA_GEARS_FPS ' "$session_dir/render-job.log"; }; }; then
            echo "Render job failed, ssh status=$smoke_status" >&2
            if [[ -n "$mesa_archive" ]]; then
                timeout 3s ssh "${ssh_options[@]}" "$target" \
                    "busybox cat $guest_smoke_log" \
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
        if [[ "$smoke_kind" == desktop ]]; then
            echo 'Desktop session exited; GPU remains active until an explicit stop.'
        else
            echo 'Render job passed; stopping GPU now.'
        fi
        break
    fi
    if [[ -n "$render_smoke" && $smoke_started -eq 0 ]] &&
       (( $(date +%s) - started_at >= 45 )); then
        echo 'Render node did not become ready in 45 seconds.' >&2
        exit 1
    fi
    sleep 1
done
