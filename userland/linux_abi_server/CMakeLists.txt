cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(linux_abi_server C ASM)

set(LINUX_ABI_SERVER_INCLUDED_SOURCES
  src/common.inc.c
  src/runtime.inc.c
  src/vfs.inc.c
  src/console.inc.c
  src/process_state.inc.c
  src/execve.inc.c
  src/fs_syscalls.inc.c
  src/fd_syscalls.inc.c
  src/process_syscalls.inc.c
  src/misc_syscalls.inc.c
  src/vm_syscalls.inc.c
  src/main_loop.inc.c
)

set_source_files_properties(${LINUX_ABI_SERVER_INCLUDED_SOURCES}
  PROPERTIES HEADER_FILE_ONLY TRUE
)

add_executable(linux_abi_server
  src/entry.S
  src/main.c
  ${LINUX_ABI_SERVER_INCLUDED_SOURCES}
)

set_target_properties(linux_abi_server PROPERTIES
  OUTPUT_NAME linux_abi_server
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_options(linux_abi_server PRIVATE
  -target x86_64-freestanding-none
  -ffreestanding
  -fno-stack-protector
  -fno-plt
  -fPIE
  -mno-red-zone
  -O2
  -Wall
  -Wextra
)

target_link_options(linux_abi_server PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)
