# web-brick tests - read a BrickEmuPy .brick device file and flatten it for the C++ core.
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
import json, os

def load(brickemu, device):
    path = os.path.join(brickemu, 'assets', device + '.brick')
    with open(path) as f:
        cfg = json.load(f)
    return cfg

def resolve(brickemu, p):
    return os.path.normpath(os.path.join(brickemu, p)) if p else None

def options(mask):
    """Mask options as (key, index, value): nested dicts become 'outer.inner', lists are indexed."""
    out = []
    def put(key, v):
        if isinstance(v, bool): out.append((key, 0, int(v)))
        elif isinstance(v, int): out.append((key, 0, v))
        elif isinstance(v, list):
            for i, x in enumerate(v):
                if isinstance(x, int): out.append((key, i, x))
        elif isinstance(v, dict):
            for k, x in v.items(): put(key + '.' + k, x)
    for k, v in mask.items(): put(k, v)
    return out

def buttons(cfg):
    """[(name, port, pin, level)] in the order of the 'buttons' section; direct inputs only."""
    direct = cfg.get('peripherals', {}).get('direct_input', {})
    out = []
    for name in cfg['buttons']:
        if name in direct:
            d = direct[name]
            out.append((name, d['port'], d['mask'], d['level']))
    return out

def machine_file(brickemu, device, path):
    cfg = load(brickemu, device)
    mask = cfg['mask_options']
    lines = ['core ' + cfg['core'], 'clock %d' % cfg['clock'], 'rom ' + resolve(brickemu, mask['rom_path'])]
    if mask.get('sound_rom_path'): lines.append('srom ' + resolve(brickemu, mask['sound_rom_path']))
    for k, i, v in options(mask): lines.append('opt %s %d %d' % (k, i, v))
    for name, port, pin, level in buttons(cfg): lines.append('button %s %d %d' % (port, pin, level))
    with open(path, 'w') as f: f.write('\n'.join(lines) + '\n')
    return cfg
