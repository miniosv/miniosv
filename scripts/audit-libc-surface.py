#!/usr/bin/env python3
"""Audit which musl objects the OSv image actually needs.

Computes the transitive closure of musl objects reachable from:
  - symbols referenced by OSv kernel/app objects (core, libc, arch, drivers,
    bsd, app, apps, fastlz, loader.o, runtime.o),
  - symbols referenced by the statically linked toolchain archives
    (libstdc++.a, libgcc.a, libgcc_eh.a, boost, gflags). This is conservative:
    it uses the whole archive's undefined set, not just the members that the
    linker would actually pull in.

Outputs (to stdout):
  - the externally-required musl symbols (the "libc surface" llvm-libc must
    eventually provide),
  - the needed musl objects,
  - the prunable musl objects (safe to delete from the Makefile musl list).

Usage: scripts/audit-libc-surface.py [builddir=build/last]
"""

import subprocess
import sys
import os
from collections import defaultdict

builddir = sys.argv[1] if len(sys.argv) > 1 else 'build/last'

def nm(args):
    r = subprocess.run(['nm'] + args, capture_output=True, text=True)
    return r.stdout.splitlines()

def defined_globals(path):
    out = set()
    for line in nm(['--defined-only', '-g', path]):
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'TWDBRVi':
            out.add(parts[2])
    return out

def undefined(path):
    out = set()
    for line in nm(['-u', path]):
        parts = line.split()
        if parts:
            sym = parts[-1].split('@')[0]
            out.add(sym)
    return out

# 1. musl object maps
musl_objs = []
for root, _, files in os.walk(os.path.join(builddir, 'musl')):
    for f in files:
        if f.endswith('.o'):
            musl_objs.append(os.path.join(root, f))

sym_to_obj = {}
obj_undefs = {}
obj_defs = {}
for o in musl_objs:
    d = defined_globals(o)
    obj_defs[o] = d
    for s in d:
        sym_to_obj.setdefault(s, o)
    obj_undefs[o] = undefined(o)

# 2. roots: external references
ext_undefs = set()
osv_dirs = ['core', 'libc', 'arch', 'drivers', 'bsd', 'app', 'apps', 'fastlz', 'tests']
for d in osv_dirs:
    p = os.path.join(builddir, d)
    if not os.path.isdir(p):
        continue
    for root, _, files in os.walk(p):
        for f in files:
            if f.endswith('.o'):
                ext_undefs |= undefined(os.path.join(root, f))
for f in ['loader.o', 'runtime.o']:
    p = os.path.join(builddir, f)
    if os.path.exists(p):
        ext_undefs |= undefined(p)

def cxx_lib(name):
    r = subprocess.run(['clang++', '-print-file-name=' + name],
                       capture_output=True, text=True)
    return r.stdout.strip()

archives = [cxx_lib('libstdc++.a'), cxx_lib('libgcc.a'), cxx_lib('libgcc_eh.a'),
            '/usr/lib/x86_64-linux-gnu/libboost_context.a',
            '/usr/lib/x86_64-linux-gnu/libgflags_nothreads.a']
for a in archives:
    if a and os.path.exists(a):
        ext_undefs |= undefined(a)

# libc/aliases.ld defines LHS = RHS link-time aliases. Two consequences:
#  - a reference to LHS is really a requirement on RHS,
#  - every alias line that stays in the file needs its RHS defined at link
#    time, so an alias whose LHS nobody references should be dropped from
#    aliases.ld rather than keep its RHS object alive.
aliases = {}
if os.path.exists('libc/aliases.ld'):
    import re as _re
    for line in open('libc/aliases.ld'):
        line = line.split('/*')[0]
        m = _re.match(r'\s*(\w+)\s*=\s*(\w+)\s*;', line)
        if m:
            aliases[m.group(1)] = m.group(2)
for lhs, rhs in aliases.items():
    if lhs in ext_undefs:
        ext_undefs.add(rhs)

# Symbols the kernel/app/toolchain need that musl currently provides:
surface = sorted(s for s in ext_undefs if s in sym_to_obj)

# 3. closure over musl objects
needed = set()
work = [sym_to_obj[s] for s in surface]
while work:
    o = work.pop()
    if o in needed:
        continue
    needed.add(o)
    for s in obj_undefs[o]:
        t = sym_to_obj.get(s)
        if t and t not in needed:
            work.append(t)

prunable = sorted(set(musl_objs) - needed)
needed = sorted(needed)

def rel(o):
    # build/last/musl/src/ctype/isalnum.o -> ctype/isalnum.o (Makefile form)
    p = os.path.relpath(o, os.path.join(builddir, 'musl', 'src'))
    return p

print('== externally required musl symbols (%d) ==' % len(surface))
for s in surface:
    print('  %s  [%s]' % (s, rel(sym_to_obj[s])))
print()
print('== needed musl objects (%d of %d) ==' % (len(needed), len(musl_objs)))
for o in needed:
    print('  ' + rel(o))
print()
print('== prunable musl objects (%d) ==' % len(prunable))
for o in prunable:
    print('  ' + rel(o))

# aliases whose RHS would not be defined by the needed musl objects nor by
# the kernel's own objects: these lines must be dropped from aliases.ld
kernel_defs = set()
for d in osv_dirs:
    p = os.path.join(builddir, d)
    if not os.path.isdir(p):
        continue
    for root, _, files in os.walk(p):
        for f in files:
            if f.endswith('.o'):
                kernel_defs |= defined_globals(os.path.join(root, f))
for f in ['loader.o', 'runtime.o']:
    p = os.path.join(builddir, f)
    if os.path.exists(p):
        kernel_defs |= defined_globals(p)
needed_defs = set()
for o in needed:
    needed_defs |= obj_defs[o]
print()
print('== aliases.ld lines with undefined RHS (drop these) ==')
for lhs, rhs in sorted(aliases.items()):
    if rhs not in needed_defs and rhs not in kernel_defs and rhs not in aliases:
        print('  %s = %s;' % (lhs, rhs))
