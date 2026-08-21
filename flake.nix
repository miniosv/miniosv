{
  description = "miniosv — slim unikernel OS";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        llvmPkgs = pkgs.llvmPackages_20;

        rtArch = if system == "x86_64-linux" then "x86_64" else "aarch64";
        rtTriple = "${rtArch}-unknown-linux-gnu";

        # nixpkgs splits clang into clang-unwrapped (binary) + clang-unwrapped.lib
        # (resource-dir headers) + compiler-rt (builtins).  clang 20 expects all
        # three to live under a single -resource-dir root, so we merge them here.
        clangResourceDir = pkgs.runCommand "clang-20-resource-dir" { } ''
          mkdir -p $out/include
          cp -r ${llvmPkgs.clang-unwrapped.lib}/lib/clang/20/include/. $out/include/

          mkdir -p $out/lib/${rtTriple}
          rt="${llvmPkgs.compiler-rt}"
          # nixpkgs may use the old layout (lib/linux/libclang_rt.builtins-<arch>.a)
          # or the new per-triple layout (lib/<triple>/libclang_rt.builtins.a).
          if [ -f "$rt/lib/${rtTriple}/libclang_rt.builtins.a" ]; then
            ln -s "$rt/lib/${rtTriple}/libclang_rt.builtins.a" \
                  "$out/lib/${rtTriple}/libclang_rt.builtins.a"
          elif [ -f "$rt/lib/linux/libclang_rt.builtins-${rtArch}.a" ]; then
            ln -s "$rt/lib/linux/libclang_rt.builtins-${rtArch}.a" \
                  "$out/lib/${rtTriple}/libclang_rt.builtins.a"
          fi
        '';

        clang = pkgs.writeShellScriptBin "clang" ''
          exec ${llvmPkgs.clang-unwrapped}/bin/clang   -resource-dir ${clangResourceDir} "$@"
        '';
        clangPP = pkgs.writeShellScriptBin "clang++" ''
          exec ${llvmPkgs.clang-unwrapped}/bin/clang++ -resource-dir ${clangResourceDir} "$@"
        '';

        buildDeps = with pkgs; [
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
          gptfdisk
          (python3.withPackages (ps: [ ps.pyyaml ]))
        ];

        ovmf_prefix = if system == "x86_64-linux" then "OVMF" else "AAVMF";
      in
      {
        devShells = rec {
          default = pkgs.mkShell {
            nativeBuildInputs = buildDeps ++ [
              pkgs.qemu
              pkgs.gdb
            ];

            # UEFI boot requires OVMF installation
            "${ovmf_prefix}_CODE" = "${pkgs.OVMF.fd}/FV/${ovmf_prefix}_CODE.fd";
            "${ovmf_prefix}_VARS" = "${pkgs.OVMF.fd}/FV/${ovmf_prefix}_VARS.fd";
          };

          aws = default.overrideAttrs (default: {
            nativeBuildInputs = [
              pkgs.awscli2
              (pkgs.python3.withPackages (
                ps: with ps; [
                  awscrt
                  boto3
                  botocore
                  # We need to redeclare every python
                  # dependency from the default shell
                  pyyaml
                ]
              ))
            ]
            ++ default.nativeBuildInputs;
          });

          cli = aws.overrideAttrs (aws: {
            nativeBuildInputs =
              with pkgs;
              [
                bear
                black
                clang-tools
                pyright
              ]
              ++ aws.nativeBuildInputs;
          });

          rust = cli.overrideAttrs (cli: {
            nativeBuildInputs = [ pkgs.cargo ] ++ cli.nativeBuildInputs;
          });
        };
      }
    );
}
