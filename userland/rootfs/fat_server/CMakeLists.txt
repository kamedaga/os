cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(fat_server C ASM)

add_executable(fat_server
  src/entry.S
  src/main.c
)

target_include_directories(fat_server PRIVATE include)

set_target_properties(fat_server PROPERTIES
  OUTPUT_NAME fat_server
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_options(fat_server PRIVATE
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

target_link_options(fat_server PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)
