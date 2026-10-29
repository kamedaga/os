# SPDX-License-Identifier: MIT

# Cross toolchain for PachaOS native ELFs: freestanding x86_64 code that
# links statically against the PachaOS musl (see pachaos_native.cmake).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
# There is no libc to link a test program against before the project is
# configured, so compiler checks build static libraries instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The Nix shell's clang wrapper injects host glibc headers and hardening
# flags; CAPOS_FREESTANDING_CC names the bare compiler, as build-libc.sh uses.
if(DEFINED ENV{CAPOS_FREESTANDING_CC})
  set(_pachaos_cc "$ENV{CAPOS_FREESTANDING_CC}")
else()
  set(_pachaos_cc clang)
endif()
if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${_pachaos_cc}")
endif()
if(NOT CMAKE_ASM_COMPILER)
  set(CMAKE_ASM_COMPILER "${_pachaos_cc}")
endif()
unset(_pachaos_cc)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-musl)
set(CMAKE_ASM_COMPILER_TARGET x86_64-linux-musl)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
