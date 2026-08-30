{ fetchgit }:

# Pinned llvmorg-22.1.7 sparse checkout. The union of paths spans what all three
# runtime sub-builds (compiler-rt, llvm-libc, libcxx) plus the kernel's libunwind
# -isystem need — this replaces the ad-hoc `git clone` the shell scripts do at
# build time. Tag must match LLVM_TAG in scripts/build-*.sh and llvm_project_tag
# in the Makefile.
fetchgit {
  url = "https://github.com/llvm/llvm-project.git";
  rev = "llvmorg-22.1.7";
  sparseCheckout = [
    "libc"
    "libcxx"
    "libcxxabi"
    "libunwind"
    "compiler-rt"
    "third-party"
    "runtimes"
    "cmake"
    "llvm/cmake"
    "llvm/utils"
  ];
  hash = "sha256-paVuy6mD1NFcD4XhjfYZtWXlwEdaO0LDZTmeLppJCRk=";
}
