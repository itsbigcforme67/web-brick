#!/usr/bin/env python3
# web-brick tests - run the built web/core/brick.wasm with wasmtime and print checkpoints in the same
# format as tests/ref_trace.py. Same job as wasm_trace.mjs, for machines without Node (pip install wasmtime).
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tests/wasm_trace.py <BrickEmuPy dir> <device> <script file> <total cycles> <every N steps>
import sys, os
import wasmtime
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brickcfg

def main():
    brickemu, dev, script, total, every = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
    cfg = brickcfg.load(brickemu, dev)
    st = wasmtime.Store()
    inst = wasmtime.Instance(st, wasmtime.Module.from_file(st.engine, os.path.join(HERE, '..', 'web', 'core', 'brick.wasm')), [])
    x = lambda n: inst.exports(st)[n]
    mem = x('memory')
    def blob(b):
        p = x('brick_alloc')(st, len(b)); assert p; mem.write(st, b, p); return p
    cstr = lambda t: blob(t.encode() + b'\0')
    # the same set-up as Machine in web/core/brick.js
    x('brick_init')(st)
    if not x('brick_create')(st, cstr(cfg['core']), cfg['clock']): sys.exit('core not supported: ' + cfg['core'])
    mask = cfg['mask_options']
    for role, key in ((0, 'rom_path'), (1, 'sound_rom_path')):
        if mask.get(key):
            d = open(brickcfg.resolve(brickemu, mask[key]), 'rb').read()
            x('brick_set_rom')(st, role, blob(d), len(d))
    for k, i, v in brickcfg.options(mask): x('brick_set_option')(st, cstr(k), i, v)
    btn = [x('brick_add_button')(st, cstr(port), pin, level) for _, port, pin, level in brickcfg.buttons(cfg)]
    x('brick_audio')(st, 16000, 8192)
    x('brick_reset')(st)
    events = [tuple(map(int, l.split())) for l in open(script) if l.strip()]
    step_, pc_, hash_, press, audio = x('brick_step'), x('brick_pc'), x('brick_state_hash'), x('brick_press'), x('brick_audio_read')
    abuf = x('brick_alloc')(st, 8192)
    M = 0xFFFFFFFF; ei = cyc = step = 0; flow = 2166136261; out = []
    while cyc < total:
        while ei < len(events) and events[ei][0] <= cyc:
            press(st, btn[events[ei][1]], events[ei][2]); ei += 1
        k = step_(st); cyc += k; step += 1
        flow = ((flow ^ (pc_(st) & M)) * 16777619) & M
        flow = ((flow ^ (cyc & M)) * 16777619) & M
        if step % every == 0:
            out.append('C %d %d %08x %08x' % (step, cyc, flow, hash_(st) & M))
            audio(st, abuf, 4096)
    sys.stdout.write('\n'.join(out) + '\n')

main()
