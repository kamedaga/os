cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(esp_server C ASM)

add_executable(esp_server
  src/entry.S
  src/main.c
  src/block_client.c
  src/fat.c
)

add_executable(esp_server_smoke
  src/smoke_entry.S
  src/smoke.c
)

target_include_directories(esp_server PRIVATE include)
target_include_directories(esp_server_smoke PRIVATE include)

set_target_properties(esp_server PROPERTIES
  OUTPUT_NAME esp_server
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

set_target_properties(esp_server_smoke PROPERTIES
  OUTPUT_NAME esp_server_smoke
  SUFFIX .elf
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_options(esp_server PRIVATE
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

target_compile_options(esp_server_smoke PRIVATE
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

target_link_options(esp_server PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)

target_link_options(esp_server_smoke PRIVATE
  -target x86_64-freestanding-none
  -fuse-ld=lld
  -nostdlib
  -Wl,-e,_start
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)
