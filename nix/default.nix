{
  nixpkgs,
  system,
  self,
  apps,
}:

let
  pkgs = nixpkgs.legacyPackages.${system};
  llvmPkgs = pkgs.llvmPackages_20;
  inherit (nixpkgs) lib;

  # A source is the cwd sentinel if it still carries the marker file. In
  # that case we swap in a derivation that fails to build with usage
  # instructions, so eval (nix flake check) stays clean but any `nix build`
  # against it prints the message.
  checkAppSrc =
    appName: appSrc:
    if builtins.pathExists (appSrc + "/CWD_SENTINEL") then
      pkgs.runCommandNoCC "miniosv-${appName}-needs-override" { } ''
        cat >&2 <<'EOF'
        The `${appName}` flake input has not been overridden. Point it at
        your app tree, e.g.:

            nix build --override-input ${appName} "path:$PWD" \
                github:seb711/miniosv#${appName}-x86_64

        Or, from a downstream flake:

            inputs.miniosv.inputs.${appName}.url = "path:./my-app";
        EOF
        exit 1
      ''
    else
      appSrc;
  checkedApps = lib.mapAttrs checkAppSrc apps;

  # Per-arch metadata. Add a new target arch by adding a row here.
  arches = {
    x64 = {
      linuxName = "x86_64";
      qemuName = "x86_64";
      firmwarePrefix = "OVMF";
      crossPkgs = pkgs.pkgsCross.gnu64;
    };
    aarch64 = {
      linuxName = "aarch64";
      qemuName = "aarch64";
      firmwarePrefix = "AAVMF";
      crossPkgs = pkgs.pkgsCross.aarch64-multiplatform;
    };
  };

  hostArch = if system == "x86_64-linux" then "x64" else "aarch64";

  llvmSource = pkgs.callPackage ./llvm-source.nix { };
  toolchain = import ./toolchain.nix { inherit pkgs llvmPkgs system; };

  # Narrowed source for compiler-rt / llvm-libc / libcxx: only the files their
  # scripts actually read. Keeps toolchain drv hashes invariant to unrelated
  # changes in the flake tree.
  toolchainSrc =
    let
      root = toString self;
      keep = [
        "scripts"
        "external/llvm-libc-config"
        "include/api"
      ];
      keepAncestor = [
        "external"
        "include"
      ];
    in
    builtins.path {
      path = self;
      name = "miniosv-toolchain-src";
      filter =
        path: _:
        let
          rel = lib.removePrefix (root + "/") (toString path);
        in
        builtins.any (p: rel == p || lib.hasPrefix (p + "/") rel) keep || builtins.elem rel keepAncestor;
    };

  # Kernel source for the image build: everything except app/. The selected
  # app is staged into app/ from its own `appSrc`, so the in-tree default app
  # must stay out of this source — otherwise editing app/app.cc would
  # invalidate the image derivation of every *other* app too.
  kernelSrc =
    let
      root = toString self;
    in
    builtins.path {
      path = self;
      name = "miniosv-kernel-src";
      filter = path: _: lib.removePrefix (root + "/") (toString path) != "app";
    };

  # Per-arch toolchain sub-builds (independent of the selected app).
  toolchainFor =
    targetArch:
    let
      args = {
        inherit toolchain llvmSource targetArch;
        src = toolchainSrc;
      };
    in
    {
      compilerRt = pkgs.callPackage ./compiler-rt.nix (args // { isCross = targetArch != hostArch; });
      llvmLibc = pkgs.callPackage ./llvm-libc.nix args;
      libcxx = pkgs.callPackage ./libcxx.nix args;
    };

  archToolchains = lib.mapAttrs (a: _: toolchainFor a) arches;

  # (targetArch, app) -> { miniosv, run, awsDeploy } derivations.
  buildVariant =
    {
      targetArch,
      appName,
      appSrc,
    }:
    let
      arch = arches.${targetArch};
      isNative = targetArch == hostArch;
      ovmfPkg = if isNative then pkgs.OVMF else arch.crossPkgs.OVMF;
    in
    rec {
      miniosv = pkgs.callPackage ./miniosv.nix (
        {
          inherit
            toolchain
            llvmSource
            targetArch
            appName
            appSrc
            ;
          src = kernelSrc;
        }
        // archToolchains.${targetArch}
      );

      run = import ./run.nix {
        inherit
          pkgs
          lib
          miniosv
          self
          ;
        inherit (arch) qemuName firmwarePrefix;
        ovmfFdDir = "${ovmfPkg.fd}/FV";
      };

      awsDeploy = import ./aws-deploy.nix {
        inherit
          pkgs
          lib
          miniosv
          self
          ;
      };
    };

  # Cross-product: apps × arches → { "<app>-<linuxName>" = variant; ... }.
  variants = lib.concatMapAttrs (
    appName: _appSrc:
    # Force the sentinel check by taking appSrc from checkedApps; an
    # unoverridden `cwd` throws with a helpful message when this attr is
    # forced (e.g. by nix build .#cwd-x86_64).
    let appSrc = checkedApps.${appName}; in
    lib.mapAttrs' (
      targetArch: arch:
      lib.nameValuePair "${appName}-${arch.linuxName}" (buildVariant {
        inherit targetArch appName appSrc;
      })
    ) arches
  ) apps;

  mkApp = binName: pkg: {
    type = "app";
    program = "${pkg}/bin/${binName}";
  };

  # `nix build .` / `nix run .` build and boot the in-tree app for the host
  # arch. This is the only host-arch alias, and it exists only because the app
  # named `default` ships with the repo, so there is an obvious thing to mean.
  # (`apps` here is the function argument, not the output attr below.)
  hostLinuxName = arches.${hostArch}.linuxName;
  hasDefaultApp = apps ? default;
  defaultVariant = variants."default-${hostLinuxName}" or null;
in
{
  packages =
    lib.mapAttrs (_: v: v.miniosv) variants
    // lib.optionalAttrs hasDefaultApp { default = defaultVariant.miniosv; };

  apps =
    lib.mapAttrs (_: v: mkApp "miniosv-run" v.run) variants
    // lib.mapAttrs' (
      name: v: lib.nameValuePair "aws-deploy-${name}" (mkApp "miniosv-aws-deploy" v.awsDeploy)
    ) variants
    // lib.optionalAttrs hasDefaultApp { default = mkApp "miniosv-run" defaultVariant.run; };

  devShells = import ./devshells.nix { inherit pkgs toolchain system; };
}
