#!/usr/bin/env python3

# Launcher for the slim miniosv UEFI unikernel under QEMU.
#
# miniosv boots only via UEFI now (the -kernel PVH/preboot paths were removed).
# This script boots the GPT/ESP disk image build/<mode>/loader.img through UEFI
# firmware (OVMF on x86_64, AAVMF on aarch64) with the kernel embedded as the
# EFI application - the same path the public clouds take. The kernel is diskless
# (the single app is linked in); the boot disk is attached as NVMe, matching AWS
# Nitro. The guest serial port is the console on your terminal.
#
# Just build, then run ./scripts/run.py. Override the firmware with the
# OVMF_CODE/OVMF_VARS (x86_64) or AAVMF_CODE/AAVMF_VARS (aarch64) env vars.
#
# Console keys: Ctrl-A C opens the QEMU monitor, Ctrl-A X quits.

import subprocess
import sys
import argparse
import os
import errno
import pty
import shutil
import tempfile

import symbolize

devnull = open('/dev/null', 'w')

osv_base = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
host_arch = os.uname().machine

stty_params = None

def stty_save():
    global stty_params
    p = subprocess.Popen(["stty", "-g"], stdout=subprocess.PIPE, stderr=devnull)
    out, _ = p.communicate()
    stty_params = out.strip()

def stty_restore():
    if stty_params:
        subprocess.call(["stty", stty_params], stderr=devnull)

def format_args(args):
    def format_arg(arg):
        if arg == '':
            return '""'
        elif ' ' in arg:
            return '"%s"' % arg
        elif arg[0] == '-':
            return '\\\n' + arg
        else:
            return arg
    return ' '.join(map(format_arg, args))

def pick(*candidates):
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None

def find_firmware(arch):
    "Return (code_path, vars_path) for the UEFI firmware, or exit with guidance."
    if arch == 'x86_64':
        code = os.environ.get('OVMF_CODE') or pick(
            '/usr/share/OVMF/OVMF_CODE_4M.fd',
            '/usr/share/OVMF/OVMF_CODE.fd',
            '/usr/share/ovmf/OVMF_CODE.fd')
        vars_ = os.environ.get('OVMF_VARS') or pick(
            '/usr/share/OVMF/OVMF_VARS_4M.fd',
            '/usr/share/OVMF/OVMF_VARS.fd',
            '/usr/share/ovmf/OVMF_VARS.fd')
        pkg, prefix = 'ovmf', 'OVMF'
    else:
        code = os.environ.get('AAVMF_CODE') or pick(
            '/usr/share/AAVMF/AAVMF_CODE.fd',
            '/usr/share/qemu-efi-aarch64/QEMU_EFI.fd',
            '/usr/share/edk2/aarch64/QEMU_EFI.fd',
            os.path.expanduser('~/.cache/aavmf/AAVMF_CODE.fd'))
        vars_ = os.environ.get('AAVMF_VARS') or pick(
            '/usr/share/AAVMF/AAVMF_VARS.fd',
            '/usr/share/qemu-efi-aarch64/QEMU_VARS.fd',
            os.path.expanduser('~/.cache/aavmf/AAVMF_VARS.fd'))
        pkg, prefix = 'qemu-efi-aarch64', 'AAVMF'
    if not code or not vars_:
        sys.exit("run: no UEFI firmware found for %s. Install '%s', or set the "
                 "%s_CODE and %s_VARS env vars to the firmware files."
                 % (arch, pkg, prefix, prefix))
    return code, vars_

def setup_pflash(arch, code, vars_, workdir):
    # pflash needs a writable copy of the variable store; the aarch64 virt
    # pflash also requires the firmware images to be exactly 64 MiB.
    code_copy = os.path.join(workdir, 'code.fd')
    vars_copy = os.path.join(workdir, 'vars.fd')
    shutil.copy(code, code_copy)
    shutil.copy(vars_, vars_copy)
    os.chmod(code_copy, 0o644)
    os.chmod(vars_copy, 0o644)
    if arch == 'aarch64':
        for f in (code_copy, vars_copy):
            with open(f, 'r+b') as fh:
                fh.truncate(64 * 1024 * 1024)
    return code_copy, vars_copy

def start_osv_qemu(options):
    workdir = tempfile.mkdtemp(prefix='miniosv-run-')
    try:
        code, vars_ = find_firmware(options.arch)
        code_copy, vars_copy = setup_pflash(options.arch, code, vars_, workdir)

        use_kvm = options.hypervisor == 'kvm'

        args = ["-m", options.memsize, "-smp", options.vcpus, "-no-reboot"]

        # Machine, CPU and acceleration.
        if options.arch == 'x86_64':
            args += ["-machine", "q35"]
        else:
            gic = "max" if use_kvm else options.gic_version
            args += ["-machine", "virt,gic-version=%s" % gic]
        if use_kvm:
            args += ["-enable-kvm", "-cpu", "host"]
        else:
            args += ["-cpu", "max"]

        # UEFI firmware on pflash.
        args += [
            "-drive", "if=pflash,format=raw,readonly=on,file=%s" % code_copy,
            "-drive", "if=pflash,format=raw,file=%s" % vars_copy]

        # Boot disk: the GPT/ESP image as an NVMe drive. The firmware finds
        # \EFI\BOOT\BOOT{X64,AA64}.EFI on it, exactly as on AWS Nitro.
        args += [
            "-drive", "id=bootdisk,format=raw,if=none,file=%s" % options.image_file,
            "-device", "nvme,serial=miniosv,drive=bootdisk"]

        # Optional extra emulated NVMe drive (e.g. a backing store for the app).
        if options.emulated_nvme:
            args += [
                "-drive", "file=%s,if=none,id=nvm1" % options.emulated_nvme,
                "-device", "nvme,serial=deadbeef,drive=nvm1"]

        # PCI passthrough: one -device per address. Devices must be bound to
        # vfio-pci on the host, and QEMU must run with enough privilege (sudo).
        for pci in options.pass_pci or []:
            args += ["-device", "vfio-pci,host=%s" % pci]

        # No networking, no graphics: the console is the serial port on this
        # terminal (Ctrl-A C for the QEMU monitor, Ctrl-A X to quit).
        args += ["-nic", "none", "-display", "none", "-serial", "mon:stdio"]

        if not options.nogdb:
            args += ["-gdb", "tcp::%s,server,nowait" % options.gdb]
        if options.wait:
            args += ["-S"]
        if options.no_shutdown:
            args += ["-no-shutdown"]

        for a in options.pass_args or []:
            args += a.split()

        qemu_path = options.qemu_path or ('qemu-system-%s' % options.arch)
        cmdline = [qemu_path] + args

        if options.dry_run:
            print(format_args(cmdline))
            return

        try:
            stty_save()
            if options.symbolize:
                log_path = os.path.join(workdir, 'console.log')
                ret = _run_with_tee(cmdline, log_path)
                if ret != 0:
                    print("qemu exited with status %d." % ret, file=sys.stderr)
                with open(log_path) as fh:
                    symbolize.print_from(
                        fh.read(), symbolize.elf_next_to(options.image_file))
            else:
                ret = subprocess.call(cmdline, env=os.environ.copy())
                if ret != 0:
                    sys.exit("qemu failed.")
        except OSError as e:
            if e.errno == errno.ENOENT:
                print("'%s' binary not found. Please install the "
                      "qemu-system-%s package." % (qemu_path, options.arch),
                      file=sys.stderr)
            else:
                print("OS error(%d): \"%s\" while running %s" %
                      (e.errno, e.strerror, " ".join(cmdline)), file=sys.stderr)
        finally:
            stty_restore()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

def _run_with_tee(cmdline, log_path):
    "Run cmdline on a pty, forwarding stdio and copying everything to log_path."
    log = open(log_path, 'wb')
    def read(fd):
        data = os.read(fd, 4096)
        log.write(data)
        log.flush()
        return data
    try:
        return pty.spawn(cmdline, read)
    finally:
        log.close()


def choose_hypervisor(arch):
    # KVM only when the guest arch matches the host and /dev/kvm is usable.
    # Note: x86 miniosv's only KVM clock is kvmclock, so an x86 guest really
    # wants KVM (under pure TCG it has no time source); aarch64 is fine on TCG
    # (its clock is the always-present ARM generic timer).
    if os.path.exists('/dev/kvm') and os.access('/dev/kvm', os.W_OK) and arch == host_arch:
        return 'kvm'
    return 'tcg'

def main(options):
    if options.hypervisor not in ("tcg", "kvm"):
        sys.exit("Unrecognized hypervisor '%s' (expected kvm or tcg)."
                 % options.hypervisor)
    start_osv_qemu(options)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='run',
        description='Boot the miniosv UEFI image under QEMU.')
    parser.add_argument("-d", "--debug", action="store_true",
                        help="run the debug build (build/debug)")
    parser.add_argument("-r", "--release", action="store_true",
                        help="run the release build (build/release)")
    parser.add_argument("-w", "--wait", action="store_true",
                        help="freeze the guest at startup (resume from the monitor or gdb)")
    parser.add_argument("-m", "--memsize", action="store", default="2G",
                        help="guest memory, e.g. 1G, 2G (default 2G; >=2G recommended)")
    parser.add_argument("-c", "--vcpus", action="store", default="4",
                        help="number of vcpus (default 4)")
    parser.add_argument("-p", "--hypervisor", action="store", default="auto",
                        help="acceleration: kvm, tcg, or auto (default)")
    parser.add_argument("-H", "--no-shutdown", action="store_true",
                        help="keep qemu alive on guest shutdown/reset (for the debugger)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the QEMU command line and exit")
    parser.add_argument("--nogdb", action="store_true",
                        help="do not expose the gdb stub")
    parser.add_argument("--gdb", action="store", default="1234",
                        help="gdb stub port (default 1234)")
    parser.add_argument("--pass-args", action="append",
                        help="append raw arguments to the QEMU command line")
    parser.add_argument("--qemu-path", action="store",
                        help="path to the qemu binary (default qemu-system-<arch>)")
    parser.add_argument("--image-path", action="store",
                        help="path to the boot image (default build/<mode>/loader.img)")
    parser.add_argument("--arch", action="store", choices=["x86_64", "aarch64"],
                        default=host_arch,
                        help="guest architecture (default: host arch)")
    parser.add_argument("--emulated-nvme", action="store", metavar="IMAGE",
                        help="attach a raw disk image as an extra emulated NVMe device")
    parser.add_argument("--pass-pci", action="store", nargs='+', metavar="ADDR",
                        help="passthrough PCI device(s) bound to vfio-pci, e.g. 0000:01:00.0")
    parser.add_argument("--gic-version", action="store", default="3",
                        help="aarch64 GIC version under TCG (default 3)")
    parser.add_argument("-s", "--symbolize", action="store_true",
                        help="after qemu exits, run any [backtrace] addresses "
                             "through llvm-symbolizer against loader.elf")
    cmdargs = parser.parse_args()

    # The build output dir is build/<mode>.<arch> (arch as x64 / aarch64), so
    # derive the image from --arch rather than the arch-ambiguous build/last.
    mode = "debug" if cmdargs.debug else "release"
    archdir = "x64" if cmdargs.arch == "x86_64" else "aarch64"
    cmdargs.image_file = os.path.abspath(cmdargs.image_path or
        os.path.join(osv_base, "build/%s.%s/loader.img" % (mode, archdir)))
    if not os.path.exists(cmdargs.image_file):
        sys.exit("Boot image %s does not exist. Build it first, e.g.:\n"
                 "  make arch=%s" % (cmdargs.image_file, archdir))

    if cmdargs.hypervisor == "auto":
        cmdargs.hypervisor = choose_hypervisor(cmdargs.arch)

    main(cmdargs)
