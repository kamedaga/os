{ pkgs, toolchains }:

let
  python = toolchains.python;
  ovmfCode = "${pkgs.OVMF.fd}/FV/OVMF_CODE.fd";
  ovmfVars = "${pkgs.OVMF.fd}/FV/OVMF_VARS.fd";

  capos = pkgs.writeShellApplication {
    name = "capos";
    runtimeInputs = toolchains.devPackages;
    text = ''
      root="$PWD"
      while [ "$root" != "/" ]; do
        if [ -f "$root/pack/capos.py" ] && [ -f "$root/pack/pack.yaml" ]; then
          break
        fi
        root="$(dirname "$root")"
      done
      if [ ! -f "$root/pack/capos.py" ]; then
        echo "capos: could not find pack/capos.py from $PWD" >&2
        exit 2
      fi
      export CAPOS_QEMU="${pkgs.qemu}/bin/qemu-system-x86_64"
      export CAPOS_QEMU_IMG="${pkgs.qemu}/bin/qemu-img"
      export CAPOS_OVMF_CODE="${ovmfCode}"
      export CAPOS_OVMF_VARS_TEMPLATE="${ovmfVars}"
      exec ${python}/bin/python "$root/pack/capos.py" "$@"
    '';
  };

  app =
    name: command:
    {
      type = "app";
      program = "${pkgs.writeShellApplication {
        name = "capos-${name}";
        runtimeInputs = toolchains.devPackages;
        text = ''
          exec ${capos}/bin/capos ${command} "$@"
        '';
      }}/bin/capos-${name}";
    };
in
{
  inherit capos;

  apps = {
    capos = {
      type = "app";
      program = "${capos}/bin/capos";
    };
    default = {
      type = "app";
      program = "${capos}/bin/capos";
    };
    plan = app "plan" "plan";
    "build-kernel" = app "build-kernel" "build kernel";
    "build-userland" = app "build-userland" "build userland";
    "gen-manifests" = app "gen-manifests" "gen manifests";
    image = app "image" "image";
    "sync-bootfs" = app "sync-bootfs" "sync bootfs";
    "sync-rootfs" = app "sync-rootfs" "sync rootfs";
    qemu = app "qemu" "qemu";
    test = app "test" "test";
    all = app "all" "all";
    ci = app "ci" "ci";
  };
}
