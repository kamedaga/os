#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
# Test-only SSH transport: redirect guest absolute paths into an isolated tree.
command="${!#}"
command="${command//\/srv\//$AMDGPU_TEST_REMOTE/srv/}"
command="${command//\/run\//$AMDGPU_TEST_REMOTE/run/}"
exec bash -c "$command"
