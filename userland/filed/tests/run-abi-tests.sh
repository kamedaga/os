#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/fs-cutover-filed-tests
"${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  userland/filed/tests/abi_test.c userland/filed/src/exec_payload.c \
  userland/filed/src/exec/linux_lpr/script.c \
  -o .artifacts/fs-cutover-filed-tests/abi-test
.artifacts/fs-cutover-filed-tests/abi-test
"${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/filed/src \
  userland/filed/tests/shebang_test.c userland/filed/src/exec_payload.c \
  userland/filed/src/exec/linux_lpr/script.c \
  -o .artifacts/fs-cutover-filed-tests/shebang-test
.artifacts/fs-cutover-filed-tests/shebang-test
for test in audio_route relay identity client_retirement lease_retirement register_reply exec_self_reply; do
  sources=()
  link_flags=()
  if [[ "$test" == relay ]]; then
    link_flags+=(-Wl,--wrap=filed_pager_create)
  fi
  if [[ "$test" == identity || "$test" == client_retirement ]]; then
    sources+=(userland/filed/src/dispatch/identity.c)
  elif [[ "$test" == register_reply || "$test" == exec_self_reply ]]; then
    sources+=(userland/filed/src/exec_payload.c)
  fi
  "${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
    -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
    -Iuserland/personality/include -Iuserland/storage/include \
    "userland/filed/tests/${test}_test.c" userland/filed/src/snapshot_cache.c userland/filed/tests/pager_stub.c "${sources[@]}" \
    kobox2/protocol/src/filesystem.c kobox2/protocol/src/virtqueue.c \
    kobox2/protocol/src/protocol.c "${link_flags[@]}" \
    -o ".artifacts/fs-cutover-filed-tests/${test}-test"
  ".artifacts/fs-cutover-filed-tests/${test}-test"
done
"${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/libfiled_client/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  userland/filed/tests/client_test.c userland/libfiled_client/src/client.c \
  kobox2/protocol/src/filesystem.c -o .artifacts/fs-cutover-filed-tests/client-test
.artifacts/fs-cutover-filed-tests/client-test
"${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/personality/include -Iuserland/storage/include \
  -Iuserland/personality/linux/decoder \
  userland/filed/tests/map_rights_test.c userland/filed/src/exec/linux_lpr/elf.c \
  -o .artifacts/fs-cutover-filed-tests/map-rights-test
.artifacts/fs-cutover-filed-tests/map-rights-test
"${CC:-/usr/bin/clang}" -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -Iuserland/filed/include -Iuserland/storage/include \
  -Iuserland/libipc/include -Iuserland/libpacha/include \
  userland/filed/tests/storage_policy_test.c userland/filed/src/storage.c \
  kobox2/protocol/src/filesystem.c -o .artifacts/fs-cutover-filed-tests/storage-policy-test
.artifacts/fs-cutover-filed-tests/storage-policy-test
bash userland/filed/tests/run-storage-wait-test.sh
bash userland/filed/tests/run-storage-client-queue-test.sh
bash userland/filed/tests/run-session-detach-test.sh

bash userland/filed/tests/run-snapshot-cache-test.sh
