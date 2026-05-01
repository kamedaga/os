cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(bootstrap_vfs C ASM)

add_executable(bootstrap_vfs
  src/entry.S
  src/main.c
)

set_target_properties(bootstrap_vfs PROPERTIES
  OUTPUT_NAME bootstrap_vfs
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(bootstrap_vfs PRIVATE ../rootfs/fat_server/include)

target_compile_options(bootstrap_vfs PRIVATE
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

target_link_options(bootstrap_vfs PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=16
  -Wl,-z,max-page-size=16
)
