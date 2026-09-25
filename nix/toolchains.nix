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
  linuxLlvm18 = pkgs.runCommand "capabilityos-linux-llvm-18"
    { nativeBuildInputs = [ pkgs.makeWrapper ]; } ''
    mkdir -p "$out/bin"
    makeWrapper "${pkgs.llvmPackages_18.clang-unwrapped}/bin/clang" "$out/bin/clang-18" \
      --add-flags "-resource-dir ${pkgs.llvmPackages_18.clang-unwrapped.lib}/lib/clang/18"
    ln -s "${pkgs.llvmPackages_18.lld}/bin/ld.lld" "$out/bin/ld.lld"
    ln -s "${pkgs.llvmPackages_18.lld}/bin/ld.lld" "$out/bin/ld.lld-18"
    for tool in llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readelf llvm-readobj; do
      ln -s "${pkgs.llvmPackages_18.llvm}/bin/$tool" "$out/bin/$tool-18"
    done
  '';
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
  inherit linuxLlvm18;
  inherit coqCompCertContrib;

  devPackages = with pkgs; [
    bash
    bc
    bison
    clang
    cmake
    coqVstTools
    dosfstools
    e2fsprogs
    e2tools
    elfutils
    fakeroot
    flex
    gcc
    go
    gptfdisk
    lld
    linuxLlvm18
    mtools
    ninja
    openssl
    patchelf
    perl
    pkg-config
    qemu
    ripgrep
    socat
    zig
    zstd
  ];
}
