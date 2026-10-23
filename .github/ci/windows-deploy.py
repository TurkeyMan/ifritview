#!/usr/bin/env python3
"""Copies into STAGE/bin every MinGW DLL that the exe and the loaders under
STAGE/lib reach, and writes a loaders.cache naming only the loaders shipped.
Usage: windows-deploy.py STAGE MINGW_BIN"""
import os, re, shutil, subprocess, sys

stage, mingw_bin = sys.argv[1], sys.argv[2]
bin_dir = os.path.join(stage, 'bin')

def imports(path):
    out = subprocess.run(['x86_64-w64-mingw32-objdump', '-p', path], capture_output=True, text=True).stdout
    return re.findall(r'DLL Name: (\S+)', out)

todo = [os.path.join(bin_dir, 'ifritview.exe')]
for root, _, files in os.walk(os.path.join(stage, 'lib')):
    todo += [os.path.join(root, f) for f in files if f.endswith('.dll')]
seen = set()
while todo:
    for dll in imports(todo.pop()):
        name = dll.lower()
        if name in seen:
            continue
        seen.add(name)
        src = next((os.path.join(mingw_bin, f) for f in os.listdir(mingw_bin) if f.lower() == name), None)
        if src:  # otherwise it's Windows' own
            shutil.copy2(src, bin_dir)
            todo.append(src)

# Fedora's cache lists loaders it doesn't ship; keep the ones that are here.
for root, _, files in os.walk(os.path.join(stage, 'lib', 'gdk-pixbuf-2.0')):
    if 'loaders.cache' not in files:
        continue
    cache = os.path.join(root, 'loaders.cache')
    entries = open(cache).read().split('\n\n')
    kept = [e for e in entries
            if not (m := re.search(r'^"(\.\./lib/[^"]+\.dll)"', e, re.M))
            or os.path.exists(os.path.join(bin_dir, m.group(1)))]
    open(cache, 'w').write('\n\n'.join(kept))
print(f'{len(os.listdir(bin_dir))} files in bin')
