{
  description = "miniosv — slim unikernel OS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";

    # The `cwd` slot is a special app whose default is a sentinel that
    # refuses to build; override it with --override-input to point at a
    # local tree:
    #   nix build --override-input cwd "path:$PWD" \
    #       github:miniosv/miniosv#cwd-x86_64
    # Or in a downstream flake:
    #   inputs.miniosv.url = "github:miniosv/miniosv";
    #   inputs.miniosv.inputs.cwd.url = "path:./my-app";
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
      cwd,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      import ./nix {
        inherit nixpkgs system self;
        apps = {
          default = ./app;
          cwd = cwd;
        };
      }
    );
}
