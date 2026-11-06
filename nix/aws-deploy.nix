{
  pkgs,
  lib,
  miniosv,
  self,
}:

let
  pythonEnv = pkgs.python3.withPackages (
    ps: with ps; [
      awscrt
      boto3
      botocore
      pyyaml
    ]
  );
in
pkgs.writeShellScriptBin "miniosv-aws-deploy" ''
  export PATH=${
    lib.makeBinPath [
      pythonEnv
      pkgs.awscli2
      pkgs.qemu
      pkgs.coreutils
    ]
  }:$PATH
  exec ${pythonEnv}/bin/python3 ${self}/scripts/aws-deploy.py \
      --image ${miniosv}/share/miniosv/loader.img \
      "$@"
''
