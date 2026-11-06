{
  stdenv,
  runCommand,
  toolchain,
  llvmSource,
  targetArch,
  src,
  isCross,
}:

# Native (target == host): repackage the clang resource-dir's builtins.
# Cross: rebuild from the pinned llvm-project via scripts/build-compiler-rt.sh.

let
  llvmTriple =
    if targetArch == "x64" then "x86_64-unknown-linux-gnu" else "aarch64-unknown-linux-gnu";
in
if !isCross then
  runCommand "miniosv-compiler-rt-22.1.7-${targetArch}" { } ''
    mkdir -p $out/lib
    cp ${toolchain.resourceDir}/lib/${llvmTriple}/libclang_rt.builtins.a $out/lib/
  ''
else
  stdenv.mkDerivation {
    pname = "miniosv-compiler-rt";
    version = "22.1.7-${targetArch}";

    inherit src;

    nativeBuildInputs = toolchain.buildInputs;

    dontConfigure = true;
    dontFixup = true;
    enableParallelBuilding = true;

    buildPhase = ''
      runHook preBuild
      mkdir -p external
      cp -r ${llvmSource} external/llvm-project
      chmod -R u+w external/llvm-project

      patchShebangs scripts
      bash scripts/build-compiler-rt.sh ${targetArch}
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p $out/lib
      cp build/compiler-rt/${targetArch}/lib/libclang_rt.builtins.a $out/lib/
      runHook postInstall
    '';
  }
