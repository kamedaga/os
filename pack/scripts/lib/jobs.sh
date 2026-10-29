#!/usr/bin/env bash

pacgo_has_jobserver() {
    [[ " ${MAKEFLAGS:-} " == *" --jobserver-auth="* ||
       " ${MAKEFLAGS:-} " == *" --jobserver-fds="* ]]
}

pacgo_make() {
    local fallback_jobs="$1"
    shift
    if pacgo_has_jobserver; then
        command make "$@"
    else
        command make -j"$fallback_jobs" "$@"
    fi
}

# Ninja joins only the FIFO jobserver, and only from 1.13. Given descriptor
# authentication (from an outer make started with --jobserver-style=pipe) or
# an older Ninja it runs at its own default width, so it gets explicitly
# reserved tokens instead.
_pacgo_ninja_joins_jobserver() {
    local ninja="$1" auth version major minor
    auth="$(_pacgo_jobserver_auth)" && [[ "$auth" == fifo:* ]] || return 1
    version="$("$ninja" --version)" || return 1
    [[ "$version" =~ ^([0-9]+)\.([0-9]+) ]] || return 1
    major="${BASH_REMATCH[1]}"
    minor="${BASH_REMATCH[2]}"
    ((major > 1 || (major == 1 && minor >= 13)))
}

# Prints the CMakeCache.txt value of a cache entry, such as CMAKE_GENERATOR or
# CMAKE_MAKE_PROGRAM, for the build directory $1.
_pacgo_cmake_cache_value() {
    local line
    [[ -f "$1/CMakeCache.txt" ]] || return 0
    while IFS= read -r line; do
        if [[ "$line" == "$2":*=* ]]; then
            printf '%s\n' "${line#*=}"
            return 0
        fi
    done < "$1/CMakeCache.txt"
}

pacgo_cmake_build() {
    local fallback_jobs="$1" status=0
    shift
    if ! pacgo_has_jobserver; then
        command cmake --build "$@" --parallel "$fallback_jobs"
    elif [[ "$(_pacgo_cmake_cache_value "$1" CMAKE_GENERATOR)" != Ninja ]] ||
         _pacgo_ninja_joins_jobserver "$(_pacgo_cmake_cache_value "$1" CMAKE_MAKE_PROGRAM)"; then
        command cmake --build "$@"
    else
        pacgo_reserve_jobs "$fallback_jobs" >/dev/null
        command cmake --build "$@" --parallel "$PACGO_RESERVED_JOBS" || status=$?
        pacgo_release_jobs
        return "$status"
    fi
}

pacgo_ninja() {
    local fallback_jobs="$1" status=0
    shift
    if ! pacgo_has_jobserver; then
        command ninja -j"$fallback_jobs" "$@"
    elif _pacgo_ninja_joins_jobserver ninja; then
        command ninja "$@"
    else
        pacgo_reserve_jobs "$fallback_jobs" >/dev/null
        command ninja -j"$PACGO_RESERVED_JOBS" "$@" || status=$?
        pacgo_release_jobs
        return "$status"
    fi
}

PACGO_RESERVED_JOBS=1
_pacgo_jobserver_read_fd=
_pacgo_jobserver_write_fd=
_pacgo_jobserver_owned_fd=
_pacgo_jobserver_tokens=()
_pacgo_jobserver_traps_installed=

_pacgo_chain_reservation_trap() {
    local signal="$1" default_action="${2:-}" specification quoted old_action=
    specification="$(trap -p "$signal")"
    if [[ -n "$specification" ]]; then
        quoted="${specification#trap -- }"
        quoted="${quoted% $signal}"
        eval "old_action=$quoted"
    fi
    printf -v "_pacgo_previous_trap_${signal}" %s "${old_action:-$default_action}"
    local previous="_pacgo_previous_trap_${signal}"
    local action='_pacgo_trap_status=$?; pacgo_release_jobs || :'
    action+=$'\n'"if (exit \"\$_pacgo_trap_status\"); then eval \"\${$previous}\"; else eval \"\${$previous}\"; fi"
    trap "$action" "$signal"
}

_pacgo_install_reservation_traps() {
    [[ -z "$_pacgo_jobserver_traps_installed" ]] || return 0
    _pacgo_jobserver_traps_installed=1
    _pacgo_chain_reservation_trap EXIT
    _pacgo_chain_reservation_trap ERR
    _pacgo_chain_reservation_trap INT 'trap - INT; kill -s INT "$$"'
    _pacgo_chain_reservation_trap TERM 'trap - TERM; kill -s TERM "$$"'
}

_pacgo_jobserver_auth() {
    local -a flags
    local index flag
    read -r -a flags <<<"${MAKEFLAGS:-}"
    for ((index = 0; index < ${#flags[@]}; index++)); do
        flag="${flags[index]}"
        case "$flag" in
            --jobserver-auth=*|--jobserver-fds=*)
                printf '%s\n' "${flag#*=}"
                return 0
                ;;
            --jobserver-auth|--jobserver-fds)
                if ((index + 1 < ${#flags[@]})); then
                    printf '%s\n' "${flags[index + 1]}"
                    return 0
                fi
                ;;
        esac
    done
    return 1
}

# Reserve capacity for a tool that cannot participate in GNU make's jobserver.
# The current recipe already owns one implicit slot, so only max-1 tokens are
# borrowed. Call this directly (not through command substitution), then use
# PACGO_RESERVED_JOBS. Every borrowed byte is returned on release or shell exit.
pacgo_reserve_jobs() {
    local maximum="${1:?maximum job count required}" auth token index
    [[ "$maximum" =~ ^[1-9][0-9]*$ ]] || {
        printf 'pacgo_reserve_jobs: expected a positive integer, got %q\n' "$maximum" >&2
        return 2
    }
    pacgo_release_jobs
    PACGO_RESERVED_JOBS="$maximum"
    if ! auth="$(_pacgo_jobserver_auth)"; then
        printf '%s\n' "$PACGO_RESERVED_JOBS"
        return 0
    fi

    if [[ "$auth" == fifo:* ]]; then
        local fifo="${auth#fifo:}"
        [[ -p "$fifo" ]] || {
            printf 'pacgo_reserve_jobs: jobserver FIFO is unavailable: %s\n' "$fifo" >&2
            return 2
        }
        exec {_pacgo_jobserver_owned_fd}<>"$fifo"
        _pacgo_jobserver_read_fd="$_pacgo_jobserver_owned_fd"
        _pacgo_jobserver_write_fd="$_pacgo_jobserver_owned_fd"
    elif [[ "$auth" =~ ^([0-9]+),([0-9]+)$ ]]; then
        _pacgo_jobserver_read_fd="${BASH_REMATCH[1]}"
        _pacgo_jobserver_write_fd="${BASH_REMATCH[2]}"
        [[ -e "/proc/$$/fd/$_pacgo_jobserver_read_fd" &&
           -e "/proc/$$/fd/$_pacgo_jobserver_write_fd" ]] || {
            printf 'pacgo_reserve_jobs: jobserver descriptors are unavailable: %s\n' "$auth" >&2
            _pacgo_jobserver_read_fd=
            _pacgo_jobserver_write_fd=
            return 2
        }
    else
        printf 'pacgo_reserve_jobs: unsupported jobserver authentication: %s\n' "$auth" >&2
        return 2
    fi

    _pacgo_jobserver_tokens=()
    _pacgo_install_reservation_traps
    for ((index = 1; index < maximum; index++)); do
        token=
        if ! IFS= read -r -N 1 -t 0.001 -u "$_pacgo_jobserver_read_fd" token; then
            break
        fi
        _pacgo_jobserver_tokens+=("$token")
    done
    PACGO_RESERVED_JOBS=$((1 + ${#_pacgo_jobserver_tokens[@]}))
    printf '%s\n' "$PACGO_RESERVED_JOBS"
}

pacgo_release_jobs() {
    if ((${#_pacgo_jobserver_tokens[@]} != 0)) && [[ -n "$_pacgo_jobserver_write_fd" ]]; then
        printf '%s' "${_pacgo_jobserver_tokens[@]}" >&"$_pacgo_jobserver_write_fd"
    fi
    _pacgo_jobserver_tokens=()
    if [[ -n "$_pacgo_jobserver_owned_fd" ]]; then
        exec {_pacgo_jobserver_owned_fd}>&-
    fi
    _pacgo_jobserver_owned_fd=
    _pacgo_jobserver_read_fd=
    _pacgo_jobserver_write_fd=
    PACGO_RESERVED_JOBS=1
}
