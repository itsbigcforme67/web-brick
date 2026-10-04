#!/usr/bin/env python3
# web-brick tests - reference trace from the original BrickEmuPy cores (no Qt needed).
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tests/ref_trace.py <BrickEmuPy dir> <device> <script file> <total cycles> <checkpoint every N steps>
#
# Prints the same lines as tests/host_trace (the C++ core), so the two can be compared:
#   C <step> <cycles> <flow hash> <state hash>     every N instructions
#   A <cycles> <channel> <on> <freq*256> <noise> <amp*256>   whenever the sound output changes
import sys, os, json
M = 0xFFFFFFFF

def main():
    brickemu, device, script, total, every = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import brickcfg
    cfg = brickcfg.load(brickemu, device)
    cwd = os.getcwd(); os.chdir(brickemu); sys.path.insert(0, brickemu)
    from cores import cores_map
    from peripherals import peripherals_map
    from interconnect import Interconnect

    out = []
    state = {'cyc': 0, 'chan': {}}   # cyc: whole cycles run so far
    class Emu:
        def audio_handler(self, ch, data):
            ev = (0, 0, 0, 0) if data is None else (1, int(data[0] * 256), int(bool(data[1])), int(round(data[2] * 256)))
            if state['chan'].get(ch, (0, 0, 0, 0)) != ev:
                state['chan'][ch] = ev
                out.append('A %d %d %d %d %d %d' % ((int(state['cyc']), ch) + ev))
        def serial_tx_handler(self, data): pass
    ic = Interconnect(Emu())
    cpu = cores_map[cfg['core']]['core'](cfg['mask_options'], cfg['clock'], ic)
    for name, value in cfg.get('peripherals', {}).items():
        if name in peripherals_map: peripherals_map[name](value, ic)
    os.chdir(cwd)

    names = [b[0] for b in brickcfg.buttons(cfg)]
    events = []
    with open(script) as f:
        for line in f:
            c, b, d = line.split()
            events.append((int(c), names[int(b)], d == '1'))
    ei = 0; cyc = 0; step = 0; flow = 2166136261
    clock = cpu.clock; pc = cpu.pc; emit_clock = ic.emit_clock; nev = len(events)
    while cyc < total:
        while ei < nev and events[ei][0] <= cyc:
            ic.emit_input(events[ei][1], events[ei][2]); ei += 1
        state['cyc'] = cyc
        k = clock(); emit_clock(k)
        cyc += k; step += 1                          # some cores (E0C6200) return fractions of a cycle
        flow = ((flow ^ pc()) * 16777619) & M
        flow = ((flow ^ (int(cyc) & M)) * 16777619) & M
        if step % every == 0:
            h = 2166136261
            for v in cpu.examine().values():
                if isinstance(v, (tuple, list)):
                    for x in v: h = ((h ^ int(x)) * 16777619) & M
                else: h = ((h ^ int(v)) * 16777619) & M
            for x in cpu.get_VRAM(): h = ((h ^ int(x)) * 16777619) & M
            out.append('C %d %d %08x %08x' % (step, int(cyc), flow, h))
    sys.stdout.write('\n'.join(out) + '\n')

main()
