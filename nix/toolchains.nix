{ pkgs }:

let
  zig = pkgs.zig_0_15;
  clangFreestanding = pkgs.llvmPackages.clang-unwrapped;
in
{
  inherit zig;
  inherit clangFreestanding;

  devPackages = with pkgs; [
    bash
    clang
    cmake
    dosfstools
    e2fsprogs
    e2tools
    fakeroot
    gcc
    go
    gptfdisk
    lld
    mtools
    ninja
    patchelf
    pkg-config
    qemu
    ripgrep
    socat
    zig
  ];
}
