{
  stdenv,
  toolchain,
  llvmSource,
  targetArch,
  src,
}:

# Build LLVM libc as a no-syscall static archive for the target arch, via
# scripts/build-llvm-libc.sh. We pre-stage the llvm-project sparse checkout so
# the script skips its `git clone`.  Produces libc.a and libm.a; also verifies
# the archives contain zero syscall/svc instructions.

stdenv.mkDerivation {
  pname = "miniosv-llvm-libc";
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
    bash scripts/build-llvm-libc.sh ${targetArch}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib
    cp build/llvm-libc/${targetArch}/libc/lib/libc.a $out/lib/
    cp build/llvm-libc/${targetArch}/libc/lib/libm.a $out/lib/
    runHook postInstall
  '';
}
