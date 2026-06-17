cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(pacha_capsule C)

add_library(pacha_capsule STATIC
  src/capsule.c
  ../libpacha/src/syscall.c
)

target_include_directories(pacha_capsule PUBLIC
  include
  ../libpacha/include
)

target_compile_options(pacha_capsule PRIVATE
  -target x86_64-freestanding-none
  -ffreestanding
  -fno-stack-protector
  -fno-plt
  -mno-red-zone
  -O2
  -Wall
  -Wextra
)
