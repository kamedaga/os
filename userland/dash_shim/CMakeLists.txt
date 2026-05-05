cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(dash_shim C ASM)

add_executable(dash_shim
  src/entry.S
  src/main.c
  ../exec_service/src/client.c
)

target_include_directories(dash_shim PRIVATE
  ../exec_client/include
  ../exec_service/include
)

set_target_properties(dash_shim PROPERTIES
  OUTPUT_NAME dash_shim
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_options(dash_shim PRIVATE
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

target_link_options(dash_shim PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=16
  -Wl,-z,max-page-size=16
)
