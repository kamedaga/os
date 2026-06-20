{
  description = "CapabilityOS WSL Linux build and packaging environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f {
            pkgs = import nixpkgs {
              inherit system;
              config.allowUnfreePredicate =
                pkg:
                builtins.elem (nixpkgs.lib.getName pkg) [
                  "coq9.1-compcert"
                  "compcert"
                ];
            };
          }
        );
    in
    {
      devShells = forAllSystems (
        { pkgs }:
        let
          toolchains = import ./nix/toolchains.nix { inherit pkgs; };
        in
        {
          default = pkgs.mkShell {
            packages = toolchains.devPackages;
            CAPOS_UNWRAPPED_CLANG = "${toolchains.clangFreestanding}/bin/clang";
            CAPOS_FREESTANDING_CC = "${toolchains.clangFreestanding}/bin/clang";
            CAPOS_QEMU = "${pkgs.qemu}/bin/qemu-system-x86_64";
            CAPOS_QEMU_IMG = "${pkgs.qemu}/bin/qemu-img";
            CAPOS_OVMF_CODE = "${pkgs.OVMF.fd}/FV/OVMF_CODE.fd";
            CAPOS_OVMF_VARS_TEMPLATE = "${pkgs.OVMF.fd}/FV/OVMF_VARS.fd";
            PACGO_NIX_DEVELOP = "1";
            shellHook = ''
              export ROCQPATH="${toolchains.coqCompCertContrib}''${ROCQPATH:+:''${ROCQPATH}}"
              if [ -n "''${PS1-}" ]; then
                echo "CapabilityOS dev shell"
                echo "  runner: ./pacgo"
                echo "  qemu:   $CAPOS_QEMU"
              fi
            '';
          };
        }
      );
    };
}
