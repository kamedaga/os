cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(seed2_root C ASM)

add_executable(seed2_root
  src/entry.S
  ../exec/src/elf.c
  src/main.c
)

set_target_properties(seed2_root PROPERTIES
  OUTPUT_NAME seed2_root
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(seed2_root PRIVATE ../rootfs/fat_server/include ../exec/include)

target_compile_options(seed2_root PRIVATE
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

target_link_options(seed2_root PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)
