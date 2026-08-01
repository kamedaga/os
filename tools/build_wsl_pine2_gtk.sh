#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_dir="${repo_root}/third_party/pine2-gtk"
out="${1:-.artifacts/userland-fixtures/pine2-gtk-root}"
case "${out}" in
  /*) out_abs="$(realpath -m -- "${out}")" ;;
  *) out_abs="$(realpath -m -- "${repo_root}/${out}")" ;;
esac
case "${out_abs}" in
  "${repo_root}/.artifacts/"*) ;;
  *)
    echo "pine2-gtk output must stay below ${repo_root}/.artifacts: ${out_abs}" >&2
    exit 1
    ;;
esac

dockerfile="${project_dir}/tools/Dockerfile.musl"
build_script="${project_dir}/tools/build-musl.sh"
binary="${project_dir}/build-musl/pine2-gtk"
desktop="${project_dir}/data/app.pine2.Pine2.desktop"
for required in "${dockerfile}" "${build_script}" "${desktop}"; do
  [[ -f "${required}" ]] || { echo "missing Pine2 GTK input: ${required}" >&2; exit 1; }
done
for command in curl docker readelf sha256sum tar; do
  command -v "${command}" >/dev/null 2>&1 || { echo "missing host tool: ${command}" >&2; exit 1; }
done

docker_arg() {
  local name="$1"
  sed -n "s/^ARG ${name}=//p" "${dockerfile}" | tail -n 1
}
cmark_version="$(docker_arg CMARK_GFM_VERSION)"
cmark_sha256="$(docker_arg CMARK_GFM_SHA256)"
[[ "${cmark_version}" =~ ^[0-9A-Za-z._-]+$ ]] || {
  echo "Dockerfile.musl has an invalid CMARK_GFM_VERSION: ${cmark_version:-missing}" >&2
  exit 1
}
[[ "${cmark_sha256}" =~ ^[0-9a-f]{64}$ ]] || {
  echo "Dockerfile.musl has an invalid CMARK_GFM_SHA256: ${cmark_sha256:-missing}" >&2
  exit 1
}

# Use the application's own musl build entry point. It fixes the container
# distribution and validates that the resulting executable uses musl.
bash "${build_script}"
[[ -x "${binary}" ]] || { echo "Pine2 GTK musl build did not produce ${binary}" >&2; exit 1; }

interpreter="$(readelf -lW "${binary}" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')"
if [[ "${interpreter}" != "/lib/ld-musl-x86_64.so.1" ]]; then
  echo "unexpected Pine2 GTK ELF interpreter: ${interpreter:-missing}" >&2
  exit 1
fi
dynamic="$(readelf -dW "${binary}")"
for library in libgtk-3.so.0 libcurl.so.4 libjson-c.so.5; do
  grep -Fq "Shared library: [${library}]" <<<"${dynamic}" || {
    echo "Pine2 GTK is missing required DT_NEEDED ${library}" >&2
    exit 1
  }
done
if grep -Fq 'Shared library: [libcmark' <<<"${dynamic}"; then
  echo "cmark-gfm must remain statically linked into Pine2 GTK" >&2
  exit 1
fi
if ! grep -Eq '\((RPATH|RUNPATH)\).*\[/usr/lib:/opt/curl/lib\]' <<<"${dynamic}"; then
  echo "Pine2 GTK is missing the scoped /usr/lib:/opt/curl/lib runtime search path" >&2
  exit 1
fi

cache="${repo_root}/.artifacts/third_party/pine2-gtk"
mkdir -p "${cache}"
tmp="$(mktemp -d "${cache}/assemble.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT
root="${tmp}/root"
mkdir -p \
  "${root}/usr/bin" \
  "${root}/usr/share/applications" \
  "${root}/usr/share/licenses/pine2-gtk"

install -m 0755 "${binary}" "${root}/usr/bin/pine2-gtk"
install -m 0644 "${desktop}" "${root}/usr/share/applications/app.pine2.Pine2.desktop"

# cmark-gfm is statically linked, so its notice cannot be supplied by a
# separate runtime package. Fetch the exact source archive pinned by the
# application's Dockerfile and verify it before copying its license.
cmark_archive="${tmp}/cmark-gfm-${cmark_version}.tar.gz"
curl --fail --location --proto '=https' --tlsv1.2 --retry 3 \
  "https://github.com/github/cmark-gfm/archive/refs/tags/${cmark_version}.tar.gz" \
  -o "${cmark_archive}"
printf '%s  %s\n' "${cmark_sha256}" "${cmark_archive}" | sha256sum -c - >/dev/null
cmark_copying="cmark-gfm-${cmark_version}/COPYING"
tar -tzf "${cmark_archive}" | grep -Fx "${cmark_copying}" >/dev/null || {
  echo "verified cmark-gfm archive does not contain ${cmark_copying}" >&2
  exit 1
}
tar -xOzf "${cmark_archive}" "${cmark_copying}" \
  >"${root}/usr/share/licenses/pine2-gtk/cmark-gfm-COPYING"
[[ -s "${root}/usr/share/licenses/pine2-gtk/cmark-gfm-COPYING" ]] || {
  echo "cmark-gfm license extraction produced an empty file" >&2
  exit 1
}

# Do not infer a license for the application. If one is added to the imported
# source later, include that exact file automatically and make it a pack input
# through the project directory entries in pack.yaml.
for candidate in LICENSE LICENSE.md LICENSE.txt COPYING COPYING.md COPYING.txt; do
  if [[ -f "${project_dir}/${candidate}" ]]; then
    install -m 0644 "${project_dir}/${candidate}" \
      "${root}/usr/share/licenses/pine2-gtk/pine2-gtk-${candidate}"
  fi
done

grep -Fxq 'Exec=pine2-gtk' "${root}/usr/share/applications/app.pine2.Pine2.desktop" || {
  echo "Pine2 desktop entry must launch the packaged executable" >&2
  exit 1
}
[[ -x "${root}/usr/bin/pine2-gtk" ]] || {
  echo "Pine2 GTK runtime overlay is incomplete" >&2
  exit 1
}
[[ "$(od -An -tx1 -N4 "${root}/usr/bin/pine2-gtk" | tr -d ' \n')" == "7f454c46" ]] || {
  echo "Pine2 GTK entry point is not an ELF executable" >&2
  exit 1
}

rm -rf "${out_abs}.tmp" "${out_abs}"
mkdir -p "$(dirname "${out_abs}")"
mv "${root}" "${out_abs}.tmp"
mv "${out_abs}.tmp" "${out_abs}"
printf 'built Pine2 GTK runtime overlay into %s\n' "${out_abs}"
printf 'interpreter: %s\n' "${interpreter}"
printf 'verified DT_NEEDED: libgtk-3.so.0 libcurl.so.4 libjson-c.so.5\n'
printf 'runtime search path: /usr/lib:/opt/curl/lib\n'
