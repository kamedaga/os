#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p .artifacts/filed-tests
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -Iuserland/filed/include -Iuserland/libpacha/include -Iuserland/libipc/include \
 -Iuserland/personality/include -Iuserland/storage/include -Ikobox2/protocol/include \
 -Ikobox2/protocol/generated/include userland/filed/tests/exec_stage_report_test.c -o .artifacts/filed-tests/exec-stage-report
.artifacts/filed-tests/exec-stage-report
