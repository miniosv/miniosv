{
  pkgs,
  llvmPkgs,
  system,
}:

# A single pure-LLVM toolchain (clang/clang++, lld, llvm binutils) that can
# cross-compile to either x86_64 or aarch64 baremetal from either host arch,
# driven only by `--target=<triple>` — no per-arch GNU cross toolchain.
#
# nixpkgs splits clang into clang-unwrapped (binary) + clang-unwrapped.lib
# (resource-dir headers) + compiler-rt (builtins). clang 20 expects all three
# under a single -resource-dir root, so we merge them here. We only need the
# host arch's builtins in the resource dir — the target arch's (when cross)
# comes from the per-arch compiler-rt derivation.

let
  rtArch = if system == "x86_64-linux" then "x86_64" else "aarch64";
  rtTriple = "${rtArch}-unknown-linux-gnu";

  resourceDir = pkgs.runCommand "clang-20-resource-dir" { } ''
    mkdir -p $out/include
    cp -r ${llvmPkgs.clang-unwrapped.lib}/lib/clang/20/include/. $out/include/

    mkdir -p $out/lib/${rtTriple}
    rt="${llvmPkgs.compiler-rt}"
    if [ -f "$rt/lib/${rtTriple}/libclang_rt.builtins.a" ]; then
      ln -s "$rt/lib/${rtTriple}/libclang_rt.builtins.a" \
            "$out/lib/${rtTriple}/libclang_rt.builtins.a"
    elif [ -f "$rt/lib/linux/libclang_rt.builtins-${rtArch}.a" ]; then
      ln -s "$rt/lib/linux/libclang_rt.builtins-${rtArch}.a" \
            "$out/lib/${rtTriple}/libclang_rt.builtins.a"
    fi
  '';

  clang = pkgs.writeShellScriptBin "clang" ''
    exec ${llvmPkgs.clang-unwrapped}/bin/clang   -resource-dir ${resourceDir} "$@"
  '';
  clangPP = pkgs.writeShellScriptBin "clang++" ''
    exec ${llvmPkgs.clang-unwrapped}/bin/clang++ -resource-dir ${resourceDir} "$@"
  '';
in
{
  inherit clang clangPP resourceDir;

  # Everything a runtime sub-build or the kernel Makefile needs to compile /
  # link. Callers add `qemu`, `gdb`, python + boto3, etc. on top.
  buildInputs = with pkgs; [
    clang
    clangPP
    llvmPkgs.llvm
    llvmPkgs.lld
    binutils
    cmake
    ninja
    git
    ctags
    mtools
    dosfstools
    gptfdisk
    (python3.withPackages (ps: [ ps.pyyaml ]))
  ];
}
