#!/usr/bin/env python3
# web-brick tools - render the library pictures (web/devices/<id>.webp).
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tools/make_thumbs.py <BrickEmuPy dir>
#
# Each picture is the device's faceplate with its screen as it looks a few seconds after
# switching on (taken from the original BrickEmuPy core when the ROM is there, otherwise blank).
# Needs: pip install playwright pillow, and a Chromium for Playwright.
import sys, os, json, io
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'web', 'devices')
HEIGHT = 420

def screen_after(brickemu, cfg, seconds=6):
    mask = dict(cfg['mask_options'])
    for k in ('rom_path', 'sound_rom_path'):
        if mask.get(k): mask[k] = os.path.join(brickemu, 'assets', mask[k])
    if not os.path.exists(mask.get('rom_path', '')): return None
    cwd = os.getcwd(); os.chdir(brickemu)
    if brickemu not in sys.path: sys.path.insert(0, brickemu)
    try:
        from cores import cores_map
        from peripherals import peripherals_map
        from interconnect import Interconnect
        class Emu:
            def audio_handler(self, *a): pass
            def serial_tx_handler(self, *a): pass
        ic = Interconnect(Emu())
        cpu = cores_map[cfg['core']]['core'](mask, cfg['clock'], ic)
        for n, v in cfg.get('peripherals', {}).items():
            if n in peripherals_map: peripherals_map[n](v, ic)
        best = None; cyc = 0; target = cfg['clock'] * seconds; nxt = cfg['clock'] // 2
        # a handheld that starts switched off (as some real ones do) gets its power button pressed after a second
        power = next((n for n in ('btnOnOff', 'btnOn') if n in cfg['buttons']), None)
        pressed = 0
        while cyc < target:
            if power and pressed == 0 and cyc > cfg['clock']:
                if any(cpu.get_VRAM()): pressed = 2      # it is on already
                else: ic.emit_input(power, True); pressed = 1
            if pressed == 1 and cyc > cfg['clock'] * 1.2: ic.emit_input(power, False); pressed = 2
            k = cpu.clock(); ic.emit_clock(k); cyc += k
            if cyc >= nxt:                       # keep the busiest screen seen, sampled twice a second
                nxt += cfg['clock'] // 2
                v = list(cpu.get_VRAM()); lit = sum(bin(x).count('1') for x in v)
                if best is None or lit >= best[0]: best = (lit, v)
        return best[1] if best else None
    finally:
        os.chdir(cwd)

JS = """([vram, ghost]) => {
  // same clean-up as web/app.js pruneFace(): draw only the body, the overlay and the segments
  const noDraw = new Set(['defs','style','clipPath','mask','linearGradient','radialGradient','pattern','filter','title','desc','metadata']);
  const isSeg = (e) => /^\\d+_\\d$/.test(e.id || '');
  const prune = (parent, top) => { for (const c of [...parent.children]) {
    if (noDraw.has(c.localName) || isSeg(c)) continue;
    if (top && (c.id === 'body' || c.id === 'overlay')) continue;
    if ([...c.querySelectorAll('[id]')].some(isSeg)) prune(c, false); else c.remove(); } };
  prune(document.querySelector('svg'), true);
  for (const e of document.querySelectorAll('svg [id]')) {
    const m = /^(\\d+)_(\\d)$/.exec(e.id); if (!m) continue;
    const on = vram && +m[1] < vram.length ? (vram[+m[1]] >> +m[2]) & 1 : 0;
    e.style.opacity = Math.min(1, on + ghost);
  }
}"""

def main():
    from playwright.sync_api import sync_playwright
    from PIL import Image
    brickemu = os.path.abspath(sys.argv[1])
    index = json.load(open(os.path.join(OUT, 'index.json')))
    with sync_playwright() as p:
        b = p.chromium.launch()
        for d in index['devices']:
            cfg = json.load(open(os.path.join(OUT, d['brick'])))
            w = round(HEIGHT * d['size'][0] / d['size'][1])
            pg = b.new_page(viewport={'width': w, 'height': HEIGHT}, device_scale_factor=1)
            svg = open(os.path.join(OUT, d['face']), errors='ignore').read()
            pg.set_content('<html><body style="margin:0;background:transparent"><style>svg{width:100vw;height:100vh;display:block}</style>%s</body></html>' % svg)
            vram = screen_after(brickemu, cfg)
            pg.evaluate(JS, [vram, cfg.get('display', {}).get('ghost_segments', 0)])
            png = pg.screenshot(omit_background=True)
            pg.close()
            Image.open(io.BytesIO(png)).save(os.path.join(OUT, d['thumb']), 'WEBP', quality=82, method=6)
            print('%-22s %dx%d %6d bytes%s' % (d['id'], w, HEIGHT, os.path.getsize(os.path.join(OUT, d['thumb'])), '' if vram else '  (blank screen: no ROM)'))
        b.close()

main()
