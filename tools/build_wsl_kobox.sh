#!/usr/bin/env bash
set -euo pipefail

repo_url=${KOBOX_REPO_URL:-https://github.com/kamedaga/kobox.git}
repo_ref=${KOBOX_REF:-main}
source_dir=${KOBOX_SOURCE_DIR:-}
src_dir=.artifacts/src/kobox
build_dir=.artifacts/cmake/kobox
out_dir=.artifacts/userland-fixtures/kobox
tool_dir=.artifacts/tools
cc_wrapper=$tool_dir/kobox-musl-clang
uapi_dir=$tool_dir/kobox-linux-uapi

mkdir -p .artifacts/src "$out_dir" "$tool_dir"

if [ -z "$source_dir" ] && [ -e "$src_dir" ] && [ ! -d "$src_dir/.git" ] && [ -f _kobox/CMakeLists.txt ]; then
  source_dir=$PWD/_kobox
fi

if [ -n "$source_dir" ]; then
  rm -rf "$src_dir"
  mkdir -p "$src_dir"
  (cd "$source_dir" && tar --exclude=.git --exclude=.artifacts -cf - .) | (cd "$src_dir" && tar -xf -)
elif [ ! -d "$src_dir/.git" ]; then
  if [ -e "$src_dir" ]; then
    echo "$src_dir exists but is not a git checkout" >&2
    exit 1
  fi
  git clone "$repo_url" "$src_dir"
fi

if [ -z "$source_dir" ]; then
  git -C "$src_dir" remote set-url origin "$repo_url"
  git -C "$src_dir" fetch --depth 1 origin "$repo_ref"
  git -C "$src_dir" checkout --force --detach FETCH_HEAD
  git -C "$src_dir" clean -fdx
  git -C "$src_dir" rev-parse HEAD > "$out_dir/kobox.commit"
else
  printf 'local:%s\n' "$source_dir" > "$out_dir/kobox.commit"
fi

if ! command -v clang >/dev/null 2>&1; then
  echo "missing clang in WSL" >&2
  exit 1
fi
if [ ! -d /usr/include/x86_64-linux-musl ] || [ ! -d /usr/lib/x86_64-linux-musl ]; then
  echo "missing musl development files: install musl-tools in WSL" >&2
  exit 1
fi

mkdir -p "$uapi_dir"
ln -sfn /usr/include/linux "$uapi_dir/linux"
ln -sfn /usr/include/x86_64-linux-gnu/asm "$uapi_dir/asm"
ln -sfn /usr/include/asm-generic "$uapi_dir/asm-generic"
uapi_abs=$PWD/$uapi_dir

cat > "$cc_wrapper" <<EOF
#!/usr/bin/env bash
set -euo pipefail
resource_dir=\$(clang -print-resource-dir)
exec clang \
  --target=x86_64-linux-musl \
  -nostdinc \
  -isystem /usr/include/x86_64-linux-musl \
  -isystem "$uapi_abs" \
  -isystem "\$resource_dir/include" \
  -B /usr/lib/x86_64-linux-musl \
  "\$@"
EOF
chmod +x "$cc_wrapper"

cmake \
  -S "$src_dir" \
  -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$PWD/$cc_wrapper" \
  -DCMAKE_EXE_LINKER_FLAGS="-B /usr/lib/x86_64-linux-musl -L /usr/lib/x86_64-linux-musl -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-rpath,/lib -Wl,-z,now -Wl,-z,relro"

cmake --build "$build_dir" --target \
  kobox-run \
  kobox-inspect \
  kobox-ls-devices

cp "$build_dir/kobox-run" "$out_dir/kobox-run.elf"
cp "$build_dir/kobox-inspect" "$out_dir/kobox-inspect.elf"
cp "$build_dir/kobox-ls-devices" "$out_dir/kobox-ls-devices.elf"
