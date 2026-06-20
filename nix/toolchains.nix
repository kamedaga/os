{ pkgs }:

let
  zig = pkgs.stdenvNoCC.mkDerivation {
    pname = "zig";
    version = "0.16.0";
    src = pkgs.fetchurl {
      url = "https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz";
      sha256 = "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00";
    };
    installPhase = ''
      runHook preInstall
      mkdir -p $out
      cp -R . $out/
      mkdir -p $out/bin
      ln -s ../zig $out/bin/zig
      runHook postInstall
    '';
  };
  clangFreestanding = pkgs.llvmPackages.clang-unwrapped;
  coqCompCert = pkgs.coqPackages.compcert;
  coqCompCertContrib =
    "${coqCompCert.lib}/lib/coq/${pkgs.coq.coq-version}/user-contrib";
  coqWithStdlib = pkgs.coq.withPackages (ps: [
    ps.compcert
    ps.stdlib
    ps.VST
  ]);
  coqVstTools = pkgs.runCommand "coq-vst-tools"
    {
      nativeBuildInputs = [ pkgs.makeWrapper ];
    }
    ''
      mkdir -p "$out/bin"
      for tool in "${coqWithStdlib}"/bin/*; do
        ln -s "$tool" "$out/bin/$(basename "$tool")"
      done
      for tool in coqc coqtop coqdep coq_makefile clightgen; do
        if [ -e "${coqWithStdlib}/bin/$tool" ]; then
          rm -f "$out/bin/$tool"
          makeWrapper "${coqWithStdlib}/bin/$tool" "$out/bin/$tool" \
            --set ROCQPATH "${coqCompCertContrib}"
        fi
      done
    '';
in
{
  inherit zig;
  inherit clangFreestanding;
  inherit coqCompCertContrib;

  devPackages = with pkgs; [
    bash
    clang
    cmake
    coqVstTools
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
