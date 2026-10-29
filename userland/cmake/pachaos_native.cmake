# SPDX-License-Identifier: MIT

# PachaOS native ELFs as ordinary CMake executables. Every target compiles
# against and statically links one of the libc builds made by
# musl/pachaos/build/build-libc.sh (the musl_libc pacgo recipes), so Ninja
# recompiles only the translation units whose inputs changed and relinks.
# Include it after project(), configured with pachaos-native.toolchain.cmake.
include_guard(GLOBAL)

get_filename_component(PACHAOS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PACHAOS_MUSL_LIBC_ROOT "${PACHAOS_REPO_ROOT}/.artifacts/musl-pachaos/libc"
  CACHE PATH "Directory holding the libc builds made by build-libc.sh")

# Only the flags below reach native ELFs: host CFLAGS/LDFLAGS and the
# per-configuration defaults (such as -O3 -DNDEBUG for Release) must not.
foreach(_pachaos_lang IN ITEMS C ASM)
  set(CMAKE_${_pachaos_lang}_FLAGS "")
  foreach(_pachaos_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_${_pachaos_lang}_FLAGS_${_pachaos_config} "")
  endforeach()
endforeach()
set(CMAKE_EXE_LINKER_FLAGS "")
foreach(_pachaos_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_EXE_LINKER_FLAGS_${_pachaos_config} "")
endforeach()
unset(_pachaos_lang)
unset(_pachaos_config)

# pachaos_native_executable(<target>
#   SOURCES <file>...              main.c first; objects link in this order
#   [LIBC default|oz]              libc build, from musl_libc / musl_libc_oz
#   [OUTPUT_NAME <name>]           <name>.elf in CMAKE_BINARY_DIR (default <target>)
#   [INCLUDE_DIRECTORIES <dir>...]
#   [COMPILE_OPTIONS <flag>...]    after the common flags, so they override them
#   [LINK_OPTIONS <flag>...]
#   [LINK_INPUTS <archive>...])    resolved together with libc.a
function(pachaos_native_executable target)
  cmake_parse_arguments(PARSE_ARGV 1 arg ""
    "LIBC;OUTPUT_NAME"
    "SOURCES;INCLUDE_DIRECTORIES;COMPILE_OPTIONS;LINK_OPTIONS;LINK_INPUTS")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "pachaos_native_executable: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT arg_SOURCES)
    message(FATAL_ERROR "pachaos_native_executable: ${target} has no SOURCES")
  endif()
  if(NOT arg_LIBC)
    set(arg_LIBC default)
  endif()
  if(arg_LIBC STREQUAL "default")
    set(libc_recipe musl_libc)
  elseif(arg_LIBC STREQUAL "oz")
    set(libc_recipe musl_libc_oz)
  else()
    message(FATAL_ERROR "pachaos_native_executable: unknown LIBC ${arg_LIBC}")
  endif()
  if(NOT arg_OUTPUT_NAME)
    set(arg_OUTPUT_NAME "${target}")
  endif()

  set(libc_dir "${PACHAOS_MUSL_LIBC_ROOT}/${arg_LIBC}")
  if(NOT EXISTS "${libc_dir}/config")
    message(FATAL_ERROR
      "${libc_dir} is not a built libc; build the pacgo recipe ${libc_recipe} first")
  endif()
  set(sysroot "${libc_dir}/sysroot")
  set(crt_start "${sysroot}/usr/lib/rcrt1.o")
  set(crti "${sysroot}/usr/lib/crti.o")
  set(crtn "${sysroot}/usr/lib/crtn.o")
  set(libc "${sysroot}/usr/lib/libc.a")

  add_executable(${target} ${arg_SOURCES})
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "${arg_OUTPUT_NAME}"
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LINK_DEPENDS "${crt_start};${crti};${crtn};${libc};${arg_LINK_INPUTS}")
  target_include_directories(${target} PRIVATE ${arg_INCLUDE_DIRECTORIES})
  target_compile_options(${target} PRIVATE
    "--sysroot=${sysroot}"
    -std=c99
    -fno-stack-protector
    -fno-plt
    -mno-red-zone
    -O2
    -Wall
    -Wextra
    -D_XOPEN_SOURCE=700
    -fPIE
    ${arg_COMPILE_OPTIONS})
  # Static PIE: the crt start files precede the objects, the archives are
  # resolved as one group, and crtn closes the init/fini sections.
  target_link_options(${target} PRIVATE
    "--sysroot=${sysroot}"
    -nostdlib
    -nostartfiles
    -static
    -fuse-ld=lld
    -fno-stack-protector
    -fno-plt
    -mno-red-zone
    -Wl,-e,_start
    -Wl,--no-dynamic-linker
    -Wl,-z,common-page-size=4096
    -Wl,-z,max-page-size=4096
    -fPIE
    -Wl,-pie
    ${arg_LINK_OPTIONS}
    "${crt_start}"
    "${crti}")
  target_link_libraries(${target} PRIVATE
    -Wl,--start-group
    ${arg_LINK_INPUTS}
    "${libc}"
    -Wl,--end-group
    "${crtn}")
endfunction()
