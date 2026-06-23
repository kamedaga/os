cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(seed2_boot C ASM)

add_executable(seed2_boot
  src/entry.S
  src/main.c
  ../exec/src/elf.c
)

set_target_properties(seed2_boot PROPERTIES
  OUTPUT_NAME seed2_boot
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(seed2_boot PRIVATE ../exec/include)

target_compile_options(seed2_boot PRIVATE
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

target_link_options(seed2_boot PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)
