{
  stdenv,
  toolchain,
  llvmSource,
  targetArch,
  src,
}:

stdenv.mkDerivation {
  pname = "miniosv-libcxx";
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
    bash scripts/build-libcxx.sh ${targetArch}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib $out/include
    cp    build/libcxx/${targetArch}/lib/libc++.a    $out/lib/
    cp    build/libcxx/${targetArch}/lib/libc++abi.a $out/lib/
    cp    build/libcxx/${targetArch}/lib/libunwind.a $out/lib/
    # -L: dereference cmake-generated header symlinks to keep the derivation
    # self-contained (no dangling links into external/llvm-project).
    cp -rL build/libcxx/${targetArch}/include/c++    $out/include/
    runHook postInstall
  '';
}
