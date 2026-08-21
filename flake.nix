{
  description = "miniosv — slim unikernel OS";
  
  # Each app is staged into app/ inside the build sandbox and compiled into
  # the kernel image. 
  # To add an app: 
  # (1) add an input pointing at its source (any tree with a Makefile fragment 
  # declaring $(app-objects) and an osv_app_main entry point)
  # (2) add it to `apps` below. Every app gets image, run, and aws-deploy 
  # outputs for both x86_64 and aarch64.
  #
  # The `cwd` slot is a special app whose default is a sentinel that
  # refuses to build; override it with --override-input to point at a
  # local tree:
  #   nix build --override-input cwd "path:$PWD" \
  #       github:seb711/miniosv#cwd-x86_64
  # Or in a downstream flake:
  #   inputs.miniosv.url = "github:seb711/miniosv";
  #   inputs.miniosv.inputs.cwd.url = "path:./my-app";
  #
  # Flake outputs (per system):
  #   packages.<app>-<arch>              - loader.img
  #   apps.<app>-<arch>                  - QEMU boot wrapper
  #   apps.aws-deploy-<app>-<arch>       - AWS deploy wrapper
  #   devShells.{default,aws,cli}        - developer environments
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";

    miniduckdb = {
      url = "github:Martin-Lndbl/miniduckdb/miniosv-trunk";
      flake = false;
    };

    # Overridable slot for "the app in your working directory". The default
    # is a sentinel that refuses to build; see the block comment above for
    # how to override.
    cwd = {
      url = "path:./nix/cwd-sentinel";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      miniduckdb,
      cwd,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      import ./nix {
        inherit nixpkgs system self;
        apps = {
          hello = ./examples/hello;
          inherit miniduckdb;
          cwd = cwd;
        };
      }
    );
}
