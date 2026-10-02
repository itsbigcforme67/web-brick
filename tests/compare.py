#!/usr/bin/env python3
# web-brick tests - check the C++ core against BrickEmuPy, instruction for instruction.
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tests/compare.py <BrickEmuPy dir> [--seconds 30] [--seed 1] [--every 1000] [device ...]
#
# For each device: makes a random button script, runs the original Python core and the C++ core
# with it, and compares checkpoints (program flow, registers, RAM, LCD) and every sound event.
# Then repeats the C++ run with a save/load round trip at every checkpoint, which must not
# change anything, and runs the built web/core/brick.wasm in Node the same way.
# Devices without a ROM in the BrickEmuPy folder are skipped.
import sys, os, subprocess, random, argparse, tempfile, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brickcfg

SUPPORTED = {'HT943', 'HTG12N0'}

def build():
    exe = os.path.join(HERE, 'host_trace')
    src = [os.path.join(HERE, 'host_trace.cpp')] + [os.path.join(HERE, '..', 'core', f) for f in ('brick.cpp', 'ht4bit.cpp')]
    cxx = os.environ.get('CXX', 'g++')
    subprocess.check_call([cxx, '-std=c++11', '-O2', '-Wall', '-Wextra', '-o', exe] + src)
    return exe

def make_script(cfg, seconds, seed, path):
    rnd = random.Random(seed)
    n = len(brickcfg.buttons(cfg)); clock = cfg['clock']
    t = 0.3; lines = []
    while t < seconds:
        b = rnd.randrange(n)
        hold = rnd.choice([0.03, 0.08, 0.15, 0.4, 1.2])
        lines.append('%d %d 1' % (int(t * clock), b))
        if rnd.random() < 0.2:                       # sometimes a second button while the first is held
            b2 = rnd.randrange(n)
            if b2 != b:
                lines.append('%d %d 1' % (int((t + hold * 0.3) * clock), b2))
                lines.append('%d %d 0' % (int((t + hold * 0.7) * clock), b2))
        lines.append('%d %d 0' % (int((t + hold) * clock), b))
        t += hold + rnd.choice([0.05, 0.2, 0.5, 1.0, 2.5])
    lines.sort(key=lambda l: int(l.split()[0]))
    with open(path, 'w') as f: f.write('\n'.join(lines) + '\n')
    return len(lines)

def diff(ref, got):
    """Returns None if equal, else a description of the first difference. Sound frequencies may differ by 1/256 Hz."""
    r, g = ref.split('\n'), got.split('\n')
    for i in range(max(len(r), len(g))):
        a = r[i] if i < len(r) else '<end>'; b = g[i] if i < len(g) else '<end>'
        if a == b: continue
        if a.startswith('A') and b.startswith('A'):
            x, y = a.split(), b.split()
            if x[:4] == y[:4] and x[5:] == y[5:] and abs(int(x[4]) - int(y[4])) <= 1: continue
        return 'line %d\n   BrickEmuPy: %s\n   C++ core:   %s' % (i + 1, a, b)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('brickemu'); ap.add_argument('devices', nargs='*')
    ap.add_argument('--seconds', type=float, default=30); ap.add_argument('--seed', type=int, default=1); ap.add_argument('--every', type=int, default=1000)
    a = ap.parse_args()
    exe = build()
    import shutil
    node = shutil.which('node') if os.path.exists(os.path.join(HERE, '..', 'web', 'core', 'brick.wasm')) else None
    if not node: print('(node or web/core/brick.wasm not found: skipping the WebAssembly check)')
    devices = a.devices or sorted(f[:-6] for f in os.listdir(os.path.join(a.brickemu, 'assets')) if f.endswith('.brick'))
    failed = 0; ran = 0
    with tempfile.TemporaryDirectory() as tmp:
        for dev in devices:
            cfg = brickcfg.load(a.brickemu, dev)
            if cfg['core'] not in SUPPORTED: continue
            rom = brickcfg.resolve(a.brickemu, cfg['mask_options'].get('rom_path'))
            if not rom or not os.path.exists(rom):
                print('%-28s skipped (no ROM in BrickEmuPy)' % dev); continue
            mf = os.path.join(tmp, 'machine.txt'); sf = os.path.join(tmp, 'script.txt')
            brickcfg.machine_file(a.brickemu, dev, mf)
            nev = make_script(cfg, a.seconds, a.seed, sf)
            total = str(int(a.seconds * cfg['clock']))
            ref = subprocess.run([sys.executable, os.path.join(HERE, 'ref_trace.py'), a.brickemu, dev, sf, total, str(a.every)], capture_output=True, text=True)
            if ref.returncode: print(ref.stderr); failed += 1; continue
            got = subprocess.run([exe, mf, sf, total, str(a.every)], capture_output=True, text=True)
            st = subprocess.run([exe, mf, sf, total, str(a.every), '--state-test'], capture_output=True, text=True)
            ran += 1
            wasm = None
            if node:
                w = subprocess.run([node, os.path.join(HERE, 'wasm_trace.mjs'), a.brickemu, dev, sf, total, str(a.every)], capture_output=True, text=True)
                refc = '\n'.join(l for l in ref.stdout.split('\n') if not l.startswith('A'))
                wasm = diff(refc, w.stdout) if w.returncode == 0 else 'WebAssembly run failed: ' + w.stderr[-400:]
            d = diff(ref.stdout, got.stdout) if got.returncode == 0 else 'C++ run failed: ' + got.stderr
            d2 = None
            if d is None:
                d2 = diff(ref.stdout, st.stdout) if st.returncode == 0 else 'state test failed: ' + st.stderr
            lines = ref.stdout.count('\n'); sound = ref.stdout.count('\nA ') + ref.stdout.startswith('A ')
            if d is None and d2 is None and wasm is None:
                print('%-28s OK   %s  %d checkpoints, %d sound events, %d button events, save/load round trips identical%s' % (dev, cfg['core'], lines - sound, sound, nev, ', brick.wasm identical' if node else ''))
            else:
                failed += 1
                print('%-28s FAIL %s' % (dev, d or (d2 and 'with save/load round trips: ' + d2) or ('brick.wasm: ' + wasm)))
    # the firmware-style example (static memory only) must build and run too
    demo_rom = os.path.join(a.brickemu, 'assets', 'E23PlusMarkII96in1.bin')
    if os.path.exists(demo_rom):
        demo = os.path.join(HERE, 'embed_example')
        core = [os.path.join(HERE, '..', 'core', f) for f in ('brick.cpp', 'ht4bit.cpp')]
        subprocess.check_call([os.environ.get('CXX', 'g++'), '-std=c++11', '-O2', '-Wall', '-Wextra', '-o', demo, os.path.join(HERE, 'embed_example.cpp')] + core)
        r = subprocess.run([demo, demo_rom, demo_rom[:-4] + '.srom'], capture_output=True, text=True)
        print('%-28s %s   %s' % ('embed_example', 'OK' if r.returncode == 0 else 'FAIL', r.stdout.strip()))
        failed += r.returncode != 0
    print('%d device(s) compared, %d failed' % (ran, failed))
    sys.exit(1 if failed else 0)

main()
