{
  pkgs,
  lib,
  miniosv,
  self,
  qemuName,
  firmwarePrefix,
  ovmfFdDir,
}:

pkgs.writeShellScriptBin "miniosv-run" ''
  export PATH=${
    lib.makeBinPath [
      pkgs.qemu
      pkgs.python3
      pkgs.coreutils
    ]
  }:$PATH
  export ${firmwarePrefix}_CODE="${ovmfFdDir}/${firmwarePrefix}_CODE.fd"
  export ${firmwarePrefix}_VARS="${ovmfFdDir}/${firmwarePrefix}_VARS.fd"

  # QEMU opens the boot disk R/W but the image path in the nix store is
  # read-only. Stage a writable copy per invocation.
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  install -m 0644 ${miniosv}/share/miniosv/loader.img "$work/loader.img"

  exec ${self}/scripts/run.py \
      --arch ${qemuName} \
      --image-path "$work/loader.img" \
      "$@"
''
