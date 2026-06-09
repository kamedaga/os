{ pkgs }:

let
  pythonEnv = pkgs.python3.withPackages (pythonPackages: [
    pythonPackages.pyyaml
    pythonPackages.rich
  ]);
  zig = pkgs.zig_0_15;
in
{
  python = pythonEnv;
  inherit zig;

  devPackages = with pkgs; [
    bash
    clang
    cmake
    dosfstools
    e2fsprogs
    e2tools
    gptfdisk
    lld
    mtools
    ninja
    pkg-config
    pythonEnv
    qemu
    ripgrep
    zig
  ];
}
