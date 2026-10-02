#!/usr/bin/env python3
# web-brick tools - build web/devices/ from a BrickEmuPy checkout.
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tools/make_devices.py <BrickEmuPy dir> [--thumbs]
#
# Copies the device definition (.brick) and faceplate (.svg) of every device whose chip the C++
# core supports, and writes web/devices/index.json with names, the ROM files each one needs
# (name, size, SHA-1: never the ROMs themselves) and a gamepad layout.
# --thumbs also renders the library pictures (needs: pip install playwright pillow).
import sys, os, json, hashlib, shutil, subprocess, re, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'web', 'devices')
sys.path.insert(0, os.path.join(HERE, '..', 'tests'))
import brickcfg

SUPPORTED_CORES = {'HT943', 'HTG12N0'}

# id: (name, what it is, detail line)
INFO = {
    'E23PlusMarkII96in1': ('Brick Game E-23 Plus Mark II', 'Brick game', '96 in 1 · HT-943D0'),
    'E88_8in1': ('Brick Game E-88', 'Brick game', '8 in 1 · HT-943E5'),
    'GA888': ('Block Game & Echo Key GA888', 'Keychain game', 'Falling blocks · HT-943I0'),
    'Keychain55in1': ('Keychain 55 in 1', 'Keychain game', 'HT-943I0'),
    'KeychainPinBall': ('Keychain Pin Ball', 'Keychain game', 'HT-943I0'),
    'Puyolin': ('Puyolin', 'Keychain game', '1997 · HT-943Q0'),
    'SpaceIntruderTK150I': ('Space Intruder TK-150I', 'LCD game', 'HTB943R0'),
    'MameGalaxian': ('Mame Galaxian', 'Keychain game', 'HTB943R0'),
    'MameTamagotch': ('Mame Game Tamagotch', 'Keychain game', 'HTGL43Q0'),
    'MickeyVGS': ('Mickey Deluxe Virtual Game', 'Virtual pet', 'HTGT43N0'),
}
ORDER = list(INFO)

# gamepad: which of the device's buttons each pad control presses (first name that exists wins)
PAD_GUESS = {
    'left': ['btnLeft', 'btnEarLeft'], 'right': ['btnRight', 'btnEarRight'], 'down': ['btnDown', 'btnLife'],
    'up': ['btnUp', 'btnRotate', 'btnStartRotate', 'btnFire', 'btnContinue'],
    'a': ['btnRotate', 'btnStartRotate', 'btnFire', 'btnStartShoot', 'btnStartOn', 'btnA', 'btnStart'],
    'b': ['btnB'],
    'start': ['btnStart', 'btnPause', 'btnOnPause'],
    'select': ['btnMute', 'btnSound', 'btnSelect', 'btnReset'],
}
PAD_OVERRIDE = {
    'MickeyVGS': {'left': 'btnEarLeft', 'right': 'btnEarRight', 'a': 'btnLeft', 'b': 'btnRight'},
    'Puyolin': {'left': 'btnLeft', 'right': 'btnRight', 'down': 'btnDown', 'a': 'btnStart', 'start': 'btnPause', 'select': 'btnMute'},
}

def pad_for(dev, names):
    if dev in PAD_OVERRIDE: return PAD_OVERRIDE[dev]
    pad = {}
    for ctl, cands in PAD_GUESS.items():
        for c in cands:
            if c in names and not (ctl == 'start' and pad.get('a') == c):
                pad[ctl] = c; break
    return pad

def sha1(path):
    with open(path, 'rb') as f: return hashlib.sha1(f.read()).hexdigest()

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('brickemu'); ap.add_argument('--thumbs', action='store_true')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    try: commit = subprocess.check_output(['git', '-C', a.brickemu, 'rev-parse', 'HEAD'], text=True).strip()
    except Exception: commit = 'main'
    assets = os.path.join(a.brickemu, 'assets')
    found = sorted(f[:-6] for f in os.listdir(assets) if f.endswith('.brick'))
    devices = []
    for dev in sorted(found, key=lambda d: (ORDER.index(d) if d in ORDER else 999, d)):
        cfg = brickcfg.load(a.brickemu, dev)
        if cfg['core'] not in SUPPORTED_CORES: continue
        face_src = brickcfg.resolve(a.brickemu, cfg['face_path'])
        if not os.path.exists(face_src): print('skip %s: no faceplate' % dev); continue
        unwired = [n for n in cfg['buttons'] if n not in cfg.get('peripherals', {}).get('direct_input', {})]
        if unwired or set(cfg.get('peripherals', {})) - {'direct_input'}:
            print('skip %s: needs a peripheral the core does not have yet' % dev); continue
        mask = cfg['mask_options']
        roms = []
        for role, key in (('rom', 'rom_path'), ('srom', 'sound_rom_path')):
            if not mask.get(key): continue
            p = brickcfg.resolve(a.brickemu, mask[key]); name = os.path.basename(p)
            r = {'role': role, 'file': name}
            if os.path.exists(p): r.update(size=os.path.getsize(p), sha1=sha1(p), upstream=True)
            roms.append(r)
            mask[key] = name
        cfg['face_path'] = dev + '.svg'
        with open(os.path.join(OUT, dev + '.json'), 'w') as f: json.dump(cfg, f, indent=1)
        shutil.copyfile(face_src, os.path.join(OUT, dev + '.svg'))
        svg = open(face_src, errors='ignore').read(4096)
        m = re.search(r'viewBox="([\d.\- ]+)"', svg)
        vb = [float(x) for x in m.group(1).split()] if m else [0, 0, 100, 100]
        name, kind, detail = INFO.get(dev, (dev, 'LCD game', cfg['core']))
        devices.append({'id': dev, 'name': name, 'kind': kind, 'detail': detail, 'core': cfg['core'],
                        'brick': dev + '.json', 'face': dev + '.svg', 'thumb': dev + '.webp',
                        'size': [round(vb[2]), round(vb[3])], 'roms': roms, 'pad': pad_for(dev, list(cfg['buttons']))})
        print('%-22s %-8s %s' % (dev, cfg['core'], 'ROM upstream' if all(r.get('upstream') for r in roms) else 'bring your own ROM'))
    index = {'source': 'https://github.com/azya52/BrickEmuPy', 'commit': commit,
             'romBase': 'https://raw.githubusercontent.com/azya52/BrickEmuPy/%s/assets/' % commit, 'devices': devices}
    with open(os.path.join(OUT, 'index.json'), 'w') as f: json.dump(index, f, indent=1)
    print('%d devices -> web/devices/index.json' % len(devices))
    if a.thumbs:
        subprocess.check_call([sys.executable, os.path.join(HERE, 'make_thumbs.py'), a.brickemu])

main()
