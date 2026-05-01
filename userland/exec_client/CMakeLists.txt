cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(exec_client C ASM)

add_executable(exec_client
  src/entry.S
  src/main.c
  ../exec_service/src/client.c
)

target_include_directories(exec_client PRIVATE
  include
  ../exec_service/include
)

set_target_properties(exec_client PROPERTIES
  OUTPUT_NAME exec_client
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_options(exec_client PRIVATE
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

target_link_options(exec_client PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=16
  -Wl,-z,max-page-size=16
)
