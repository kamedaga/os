#!/usr/bin/env bash
# Rebuild only libgallium; EGL/GL/GBM loaders remain the matching Alpine packages.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime="$(realpath "${1:?Alpine Mesa staging root required}")"
version=25.1.9
library="libgallium-${version}.so"
patch_file="${repo_root}/patches/mesa/0001-virgl-cache-render-target-sampler-view.patch"
cache="${repo_root}/.artifacts/third_party/mesa-${version}"
work="${repo_root}/.artifacts/build/mesa-${version}"
mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
jobs="${MESA_BUILD_JOBS:-8}"

[[ "$(uname -m)" == x86_64 && "${jobs}" =~ ^[1-9][0-9]*$ ]] || {
  echo "Mesa build requires an x86_64 Linux host and positive MESA_BUILD_JOBS" >&2
  exit 1
}
[[ -f "${runtime}/usr/lib/${library}" && ! -L "${runtime}/usr/lib/${library}" ]] || {
  echo "expected regular Alpine Mesa ${version} library in ${runtime}" >&2
  exit 1
}
for tool in bwrap curl tar patch sha256sum flock readelf; do
  command -v "${tool}" >/dev/null || { echo "missing Mesa build tool: ${tool}" >&2; exit 1; }
done
mkdir -p "${cache}" "${work}"
exec 9>"${work}/build.lock"
flock 9

fetch() {
  local url="$1" path="$2" checksum="$3"
  if [[ -f "${path}" ]] && echo "${checksum}  ${path}" | sha256sum -c --status; then
    return
  fi
  curl -fL --retry 3 "${url}" -o "${path}.download"
  echo "${checksum}  ${path}.download" | sha256sum -c
  mv "${path}.download" "${path}"
}

# Pin source and bootstrap independently of temporary experiment directories.
source_sha=412df33a1bb3c785ed698555a3972118a37c458e7accf6ae53f4bb87b3db454a
root_sha=4b4daa9fe2fc696c4919c4412a4c3d3e770d8fb70292a004a2c72f5096175282
fetch "https://archive.mesa3d.org/mesa-${version}.tar.xz" \
  "${cache}/mesa-${version}.tar.xz" "${source_sha}"
fetch "${mirror}/v3.22/releases/x86_64/alpine-minirootfs-3.22.5-x86_64.tar.gz" \
  "${cache}/alpine-minirootfs.tar.gz" "${root_sha}"

packages=(build-base meson ninja python3 py3-mako py3-packaging py3-ply py3-yaml
  bison flex elfutils-dev eudev-dev expat-dev libdrm-dev libx11-dev libxcb-dev
  libxdamage-dev libxext-dev libxfixes-dev libxml2-dev libxrandr-dev libxshmfence-dev
  libxxf86vm-dev llvm20-dev clang20-dev wayland-dev wayland-protocols xorgproto
  zlib-dev zstd-dev libva-dev libvdpau-dev libclc-dev spirv-llvm-translator-dev
  glslang-dev vulkan-loader-dev)
toolchain_key="$(printf '%s\n' "${root_sha}" "${mirror}" "${packages[@]}" | sha256sum | cut -c1-16)"
toolchain="${work}/toolchain-${toolchain_key}"
if [[ ! -f "${toolchain}/.ready" ]]; then
  # This is a separate host build environment, never a copy of the guest rootfs.
  mkdir -p "${toolchain}"
  tar -xzf "${cache}/alpine-minirootfs.tar.gz" -C "${toolchain}"
  bwrap --unshare-user --uid 0 --gid 0 --bind "${toolchain}" / \
    --dev /dev --proc /proc --tmpfs /tmp \
    --ro-bind /etc/resolv.conf /etc/resolv.conf \
    --ro-bind /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem \
    /sbin/apk --no-cache --no-progress \
      --repository "${mirror}/v3.22/main" --repository "${mirror}/v3.22/community" \
      add "${packages[@]}"
  touch "${toolchain}/.ready"
fi
mkdir -p "${toolchain}/source" "${toolchain}/build"

# Changing the patch, recipe or installed toolchain invalidates the build cache.
key="$(sha256sum "${patch_file}" "${BASH_SOURCE[0]}" "${toolchain}/lib/apk/db/installed" |
  sha256sum | cut -c1-32)"
build_root="${work}/${key}"
source_dir="${build_root}/source"
build_dir="${build_root}/build"
built_library="${build_dir}/src/gallium/targets/dri/${library}"
mkdir -p "${source_dir}" "${build_dir}"
if [[ ! -f "${build_root}/source-prepared" ]]; then
  tar -xJf "${cache}/mesa-${version}.tar.xz" --strip-components=1 -C "${source_dir}"
  [[ "$(<"${source_dir}/VERSION")" == "${version}" ]]
  patch --dry-run --fuzz=0 --forward -p1 -d "${source_dir}" -i "${patch_file}"
  patch --fuzz=0 --forward -p1 -d "${source_dir}" -i "${patch_file}"
  touch "${build_root}/source-prepared"
fi

if [[ ! -f "${build_root}/library.sha256" ]] ||
   ! sha256sum -c --status "${build_root}/library.sha256"; then
  # Upstream source is read-only during compilation; outputs go to /build.
  bwrap --unshare-user --uid 0 --gid 0 --ro-bind "${toolchain}" / \
    --ro-bind "${source_dir}" /source --bind "${build_dir}" /build \
    --dev /dev --proc /proc --tmpfs /tmp --chdir /build \
    --clearenv --setenv PATH /usr/lib/llvm20/bin:/usr/bin:/bin \
    --setenv LC_ALL C /bin/sh -eu -c '
      if [ ! -f build.ninja ]; then
        meson setup /build /source \
          --prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
          -Db_ndebug=true -Db_lto=false -Dauto_features=disabled \
          -Dallow-kcmp=enabled -Dexpat=enabled -Dshader-cache=enabled \
          -Dxlib-lease=enabled -Dxmlconfig=enabled -Dzstd=enabled -Dzlib=enabled \
          -Dbackend_max_links=2 -Ddri-drivers-path=/usr/lib/dri \
          -Dgallium-drivers=r300,r600,radeonsi,nouveau,llvmpipe,virgl,zink,svga,i915,iris,crocus \
          -Dvulkan-drivers=[] -Dplatforms=x11,wayland \
          -Dllvm=enabled -Dshared-llvm=enabled -Dgbm=enabled -Dglx=dri \
          -Dopengl=true -Dosmesa=false -Dgles1=enabled -Dgles2=enabled -Degl=enabled \
          -Dgallium-extra-hud=true -Dgallium-nine=true -Dgallium-xa=enabled \
          -Dgallium-va=enabled -Dgallium-vdpau=enabled -Dvideo-codecs=all
      fi
      ninja -C /build -j "$1" "src/gallium/targets/dri/$2"
    ' sh "${jobs}" "${library}"
  sha256sum "${built_library}" >"${build_root}/library.sha256"
else
  echo "using verified Mesa build cache: ${key}"
fi

readelf -d "${built_library}" | grep -Fq "[${library}]"
install -m 0755 "${built_library}" "${runtime}/usr/lib/${library}"
mkdir -p "${runtime}/usr/share/pacha"
{
  printf 'Mesa %s; VirGL render-target/sampler-view cache eligibility only\n' "${version}"
  printf 'source-sha256 %s\nbuild-key %s\n' "${source_sha}" "${key}"
  sha256sum "${patch_file}" "${built_library}"
} >"${runtime}/usr/share/pacha/mesa-virgl-build.txt"
echo "installed patched ${library} into ${runtime}"
