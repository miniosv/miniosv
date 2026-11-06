# Shared helpers for symbolizing kernel backtraces captured on the serial
# console. Used by run.py -s (local QEMU) and aws-deploy.py -s (EC2 --attach).

import os
import re
import shutil
import subprocess
import sys

# `#5  0x00000000402188bf  cfa=0x00002000000fff80  <0x402188bf>`
_BT_LINE = re.compile(r'^#\s*\d+\s+(0x[0-9a-fA-F]+)\b', re.MULTILINE)

# DWARF line-0 means "no source line info for this PC" and col-0 means "no
# column"; showing them is misleading precision. Strip both cases.
_LINE_ZERO = re.compile(r':0:\d+(\s|$)')
_COL_ZERO = re.compile(r'(:\d+):0(\s|$)')


def elf_next_to(image_path):
    "loader.img and loader.elf live in the same build dir."
    return os.path.join(os.path.dirname(os.path.abspath(image_path)),
                        'loader.elf')


def _extract_blocks(text):
    "Return unique [backtrace] blocks (PC tuples), in order of first appearance."
    blocks, seen = [], set()
    for part in text.split('[backtrace]')[1:]:
        addrs = tuple(m.group(1) for m in _BT_LINE.finditer(part))
        if addrs and addrs not in seen:
            seen.add(addrs)
            blocks.append(addrs)
    return blocks


def _format(text):
    # Each --pretty-print paragraph lists inline scopes innermost-first,
    # ending with the outermost containing function. Take the last line so
    # function name and file:line come from the same DWARF entry.
    frames = [f for f in text.strip().split('\n\n') if f.strip()]
    out = []
    for i, frame in enumerate(frames):
        outer = frame.rstrip().split('\n')[-1]
        if outer.lstrip().startswith('(inlined by) '):
            outer = outer.lstrip()[len('(inlined by) '):]
        outer = _LINE_ZERO.sub(r'\1', outer)
        outer = _COL_ZERO.sub(r'\1\2', outer)
        out.append("  #%-2d %s" % (i, outer))
    return '\n'.join(out)


def _print_block(header, addrs, symbolizer, elf_path):
    proc = subprocess.run(
        [symbolizer, '--obj=' + elf_path, '--demangle', '--pretty-print'],
        input=('\n'.join(addrs) + '\n').encode(),
        capture_output=True, check=False)
    body = _format(proc.stdout.decode('utf-8', errors='replace'))
    tty = sys.stdout.isatty()
    accent = (lambda s: "\x1b[1;36m%s\x1b[0m" % s) if tty else (lambda s: s)
    rule = "━" * 72
    print()
    print(accent(rule))
    print(accent("  " + header))
    print(accent(rule))
    print(body)
    print(accent(rule))


def print_from(text, elf_path):
    """Extract every [backtrace] block from `text`, symbolize each against
    `elf_path`, and print a formatted, boxed block per unique backtrace."""
    blocks = _extract_blocks(text)
    if not blocks:
        return
    if not os.path.exists(elf_path):
        print("(symbolize: no ELF at %s, skipping)" % elf_path, file=sys.stderr)
        return
    symbolizer = shutil.which('llvm-symbolizer')
    if not symbolizer:
        print("(symbolize: llvm-symbolizer not on PATH; try `nix develop .#cli`)",
              file=sys.stderr)
        return
    for i, addrs in enumerate(blocks):
        header = "backtrace (symbolized)" if len(blocks) == 1 \
            else "backtrace #%d (symbolized)" % (i + 1)
        _print_block(header, addrs, symbolizer, elf_path)
