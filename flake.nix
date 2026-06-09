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
            };
          }
        );
    in
    {
      packages = forAllSystems (
        { pkgs }:
        let
          toolchains = import ./nix/toolchains.nix { inherit pkgs; };
          runners = import ./nix/runners.nix {
            inherit pkgs toolchains;
          };
        in
        {
          capos = runners.capos;
          default = runners.capos;
        }
      );

      apps = forAllSystems (
        { pkgs }:
        let
          toolchains = import ./nix/toolchains.nix { inherit pkgs; };
          runners = import ./nix/runners.nix {
            inherit pkgs toolchains;
          };
        in
        runners.apps
      );

      devShells = forAllSystems (
        { pkgs }:
        let
          toolchains = import ./nix/toolchains.nix { inherit pkgs; };
          runners = import ./nix/runners.nix {
            inherit pkgs toolchains;
          };
        in
        {
          default = pkgs.mkShell {
            packages = toolchains.devPackages ++ [
              runners.capos
            ];
            CAPOS_QEMU = "${pkgs.qemu}/bin/qemu-system-x86_64";
            CAPOS_QEMU_IMG = "${pkgs.qemu}/bin/qemu-img";
            CAPOS_OVMF_CODE = "${pkgs.OVMF.fd}/FV/OVMF_CODE.fd";
            CAPOS_OVMF_VARS_TEMPLATE = "${pkgs.OVMF.fd}/FV/OVMF_VARS.fd";
            shellHook = ''
              echo "CapabilityOS dev shell"
              echo "  runner: capos"
              echo "  qemu:   $CAPOS_QEMU"
            '';
          };
        }
      );
    };
}
