#!/usr/bin/env python3
# web-brick tests - drive the real app in a phone-sized Chromium (needs: pip install playwright).
# Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tests/e2e.py <BrickEmuPy dir> [url] [screenshot dir]
#
# Serves nothing itself: start the site first, e.g.  (cd web && python3 -m http.server 8767)
# Checks: library, ROM import (files and a zip), ROM download (GitHub is replaced by the local
# BrickEmuPy folder, so no network is needed), booting every device with a ROM, touch / keyboard
# input, sound, and saved games.
import sys, os, io, time, zipfile, json

def main():
    from playwright.sync_api import sync_playwright
    brickemu = os.path.abspath(sys.argv[1]); assets = os.path.join(brickemu, 'assets')
    url = sys.argv[2] if len(sys.argv) > 2 else 'http://localhost:8767/'
    shots = sys.argv[3] if len(sys.argv) > 3 else None
    if shots: os.makedirs(shots, exist_ok=True)
    fails = []
    def check(name, ok, detail=''):
        print(('ok    ' if ok else 'FAIL  ') + name + (('  ' + str(detail)) if detail != '' else ''))
        if not ok: fails.append(name)
    def shot(pg, name):
        if shots: pg.screenshot(path=os.path.join(shots, name + '.png'))

    with sync_playwright() as p:
        b = p.chromium.launch(args=['--autoplay-policy=no-user-gesture-required'])
        ctx = b.new_context(viewport={'width': 390, 'height': 844}, device_scale_factor=2, has_touch=True, is_mobile=True)
        pg = ctx.new_page()
        errors = []
        pg.on('pageerror', lambda e: errors.append(str(e)))
        pg.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
        requested = []
        def github(route):
            name = route.request.url.split('/')[-1]
            requested.append(name)
            path = os.path.join(assets, name.replace('%26', '&'))
            if os.path.exists(path): route.fulfill(status=200, body=open(path, 'rb').read(), headers={'access-control-allow-origin': '*'})
            else: route.fulfill(status=404, body='')
        pg.route('https://raw.githubusercontent.com/**', github)

        pg.goto(url); pg.wait_for_selector('.game')
        index = pg.evaluate("fetch('devices/index.json').then(r => r.json())")
        n = len(index['devices'])
        check('library lists every device', pg.locator('.game').count() == n, n)
        check('nothing playable before ROMs are added', pg.locator('.game.missing').count() == n)
        shot(pg, '1-library-empty')

        # --- import two loose files
        pg.set_input_files('#import-input', [os.path.join(assets, 'E23PlusMarkII96in1.bin'), os.path.join(assets, 'E23PlusMarkII96in1.srom')])
        pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('1 of')")
        check('importing loose ROM files', True, pg.inner_text('#toast'))

        # --- import a zip laid out like the GitHub download, with unrelated files in it
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
            for f in ('GA888.bin', 'GA888.srom', 'GA888.svg', 'GA888.brick', 'DorayakiHouse.bin'):
                z.write(os.path.join(assets, f), 'BrickEmuPy-main/assets/' + f)
        pg.set_input_files('#import-input', [{'name': 'BrickEmuPy-main.zip', 'mimeType': 'application/zip', 'buffer': buf.getvalue()}])
        pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('2 of')")
        check('importing a zip', True, pg.inner_text('#toast'))

        # --- a file with the right name but the wrong contents is refused
        pg.set_input_files('#import-input', [{'name': 'E88_8in1.bin', 'mimeType': 'application/octet-stream', 'buffer': b'\x00' * 4096}])
        time.sleep(0.5)
        check('a wrong file under a known name is refused', pg.inner_text('#lib-status').startswith('2 of'), pg.inner_text('#toast'))

        # --- download the rest
        pg.click('#btn-fetch')
        with_rom = sum(1 for d in index['devices'] if all(r.get('upstream') for r in d['roms']))
        pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('%d of')" % with_rom, timeout=30000)
        check('download from BrickEmuPy fills in the rest', pg.locator('#rom-banner.hidden').count() == 1, '%d files requested' % len(requested))
        check('already imported ROMs are not downloaded again', 'E23PlusMarkII96in1.bin' not in requested and 'GA888.bin' not in requested)
        shot(pg, '2-library-ready')

        # --- boot every playable device
        P = "window.webBrick.player"
        lit_js = "() => %s.segs.filter(s => s.cur > 0.5).map(s => s.el.id).sort().join(',')" % P
        for i, d in enumerate(index['devices']):
            if not all(r.get('upstream') for r in d['roms']): continue
            pg.locator('.game').nth(i).click()
            pg.wait_for_selector('#stage svg')
            time.sleep(2.5)
            info = pg.evaluate("() => ({ segs: %s.segs.length, hits: Object.keys(%s.hits).length, buttons: Object.keys(%s.brick.buttons).length, names: Object.keys(%s.brick.buttons) })" % (P, P, P, P))
            lit = pg.evaluate(lit_js)
            power = next((n for n in ('btnOnOff', 'btnOn', 'btnStartOn', 'btnOnReset') if n in info['names']), None)
            if not lit and power:                     # some handhelds start switched off, like the real ones
                pg.evaluate("(n) => { %s.m.press(n, true); setTimeout(() => %s.m.press(n, false), 200); }" % (P, P), power)
                time.sleep(2.0); lit = pg.evaluate(lit_js)
            check('%s boots and draws' % d['id'], info['segs'] > 50 and lit.count(',') > 3 and info['hits'] == info['buttons'], '%d segments, %d lit, %d buttons' % (info['segs'], lit.count(',') + 1 if lit else 0, info['hits']))
            shot(pg, 'device-' + d['id'])
            pg.click('#btn-back'); pg.wait_for_selector('#library.active')

        # --- play the 96-in-1: touch, keyboard, sound
        pg.locator('.game').first.click(); pg.wait_for_selector('#stage svg'); time.sleep(1.0)
        check('saved game is picked up again', 'Continued' in pg.inner_text('#toast'), pg.inner_text('#toast'))
        pg.click('#btn-pmenu'); pg.click('text=Take the batteries out'); time.sleep(2.0)
        before = pg.evaluate(lit_js)
        box = pg.evaluate("() => { const r = %s.hits.btnStart.hit.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2, w: r.width, h: r.height } }" % P)
        check('small buttons get a finger-sized touch area', box['w'] >= 30 and box['h'] >= 30, '%.0f x %.0f px' % (box['w'], box['h']))
        pg.evaluate("() => { const a = %s.audio; a.ring.fill(0); }" % P)
        pg.touchscreen.tap(box['x'], box['y']); time.sleep(1.5)
        after = pg.evaluate(lit_js)
        check('tapping START changes the screen', after != before)
        loud = pg.evaluate("() => { const a = %s.audio; let m = 0; for (const v of a.ring) m = Math.max(m, Math.abs(v)); return m }" % P)
        check('sound is produced', loud > 0.05, 'peak %.2f' % loud)
        pg.keyboard.down('ArrowLeft'); time.sleep(0.15)
        held = pg.evaluate("() => %s.hits.btnLeft.hit.classList.contains('pressed')" % P)
        pg.keyboard.up('ArrowLeft'); time.sleep(0.15)
        released = not pg.evaluate("() => %s.hits.btnLeft.hit.classList.contains('pressed')" % P)
        check('keyboard presses and releases a button', held and released)
        for k in ('ArrowLeft', 'ArrowRight', 'Space', 'ArrowDown', 'ArrowDown'):
            pg.keyboard.press(k, delay=80); time.sleep(0.25)
        time.sleep(1.0)
        shot(pg, '3-playing')
        h1 = pg.evaluate(lit_js)
        pg.click('#btn-back'); pg.wait_for_selector('#library.active')
        check('leaving saves the game', 'saved' in pg.locator('.game').first.inner_text())
        pg.reload(); pg.wait_for_selector('.game')
        pg.locator('.game').first.click(); pg.wait_for_selector('#stage svg')
        check('game continues after reloading the page', 'Continued' in pg.inner_text('#toast'))
        pg.click('#btn-back'); pg.wait_for_selector('#library.active')

        # --- time passing while away (on by default for virtual pets; switched on here for a game that has a ROM)
        ids = [d['id'] for d in index['devices']]
        if 'AlienFever' in ids:
            card = pg.locator('.game').nth(ids.index('AlienFever'))
            pg.evaluate("localStorage.setItem('wbk.live.AlienFever', 'true')")
            backdate = """async (hours) => {
              const d = await new Promise((res) => { const r = indexedDB.open('web-brick', 1); r.onsuccess = () => res(r.result); });
              const s = await new Promise((res) => { const q = d.transaction('kv').objectStore('kv').get('state:AlienFever'); q.onsuccess = () => res(q.result); });
              s.saved = Date.now() - hours * 3600e3;
              await new Promise((res) => { const t = d.transaction('kv', 'readwrite'); t.objectStore('kv').put(s, 'state:AlienFever'); t.oncomplete = res; });
            }"""
            card.click(); pg.wait_for_selector('#stage svg'); time.sleep(1.0)
            pg.click('#btn-back'); pg.wait_for_selector('#library.active')
            pg.evaluate(backdate, 0.5)
            t0 = time.time(); card.click()
            pg.wait_for_selector('#catchup:not(.hidden)', timeout=5000)
            pg.wait_for_selector('#catchup.hidden', state='attached', timeout=60000)
            check('time away is caught up on', pg.evaluate("() => !%s.catching && !!%s.m" % (P, P)), '30 min in %.1f s' % (time.time() - t0))
            pg.click('#btn-back'); pg.wait_for_selector('#library.active')
            pg.evaluate(backdate, 48)
            card.click(); pg.wait_for_selector('#catchup:not(.hidden)', timeout=5000)
            shot(pg, '3b-catching-up')
            pg.click('#catchup-skip'); pg.wait_for_selector('#catchup.hidden', state='attached', timeout=5000)
            check('catching up can be skipped', 'Skipped' in pg.inner_text('#toast'), pg.inner_text('#toast'))
            pg.click('#btn-back'); pg.wait_for_selector('#library.active')

        # --- sideways phone and desktop layouts
        pg.set_viewport_size({'width': 844, 'height': 390})
        pg.locator('.game').nth(2).click(); pg.wait_for_selector('#stage svg'); time.sleep(1.5)
        r = pg.evaluate("() => { const r = document.querySelector('#stage svg').getBoundingClientRect(); return [r.width, r.height, innerWidth, innerHeight] }")
        check('fits a sideways phone', r[0] <= r[2] and r[1] <= r[3], r)
        shot(pg, '4-landscape')
        pg.click('#btn-back'); pg.wait_for_selector('#library.active')
        pg.set_viewport_size({'width': 1280, 'height': 800}); time.sleep(0.3)
        shot(pg, '5-desktop-library')

        check('no page errors', not errors, errors[:3])
        b.close()
    print('%d check(s) failed' % len(fails) if fails else 'all checks passed')
    sys.exit(1 if fails else 0)

main()
