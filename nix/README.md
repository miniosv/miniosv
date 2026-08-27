# nix/

Nix layer for miniOSv. Wraps the kernel Makefile build in reproducible
derivations, wires each user application into a full matrix of image / QEMU /
AWS-deploy outputs, and pins the LLVM toolchain sub-builds so they're only
rebuilt when their own inputs change.

## Layout

| File | Purpose |
|---|---|
| `default.nix` | Per-system entry point. Turns `flake.nix` inputs into `packages`, `apps`, `devShells`. Owns the `arches` table and the app × arch cross-product. |
| `miniosv.nix` | Builds the unikernel image (`loader.img` / `loader.efi` / `loader.elf`) for one (app, arch) pair. Stages `appSrc` into `app/`, then runs the top-level Makefile. |
| `compiler-rt.nix` | Provides `libclang_rt.builtins.a`. Native: repackages the clang resource-dir. Cross: builds from the pinned llvm-project. |
| `llvm-libc.nix` | Builds llvm-libc as a no-syscall static `libc.a` / `libm.a`. |
| `libcxx.nix` | Builds libc++ / libc++abi / libunwind static archives + v1 headers. |
| `toolchain.nix` | Assembles the clang / lld / llvm-* tools the kernel build needs. |
| `llvm-source.nix` | Pins the llvm-project sparse checkout shared by the three toolchain builds. |
| `run.nix` | Shell wrapper around `scripts/run.py`: exports OVMF/AAVMF firmware, stages a writable boot-disk copy, boots QEMU. |
| `aws-deploy.nix` | Shell wrapper around `scripts/aws-deploy.py` with Python + AWS deps pinned. |
| `devshells.nix` | `nix develop` environments (`default`, `aws`, `cli`). |
| `cwd-sentinel/` | Placeholder source for the overridable `cwd` app slot (see below). |

## Outputs

Per system (`x86_64-linux`, `aarch64-linux`), the flake exposes:

```
packages.<app>-<arch>              # loader.img (build only)
packages.default                   # = packages.default-<host arch>
apps.<app>-<arch>                  # QEMU boot wrapper
apps.default                       # = apps.default-<host arch>
apps.aws-deploy-<app>-<arch>       # AWS deploy wrapper
devShells.{default,aws,cli}
```

Where `<app>` is any key from the `apps` attrset in `flake.nix`
(`default`, `miniduckdb`, `cwd`, …) and `<arch>` is `x86_64` or
`aarch64`.

There is exactly one alias: `packages.default` and `apps.default` point at
the in-tree app (`../app/`) built for the *host* arch, so plain `nix build .`
and `nix run .` do the obvious thing. Every other target names both an app
and an arch explicitly, and only things in `apps.*` are runnable via
`nix run`.

## Common commands

```bash
# Build an image (no run):
nix build .                           # in-tree app, host arch
nix build .#default-x86_64
nix build .#miniduckdb-aarch64        # cross-compiled on any host

# Boot under QEMU (KVM when target == host, else TCG):
nix run .                             # in-tree app, host arch
nix run .#default-x86_64
nix run .#miniduckdb-aarch64          # TCG on x86 host: pass -cpu cortex-a72 -c 1 for a sane boot

# Deploy on AWS (see scripts/aws-deploy.py --help):
nix run .#aws-deploy-default-x86_64 -- eu-north-1 c5.large --attach
nix run .#aws-deploy-miniduckdb-aarch64    -- eu-north-1 c7g.large --attach

# Enter a dev shell (all toolchain + qemu on $PATH):
nix develop           # default
nix develop .#cli     # + bear / clang-tools / pyright / black
nix develop .#aws     # + awscli2 + python awscrt/boto3
```

## Firmware

The QEMU wrapper always exports the right UEFI firmware — `OVMF_*` for
x86_64 targets, `AAVMF_*` for aarch64 — pinned to nixpkgs. For cross runs
(target arch ≠ host arch) it goes through `pkgs.pkgsCross.<triple>.OVMF`,
which is available on cache.nixos.org so no local edk2 build is needed.

## Adding a new app

`flake.nix` owns the app registry. Two-line change:

```nix
inputs.my-app = {
  url    = "github:me/my-app/main";  # or "path:./my-app"
  flake  = false;
};

# in outputs:
apps = {
  default = ./app;
  inherit miniduckdb;
  my-app = my-app;                   # <-- new
  cwd = cwd;
};
```

Each app source must be miniOSv-compliant: it needs a top-level `Makefile`
that the kernel build includes via `include app/Makefile` (declaring
`app-objects = ...`) and a C/C++ entry point `extern "C" void
osv_app_main()`. See `../app/` for the minimum viable shape.

Rebuilding: after `nix flake lock --update-input my-app`, the full matrix
of `packages.my-app-{x86_64,aarch64}` and the two matching `apps.*` entries
appears automatically.

## The `cwd` slot (build from your working directory)

`flake.nix` declares a `cwd` input with a sentinel default. Nothing to
edit — just override at CLI time or from a downstream flake.

```bash
# From a checkout of the miniOSv repo:
nix build --override-input cwd "path:$PWD/my-app" .#cwd-x86_64

# From anywhere, against the remote flake:
nix build --override-input cwd "path:$PWD" github:seb711/miniosv#cwd-x86_64

# From another flake:
inputs.miniosv.url = "github:seb711/miniosv";
inputs.miniosv.inputs.cwd.url = "path:./my-app";
```

Building `.#cwd-*` without an override fails at build time with a message
explaining exactly what to type. `nix flake check` still passes — the
sentinel is a build-time failure, not an eval-time one, so the outputs
don't disappear from `nix flake show`.

## Adding a new target arch

One row in the `arches` table in `default.nix`:

```nix
arches = {
  x64     = { linuxName = "x86_64";  qemuName = "x86_64";  firmwarePrefix = "OVMF";  crossPkgs = pkgs.pkgsCross.gnu64; };
  aarch64 = { linuxName = "aarch64"; qemuName = "aarch64"; firmwarePrefix = "AAVMF"; crossPkgs = pkgs.pkgsCross.aarch64-multiplatform; };
  # riscv64 = { ... };
};
```

Everything downstream (`packages.*`, `apps.*`, `apps.aws-deploy-*`) picks
it up. Actually building the image still requires the kernel Makefile and
`scripts/build-*.sh` to know about the arch — that's a separate concern
from the nix wrapping.

## Toolchain caching

`compiler-rt`, `llvm-libc`, and `libcxx` are per-arch, not per-app. Every
`packages.<app>-<arch>` for a given arch consumes the same three
derivations by store hash — so switching apps or editing app sources
never rebuilds them. Confirm with:

```bash
for app in default miniduckdb; do
  echo "=== $app ==="
  nix derivation show .#$app-x86_64 | \
    jq -r '.[].inputs.drvs | keys[]' | \
    grep -E 'miniosv-(compiler-rt|llvm-libc|libcxx)'
done
# Same three .drv paths under both apps.
```

The three toolchain derivations also use a narrowed source
(`toolchainSrc` in `default.nix`) covering only `scripts/`,
`external/llvm-libc-config/`, and `include/api/` — the paths their build
scripts actually read. So editing `README.md`, `flake.nix`, wrappers in
`nix/`, or any app tree does **not** invalidate them. Only edits to a
whitelisted path trigger a rebuild.

The image derivation uses `kernelSrc` (the repo *minus* `app/`), so editing
the in-tree app rebuilds `packages.default-*` only — not the other apps.

## Debugging what will be built

```bash
nix flake show                          # all outputs
nix flake check --no-build              # eval only
nix path-info .#default-x86_64   # store path of the image
nix derivation show .#default-x86_64 | jq '.[].inputs.drvs | keys'
```
