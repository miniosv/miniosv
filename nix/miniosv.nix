{
  stdenv,
  lib,
  toolchain,
  llvmSource,
  compilerRt,
  llvmLibc,
  libcxx,
  targetArch,
  src,
  appName,
  appSrc,
  # Extra `make` arguments the application asked for (conf_* format)
  appMakeFlags ? [ ],
}:

let
  archName = targetArch;
in
stdenv.mkDerivation {
  pname = "miniosv-${appName}";
  version = archName;

  inherit src; # kernel tree minus app/ (kernelSrc in default.nix)

  nativeBuildInputs = toolchain.buildInputs;

  dontConfigure = true;
  dontFixup = true;
  enableParallelBuilding = true;

  # Prevent stdenv's gcc/ld wrappers from leaking into the pure-LLVM link.
  CC = "";
  CXX = "";
  LD = "";
  AR = "";
  AS = "";
  NM = "";
  RANLIB = "";
  STRIP = "";
  OBJCOPY = "";
  READELF = "";

  buildPhase = ''
    runHook preBuild

    # Stage the app. rm first: no file of another app may survive into app/.
    rm -rf app && mkdir app
    cp -r ${appSrc}/. app/
    chmod -R u+w app

    mkdir -p external
    cp -r ${llvmSource} external/llvm-project
    chmod -R u+w external/llvm-project
    touch external/llvm-project/.sparse-ready

    mkdir -p build/compiler-rt/${archName}/lib
    cp ${compilerRt}/lib/libclang_rt.builtins.a \
       build/compiler-rt/${archName}/lib/

    mkdir -p build/llvm-libc/${archName}/libc/lib
    cp ${llvmLibc}/lib/libc.a ${llvmLibc}/lib/libm.a \
       build/llvm-libc/${archName}/libc/lib/

    mkdir -p build/libcxx/${archName}/lib build/libcxx/${archName}/include
    cp ${libcxx}/lib/libc++.a    build/libcxx/${archName}/lib/
    cp ${libcxx}/lib/libc++abi.a build/libcxx/${archName}/lib/
    cp ${libcxx}/lib/libunwind.a build/libcxx/${archName}/lib/
    cp -r ${libcxx}/include/c++ build/libcxx/${archName}/include/

    outDir=build/release.${archName}
    mkdir -p "$outDir"
    touch "$outDir/.compiler-rt-built"
    touch "$outDir/.llvm-libc-built"
    touch "$outDir/.libcxx-built"
    touch build/compiler-rt/${archName}/lib/libclang_rt.builtins.a
    touch build/llvm-libc/${archName}/libc/lib/*.a
    touch build/libcxx/${archName}/lib/*.a

    patchShebangs scripts

    make -j"$NIX_BUILD_CORES" arch=${archName} \
        STRIP=llvm-strip OBJCOPY=llvm-objcopy READELF=llvm-readelf${
          lib.optionalString (appMakeFlags != [ ]) " \\\n        ${lib.escapeShellArgs appMakeFlags}"
        }

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/share/miniosv $out/bin
    cp build/release.${archName}/loader.img         $out/share/miniosv/
    cp build/release.${archName}/loader.efi         $out/share/miniosv/
    cp build/release.${archName}/loader.elf         $out/share/miniosv/
    cp build/release.${archName}/loader-stripped.elf $out/share/miniosv/
    runHook postInstall
  '';

  meta = with lib; {
    description = "miniosv unikernel image with ${appName} app (${archName})";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
