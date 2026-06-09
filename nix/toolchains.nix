{ pkgs }:

let
  zig = pkgs.zig_0_15;
in
{
  inherit zig;

  devPackages = with pkgs; [
    bash
    clang
    cmake
    dosfstools
    e2fsprogs
    e2tools
    go
    gptfdisk
    lld
    mtools
    ninja
    pkg-config
    qemu
    ripgrep
    zig
  ];
}
