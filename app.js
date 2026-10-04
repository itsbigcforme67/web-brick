// web-brick - Copyright (C) 2026 the web-brick authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// Phone/desktop UI: library, ROM storage, the handheld on screen (SVG faceplate), sound, saves.
// The chips are emulated by core/brick.wasm; device definitions and faceplates come from BrickEmuPy.
import { loadCore, Machine } from './core/brick.js';
import { isZip, unzip } from './zip.js';

const $ = (s) => document.querySelector(s);
const STATE_FORMAT = 1;

// ------------------------------------------------------------------ storage (IndexedDB)
const db = (() => {
  let p;
  const open = () => p || (p = new Promise((res, rej) => {
    const r = indexedDB.open('web-brick', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  }));
  const tx = async (mode, fn) => {
    const d = await open();
    return new Promise((res, rej) => {
      const t = d.transaction('kv', mode);
      const req = fn(t.objectStore('kv'));
      t.oncomplete = () => res(req && req.result);
      t.onerror = () => rej(t.error);
    });
  };
  return {
    get: (k) => tx('readonly', (s) => s.get(k)),
    set: (k, v) => tx('readwrite', (s) => s.put(v, k)),
    del: (k) => tx('readwrite', (s) => s.delete(k)),
    keys: () => tx('readonly', (s) => s.getAllKeys()),
  };
})();

const prefs = {
  get(k, d) { try { const v = localStorage.getItem('wbk.' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('wbk.' + k, JSON.stringify(v)); } catch { /* private mode */ } },
};

async function sha1(bytes) {
  if (!(globalThis.crypto && crypto.subtle)) return '';      // plain http on a LAN address: no hashing available
  const h = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(h), (b) => b.toString(16).padStart(2, '0')).join('');
}

// ------------------------------------------------------------------ UI helpers
function toast(msg, ms = 2600) {
  const t = $('#toast');
  t.textContent = msg; t.classList.remove('hidden');
  clearTimeout(toast.timer); toast.timer = setTimeout(() => t.classList.add('hidden'), ms);
}
function sheet(title, bodyHTML, actions, onOpen) {
  return new Promise((resolve) => {
    $('#sheet-title').textContent = title;
    $('#sheet-body').innerHTML = bodyHTML || '';
    const box = $('#sheet-actions');
    box.innerHTML = '';
    const s = $('#sheet');
    const close = (v) => { s.classList.add('hidden'); s.onclick = null; resolve(v); };
    for (const a of actions) {
      const b = document.createElement('button');
      b.className = 'btn ' + (a.cls || '');
      b.textContent = a.label;
      b.onclick = () => close(a.value);
      box.appendChild(b);
    }
    s.onclick = (e) => { if (e.target === s) close(null); };
    s.classList.remove('hidden');
    if (onOpen) onOpen($('#sheet-body'), close);
  });
}
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
function showView(id) {
  document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.id === id));
  document.documentElement.classList.toggle('in-player', id === 'player');
}
function ago(t) {
  const s = Math.round((Date.now() - t) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return new Date(t).toLocaleDateString();
}

// ------------------------------------------------------------------ devices and their ROMs
let index = { devices: [] };       // devices/index.json
let have = new Set();              // ROM file names stored on this device
let saved = {};                    // device id -> time of its saved game

const ROLE_NAME = { rom: 'program ROM', srom: 'sound ROM' };
const missing = (dev) => dev.roms.filter((r) => !have.has(r.file));
const canFetch = (r) => !!(r.upstream && index.romBase);
const ready = (dev) => missing(dev).length === 0;

async function refreshStore() {
  const keys = await db.keys();
  have = new Set(keys.filter((k) => typeof k === 'string' && k.startsWith('rom:')).map((k) => k.slice(4)));
  saved = {};
  for (const k of keys) if (typeof k === 'string' && k.startsWith('state:')) { const s = await db.get(k); if (s) saved[k.slice(6)] = s.saved; }
}

/** Every ROM file the library knows about: lower-case name -> entry, and SHA-1 -> entry. */
function romCatalogue() {
  const byName = new Map(), bySha = new Map();
  for (const d of index.devices) for (const r of d.roms) { byName.set(r.file.toLowerCase(), r); if (r.sha1) bySha.set(r.sha1, r); }
  return { byName, bySha };
}

/** Store whatever ROM files are in this list of { name, data }. Returns how many were recognised. */
async function addRomFiles(files) {
  const { byName, bySha } = romCatalogue();
  let n = 0;
  for (const f of files) {
    const base = f.name.split(/[\\/]/).pop().toLowerCase();
    let r = byName.get(base);
    if (r && r.sha1 && (await sha1(f.data)) !== r.sha1 && crypto.subtle) r = null;   // right name, wrong contents
    if (!r) r = bySha.get(await sha1(f.data));
    if (!r) continue;
    await db.set('rom:' + r.file, f.data);
    n++;
  }
  return n;
}

async function importFiles(fileList) {
  const { byName } = romCatalogue();
  const files = [];
  for (const f of fileList) {
    const buf = new Uint8Array(await f.arrayBuffer());
    if (isZip(buf)) {
      // a zip of ROMs, or the whole BrickEmuPy download: only unpack files that look like ROMs
      try { files.push(...await unzip(buf, (name) => byName.has(name.split('/').pop().toLowerCase()) || /\.(bin|srom|rom)$/i.test(name))); }
      catch (e) { toast(`${f.name}: ${e.message}`); }
    } else files.push({ name: f.name, data: buf });
  }
  const before = index.devices.filter(ready).length;
  const n = await addRomFiles(files);
  await refreshStore(); renderLibrary();
  const now = index.devices.filter(ready).length;
  toast(n ? `Added ${n} ROM file${n === 1 ? '' : 's'}${now > before ? `: ${now - before} more handheld${now - before === 1 ? '' : 's'} ready` : ''}` : 'No ROM files for these handhelds were found in that selection', 3600);
}

/** Download the ROMs that BrickEmuPy publishes, straight from its GitHub repository to this device. */
async function fetchUpstream(only) {
  const want = [];
  for (const d of only ? [only] : index.devices) for (const r of d.roms) if (canFetch(r) && !have.has(r.file) && !want.includes(r)) want.push(r);
  if (!want.length) return 0;
  let ok = 0, failed = 0;
  for (const r of want) {
    toast(`Downloading ${ok + failed + 1} of ${want.length}…`, 20000);
    try {
      const res = await fetch(index.romBase + encodeURIComponent(r.file));
      if (!res.ok) throw new Error(res.status);
      const data = new Uint8Array(await res.arrayBuffer());
      const h = await sha1(data);
      if (data.length !== r.size || (h && h !== r.sha1)) throw new Error('unexpected contents');
      await db.set('rom:' + r.file, data);
      ok++;
    } catch { failed++; }
  }
  await refreshStore(); renderLibrary();
  toast(failed ? `Downloaded ${ok}, ${failed} failed. Check the connection, or import the files instead.` : `Downloaded ${ok} ROM file${ok === 1 ? '' : 's'}`, 4000);
  return ok;
}

// ------------------------------------------------------------------ library
const FILTER_TITLE = { all: 'Handhelds', arcade: 'Arcade games', pet: 'Virtual pets' };
function renderLibrary() {
  const grid = $('#game-grid');
  grid.innerHTML = '';
  const filter = prefs.get('filter', 'all');
  for (const b of document.querySelectorAll('.filter button')) b.setAttribute('aria-selected', String(b.dataset.filter === filter));
  $('#lib-title').textContent = FILTER_TITLE[filter] || FILTER_TITLE.all;
  for (const d of index.devices) {
    if (filter !== 'all' && (d.group || 'arcade') !== filter) continue;
    const ok = ready(d);
    const card = document.createElement('div');
    card.className = 'game' + (ok ? '' : ' missing');
    card.setAttribute('role', 'button'); card.tabIndex = 0;
    const status = !ok ? '<span class="tag warn">needs ROM</span>' : saved[d.id] ? `<span class="tag good">saved ${esc(ago(saved[d.id]))}</span>` : '';
    card.innerHTML = `<div class="thumb"><img src="devices/${esc(d.thumb)}" alt="" loading="lazy"></div>
      <div class="name">${esc(d.name)}</div>
      <div class="meta"><span class="tag">${esc(d.kind)}</span>${status}</div>
      <div class="meta">${esc(d.detail)}</div>
      <button class="more" aria-label="Options for ${esc(d.name)}">⋯</button>`;
    card.onclick = (e) => { if (e.target.closest('.more')) deviceMenu(d); else if (ok) openDevice(d); else romSheet(d); };
    card.onkeydown = (e) => { if (e.key === 'Enter') card.click(); };
    grid.appendChild(card);
  }
  const n = index.devices.filter(ready).length;
  $('#lib-status').textContent = n ? `${n} of ${index.devices.length} ready to play` : 'ROMs are stored on this device only';
  const fetchable = index.devices.some((d) => d.roms.some((r) => canFetch(r) && !have.has(r.file)));
  $('#rom-banner').classList.toggle('hidden', !(fetchable || n === 0));
  $('#btn-fetch').classList.toggle('hidden', !fetchable);
  $('#rom-banner-text').innerHTML = fetchable
    ? `Most of these are published in the <a href="${esc(index.source || '#')}" target="_blank" rel="noopener">BrickEmuPy</a> project: download them from there to this device in one go, or import your own files (single ROMs, a zip, or the BrickEmuPy zip).`
    : `Import your own files: single ROMs, a zip of them, or the zip of the <a href="${esc(index.source || '#')}" target="_blank" rel="noopener">BrickEmuPy</a> project.`;
}

async function romSheet(d) {
  const miss = missing(d);
  const allUp = miss.every(canFetch);
  const list = miss.map((r) => `<code>${esc(r.file)}</code> (${ROLE_NAME[r.role] || r.role})`).join(' and ');
  const acts = [];
  if (allUp) acts.push({ label: 'Download from BrickEmuPy', value: 'fetch', cls: 'primary' });
  for (const r of miss) acts.push({ label: `Choose the ${ROLE_NAME[r.role] || r.role} file…`, value: r });
  acts.push({ label: 'Cancel', value: null });
  const v = await sheet(d.name, `<p>This handheld needs ${list}.</p><p>${allUp ? 'BrickEmuPy publishes ' + (miss.length > 1 ? 'them' : 'it') + '; it can be downloaded from there to this device.' : 'BrickEmuPy does not include ' + (miss.length > 1 ? 'these' : 'this') + ', so you need your own dump.'} ROMs stay in this browser and are never uploaded.</p>`, acts);
  if (v === 'fetch') { await fetchUpstream(d); if (ready(d)) openDevice(d); }
  else if (v) pickRole(d, v);
}

function pickRole(d, rom) {
  const inp = $('#role-input');
  inp.value = '';
  inp.onchange = async () => {
    const f = inp.files[0];
    if (!f) return;
    let data = new Uint8Array(await f.arrayBuffer());
    if (isZip(data)) { const inner = await unzip(data).catch(() => []); if (inner.length === 1) data = inner[0].data; else { toast('Choose the ROM file itself, or a zip holding only that file'); return; } }
    if (rom.size && data.length !== rom.size) { toast(`That file is ${data.length} bytes; ${rom.file} should be ${rom.size}`); return; }
    await db.set('rom:' + rom.file, data);
    await refreshStore(); renderLibrary();
    if (ready(d)) openDevice(d); else romSheet(d);
  };
  inp.click();
}

async function deviceMenu(d) {
  const acts = [];
  if (ready(d)) acts.push({ label: saved[d.id] ? 'Continue' : 'Play', value: 'play', cls: 'primary' });
  else acts.push({ label: 'Add its ROM…', value: 'rom', cls: 'primary' });
  if (saved[d.id]) acts.push({ label: 'Start fresh (forget the saved game)', value: 'fresh' });
  if (d.roms.some((r) => have.has(r.file))) acts.push({ label: 'Remove its ROM from this device', value: 'unrom', cls: 'danger' });
  acts.push({ label: 'Close', value: null });
  const v = await sheet(d.name, `<p>${esc(d.kind)} · ${esc(d.detail)} · ${esc(d.core)} chip</p>`, acts);
  if (v === 'play') openDevice(d);
  if (v === 'rom') romSheet(d);
  if (v === 'fresh') { await db.del('state:' + d.id); await refreshStore(); renderLibrary(); openDevice(d); }
  if (v === 'unrom') {
    // a ROM file can be shared by two devices (regional variants): keep it while another one still uses it
    for (const r of d.roms) if (!index.devices.some((o) => o !== d && ready(o) && o.roms.some((x) => x.file === r.file))) await db.del('rom:' + r.file);
    await refreshStore(); renderLibrary();
  }
}

// ------------------------------------------------------------------ player
const player = { dev: null, brick: null, m: null, raf: 0, last: 0, lcdAcc: 0, segs: [], hits: {}, keys: {}, audio: null, muted: prefs.get('muted', false), pad: {}, opening: false, catching: null, hiddenAt: 0 };

// ---- time passing while away: virtual pets keep living (hunger, age, the clock) unless switched off
const MAX_AWAY_MS = 7 * 24 * 3600 * 1000;               // catching up longer than a week is not worth the wait
const livesOn = (d) => prefs.get('live.' + d.id, d.group === 'pet');
function duration(ms) {
  const m = Math.round(ms / 60000);
  if (m < 1) return `${Math.round(ms / 1000)} s`;
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h} h ${m % 60} min`;
  return `${Math.floor(h / 24)} days ${h % 24} h`;
}

/** Run the machine through `ms` of its own time as fast as the device allows, in 30 ms slices.
 *  Resolves when done or when the player skips the rest (which then simply does not happen). */
function catchUp(ms) {
  ms = Math.min(ms, MAX_AWAY_MS);
  if (!player.m || ms < 2000) return Promise.resolve();
  if (player.catching) { player.catching.left += ms; player.catching.total += ms; return player.catching.done; }
  releaseEverything();
  const box = $('#catchup');
  const c = { left: ms, total: ms, skip: false };
  player.catching = c;
  box.classList.remove('hidden');
  $('#catchup-skip').onclick = () => { c.skip = true; };
  c.done = new Promise((resolve) => {
    const m = player.m;
    const slice = () => {
      if (player.m !== m) { box.classList.add('hidden'); player.catching = null; resolve(); return; }
      const until = performance.now() + 30;
      while (c.left > 0 && !c.skip && performance.now() < until) {
        const step = Math.min(c.left, 1000);
        m.runMs(step);
        m.readAudio();                                    // nobody was there to hear it
        c.left -= step;
      }
      $('#catchup-text').textContent = `Catching up on ${duration(c.total)} away… ${Math.floor(100 * (1 - c.left / c.total))}%`;
      drawLcd(true);
      if (c.left > 0 && !c.skip) { setTimeout(slice, 0); return; }   // timers, not frames: keeps going even where nothing is painted
      box.classList.add('hidden');
      player.catching = null;
      player.last = performance.now();
      if (c.skip) toast(`Skipped ${duration(c.left)}: that time did not pass for ${player.dev ? player.dev.name : 'it'}`, 3600);
      resolve();
    };
    setTimeout(slice, 0);
  });
  return c.done;
}

// BrickEmuPy stores keyboard shortcuts as Qt key codes
const QT_KEYS = { ArrowLeft: 16777234, ArrowUp: 16777235, ArrowRight: 16777236, ArrowDown: 16777237, Enter: 16777220, Escape: 16777216, Tab: 16777217, Backspace: 16777219, ' ': 32 };
const QT_NAMES = { 16777234: '←', 16777235: '↑', 16777236: '→', 16777237: '↓', 16777220: 'Enter', 32: 'Space' };
function qtCode(e) {
  if (QT_KEYS[e.key] !== undefined) return QT_KEYS[e.key];
  if (e.key.length === 1) return e.key.toUpperCase().charCodeAt(0);
  return -1;
}
const keyName = (c) => QT_NAMES[c] || (c > 32 && c < 127 ? String.fromCharCode(c) : '?');
const buttonLabel = (n) => n.replace(/^btn/, '').replace(/([a-z])([A-Z])/g, '$1 / $2');

async function openDevice(d) {
  if (player.opening) return;
  player.opening = true;
  try {
    $('#game-title').textContent = d.name;
    $('#game-sub').textContent = d.detail;
    const stage = $('#stage');
    stage.innerHTML = '<p class="loading">Loading…</p>';
    showView('player');
    setupAudio();
    const [brick, svgText, module, rom, srom, snap] = await Promise.all([
      fetch('devices/' + d.brick).then((r) => r.json()),
      fetch('devices/' + d.face).then((r) => r.text()),
      loadCore(),
      db.get('rom:' + d.roms.find((r) => r.role === 'rom').file),
      d.roms.some((r) => r.role === 'srom') ? db.get('rom:' + d.roms.find((r) => r.role === 'srom').file) : null,
      db.get('state:' + d.id),
    ]);
    const m = await Machine.create(module, { brick, rom, soundRom: srom, sampleRate: player.audio ? player.audio.ctx.sampleRate : 0, gain: 16384 });
    let resumed = false;
    if (snap && snap.format === STATE_FORMAT && snap.data) resumed = m.loadState(snap.data);
    const away = resumed && livesOn(d) && snap.saved ? Date.now() - snap.saved : 0;
    if (!resumed) m.reset();
    Object.assign(player, { dev: d, brick, m, last: performance.now(), lcdAcc: 0, keys: {}, pad: {} });
    mountFace(svgText);
    applySound();
    cancelAnimationFrame(player.raf);
    player.raf = requestAnimationFrame(frame);
    if (away >= 2000) catchUp(away);
    else if (resumed) toast('Continued where you left off', 1600);
  } catch (e) {
    console.error(e);
    toast('Could not start: ' + e.message, 4000);
    showView('library');
  } finally { player.opening = false; }
}

/** Keep only what BrickEmuPy draws: the "body" and "overlay" layers and the LCD segments.
 *  (A few drawings carry leftovers, such as a white backdrop, that the original never shows.) */
const SVG_NO_DRAW = new Set(['defs', 'style', 'clipPath', 'mask', 'linearGradient', 'radialGradient', 'pattern', 'filter', 'title', 'desc', 'metadata']);
const isSegment = (e) => /^\d+_\d$/.test(e.id || '');
function pruneFace(parent, top) {
  for (const c of [...parent.children]) {
    if (SVG_NO_DRAW.has(c.localName) || isSegment(c)) continue;
    if (top && (c.id === 'body' || c.id === 'overlay')) continue;
    if ([...c.querySelectorAll('[id]')].some(isSegment)) pruneFace(c, false); else c.remove();
  }
}

/** Put the faceplate SVG on screen, find its LCD segments and make its buttons work. */
function mountFace(svgText) {
  const stage = $('#stage');
  const doc = new DOMParser().parseFromString(svgText, 'image/svg+xml');
  const svg = doc.documentElement;
  if (!svg || svg.nodeName !== 'svg') throw new Error('faceplate did not load');
  svg.querySelectorAll('script, foreignObject').forEach((n) => n.remove());
  pruneFace(svg, true);
  svg.removeAttribute('width'); svg.removeAttribute('height');
  stage.innerHTML = '';
  stage.appendChild(document.importNode(svg, true));
  const live = stage.firstElementChild;
  const NS = 'http://www.w3.org/2000/svg';
  const look = lcdLook();

  // LCD segments: an element with id "<n>_<bit>" shows bit <bit> of byte <n> of the chip's display memory
  player.segs = [];
  for (const el of live.querySelectorAll('[id]')) {
    const mm = /^(\d+)_(\d)$/.exec(el.id);
    if (!mm) continue;
    const base = el.hasAttribute('opacity') ? parseFloat(el.getAttribute('opacity')) || 1 : 1;
    if (look.shadow) {
      // the shadow each segment casts on the reflector behind the glass: a faint copy, slightly offset
      const u = document.createElementNS(NS, 'use');
      u.setAttribute('href', '#' + el.id);
      u.setAttribute('transform', `translate(${look.shadow} ${look.shadow})`);
      u.setAttribute('opacity', '0.1');
      u.style.pointerEvents = 'none';
      el.parentNode.insertBefore(u, el);
    }
    el.style.opacity = '0';
    player.segs.push({ el, i: +mm[1], bit: +mm[2], base, cur: 0, shown: -1 });
  }

  // buttons: the faceplate's own shapes, with a larger invisible touch area on top of small ones
  const ctm = live.getScreenCTM();
  const inv = ctm ? ctm.inverse() : null;
  const vb = live.viewBox.baseVal;
  const boxes = [];
  for (const name of Object.keys(player.brick.buttons)) {
    const el = live.querySelector('#' + CSS.escape(name));
    if (!el || !inv || player.m.buttons[name] === undefined) continue;
    const r = el.getBoundingClientRect();
    const a = new DOMPoint(r.left, r.top).matrixTransform(inv), b = new DOMPoint(r.right, r.bottom).matrixTransform(inv);
    boxes.push({ name, x: a.x, y: a.y, w: b.x - a.x, h: b.y - a.y });
  }
  const want = Math.min(vb.width, vb.height) * 0.1;      // comfortable finger size in drawing units
  const gap = (p, q) => Math.max(q.x - (p.x + p.w), p.x - (q.x + q.w), q.y - (p.y + p.h), p.y - (q.y + q.h));
  player.hits = {};
  for (const bx of boxes) {
    let pad = Math.max(0, (want - Math.min(bx.w, bx.h)) / 2);
    for (const o of boxes) if (o !== bx) pad = Math.min(pad, Math.max(0, gap(bx, o) / 2));
    const hit = document.createElementNS(NS, 'rect');
    hit.setAttribute('x', bx.x - pad); hit.setAttribute('y', bx.y - pad);
    hit.setAttribute('width', bx.w + 2 * pad); hit.setAttribute('height', bx.h + 2 * pad);
    hit.setAttribute('rx', Math.min(bx.w, bx.h) / 2 + pad);
    hit.setAttribute('class', 'bk-hit');
    hit.setAttribute('aria-label', buttonLabel(bx.name));
    const down = (e) => { e.preventDefault(); try { hit.setPointerCapture(e.pointerId); } catch { /* old browser */ } setButton(bx.name, true, 'touch'); resumeAudio(); };
    const up = (e) => { e.preventDefault(); setButton(bx.name, false, 'touch'); };
    hit.addEventListener('pointerdown', down);
    hit.addEventListener('pointerup', up);
    hit.addEventListener('pointercancel', up);
    hit.addEventListener('contextmenu', (e) => e.preventDefault());
    live.appendChild(hit);
    player.hits[bx.name] = { hit, held: new Set() };
  }
  drawLcd(true);
}

/** A button can be held by a finger, a key and a gamepad at once; it is released when the last one lets go.
 *  A very quick tap is stretched to MIN_HOLD_MS so the chip's key scan cannot miss it. */
const MIN_HOLD_MS = 60;
function setButton(name, down, source) {
  const h = player.hits[name] || (player.hits[name] = { hit: null, held: new Set() });
  if (down) h.held.add(source); else h.held.delete(source);
  const want = h.held.size > 0;
  if (want === !!h.on || !player.m) return;
  const off = () => { h.timer = 0; if (h.held.size || !h.on) return; h.on = false; if (player.m) player.m.press(name, false); if (h.hit) h.hit.classList.remove('pressed'); };
  if (want) {
    h.on = true; h.since = performance.now();
    player.m.press(name, true);
    if (h.hit) h.hit.classList.add('pressed');
  } else if (!h.timer) {
    const wait = MIN_HOLD_MS - (performance.now() - h.since);
    if (wait > 0) h.timer = setTimeout(off, wait); else off();
  }
}

function releaseEverything() {
  for (const [name, h] of Object.entries(player.hits)) {
    h.held.clear(); clearTimeout(h.timer); h.timer = 0;
    if (h.on) { h.on = false; if (player.m) player.m.press(name, false); if (h.hit) h.hit.classList.remove('pressed'); }
  }
}

function lcdLook() {
  const d = (player.brick && player.brick.display) || {};
  return {
    blur: prefs.get('lcdBlur', true) ? (d.motion_blur ?? 0.6) : 0,
    ghost: prefs.get('lcdGhost', true) ? (d.ghost_segments ?? 0) : 0,
    shadow: prefs.get('lcdShadow', true) ? (d.shadow ?? 5) : 0,
  };
}

/** One 60 Hz step of the LCD: segments fade towards on/off the way real liquid crystal does. */
function drawLcd(snap) {
  const v = player.m.vram();
  const look = lcdLook();
  const k = snap ? 1 : 1 - look.blur;
  for (const s of player.segs) {
    const target = s.i < v.length ? ((v[s.i] >> s.bit) & 1) + look.ghost : look.ghost;
    if (s.cur !== target) {
      s.cur += k * (target - s.cur);
      if (Math.abs(s.cur - target) < 1e-3) s.cur = target;
    }
    const show = Math.round(Math.min(1, s.cur) * s.base * 200) / 200;
    if (show !== s.shown) { s.shown = show; s.el.style.opacity = show; }
  }
}

function frame(now) {
  const dt = Math.min(Math.max(now - player.last, 0), 100);   // after a pause (tab hidden) do not try to catch up
  player.last = now;
  if (player.catching) { player.raf = requestAnimationFrame(frame); return; }
  pollGamepad();
  player.m.runMs(dt);
  pumpAudio();
  player.lcdAcc += dt;
  let steps = 0;
  while (player.lcdAcc >= 1000 / 60 && steps < 4) { drawLcd(false); player.lcdAcc -= 1000 / 60; steps++; }
  if (steps === 4) player.lcdAcc = 0;
  player.raf = requestAnimationFrame(frame);
}

async function saveNow() {
  if (!player.m || !player.dev || player.catching) return;   // mid catch-up the state is behind the clock
  const data = player.m.saveState();
  if (data) { await db.set('state:' + player.dev.id, { format: STATE_FORMAT, data, saved: Date.now() }); saved[player.dev.id] = Date.now(); }
}

async function closePlayer() {
  cancelAnimationFrame(player.raf); player.raf = 0;
  releaseEverything();
  await saveNow();
  player.m = null; player.dev = null; player.segs = []; player.hits = {};
  $('#stage').innerHTML = '';
  if (player.audio) player.audio.w = player.audio.r = 0;
  renderLibrary();
  showView('library');
}

// ---- sound
function setupAudio() {
  if (player.audio) return;
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  const ctx = new AC();
  const ring = new Float32Array(16384);
  const a = { ctx, ring, r: 0, w: 0 };
  const node = ctx.createScriptProcessor(1024, 0, 1);
  node.onaudioprocess = (e) => {
    const out = e.outputBuffer.getChannelData(0);
    const avail = (a.w - a.r + ring.length) % ring.length;
    for (let i = 0; i < out.length; i++) {
      if (i < avail) { out[i] = ring[a.r]; a.r = (a.r + 1) % ring.length; } else out[i] = 0;
    }
    const left = (a.w - a.r + ring.length) % ring.length;
    if (left > 6000) a.r = (a.w - 2048 + ring.length) % ring.length;   // keep latency bounded
  };
  const gain = ctx.createGain(); gain.gain.value = 0.35;
  node.connect(gain); gain.connect(ctx.destination);
  player.audio = a;
}
function resumeAudio() { if (player.audio && !player.muted && player.audio.ctx.state !== 'running') player.audio.ctx.resume().catch(() => {}); }
function applySound() {
  $('#btn-sound').textContent = player.muted ? '🔇' : '🔈';
  if (player.audio) { if (player.muted) player.audio.w = player.audio.r = 0; else resumeAudio(); }
}
function pumpAudio() {
  const a = player.audio;
  for (let guard = 0; guard < 4; guard++) {
    const s = player.m.readAudio();
    if (!s.length) break;
    if (a && !player.muted) for (let i = 0; i < s.length; i++) { a.ring[a.w] = s[i] / 32768; a.w = (a.w + 1) % a.ring.length; }
    if (s.length < 4096) break;
  }
}

// ---- keyboard and gamepad
function onKey(e, down) {
  if (!player.m || player.catching || !$('#sheet').classList.contains('hidden')) return;
  if (e.key === 'Escape') { if (down) closePlayer(); return; }
  if (e.repeat) { e.preventDefault(); return; }
  const code = qtCode(e);
  let used = false;
  for (const [name, b] of Object.entries(player.brick.buttons)) {
    if ((b.hot_keys || []).includes(code)) { setButton(name, down, 'key'); used = true; }
  }
  if (used) { e.preventDefault(); resumeAudio(); }
}
const PAD_BUTTONS = { a: [0], b: [1, 2], select: [8], start: [9], up: [12], down: [13], left: [14], right: [15] };
function pollGamepad() {
  if (!navigator.getGamepads || !player.dev) return;
  const now = {};
  for (const gp of navigator.getGamepads()) {
    if (!gp || !gp.connected) continue;
    for (const [ctl, idxs] of Object.entries(PAD_BUTTONS)) if (idxs.some((i) => gp.buttons[i] && gp.buttons[i].pressed)) now[ctl] = true;
    if (gp.axes[0] < -0.5) now.left = true; if (gp.axes[0] > 0.5) now.right = true;
    if (gp.axes[1] < -0.5) now.up = true; if (gp.axes[1] > 0.5) now.down = true;
  }
  for (const [ctl, name] of Object.entries(player.dev.pad || {})) {
    if (!!now[ctl] !== !!player.pad[ctl]) { setButton(name, !!now[ctl], 'pad:' + ctl); if (now[ctl]) resumeAudio(); }
  }
  player.pad = now;
}

async function playerMenu() {
  const d = player.dev;
  const rows = Object.entries(player.brick.buttons).map(([name, b]) => {
    const pads = Object.entries(d.pad || {}).filter(([, n]) => n === name).map(([c]) => c);
    return `<tr><td>${esc(buttonLabel(name))}</td><td>${(b.hot_keys || []).map((c) => `<kbd>${esc(keyName(c))}</kbd>`).join(' ')}${pads.length ? ' · pad ' + esc(pads.join(', ')) : ''}</td></tr>`;
  }).join('');
  const box = (id, label, key) => `<label class="field"><input type="checkbox" id="${id}" ${prefs.get(key, true) ? 'checked' : ''}><span>${label}</span></label>`;
  const body = `<p>Tap the handheld's own buttons, or use a keyboard or gamepad:</p><table class="keys">${rows}</table>
    <p style="margin-top:14px">Screen</p>
    ${box('fx-blur', 'Slow liquid crystal (segments fade in and out)', 'lcdBlur')}
    ${box('fx-ghost', 'Faint unlit segments', 'lcdGhost')}
    ${box('fx-shadow', 'Segment shadows', 'lcdShadow')}
    <p style="margin-top:14px">Time</p>
    <label class="field"><input type="checkbox" id="live" ${livesOn(d) ? 'checked' : ''}><span>Time passes while the app is closed (it catches up when you come back)</span></label>`;
  const v = await sheet(d.name, body, [{ label: 'Take the batteries out (full reset)', value: 'reset', cls: 'danger' }, { label: 'Close', value: null }], (el) => {
    const bind = (id, key, remount) => { el.querySelector('#' + id).onchange = async (e) => { prefs.set(key, e.target.checked); if (remount) mountFace(await fetch('devices/' + d.face).then((r) => r.text())); }; };
    bind('fx-blur', 'lcdBlur'); bind('fx-ghost', 'lcdGhost'); bind('fx-shadow', 'lcdShadow', true);
    el.querySelector('#live').onchange = (e) => prefs.set('live.' + d.id, e.target.checked);
  });
  if (v === 'reset') { releaseEverything(); player.m.reset(); await db.del('state:' + d.id); delete saved[d.id]; toast('Reset: scores and progress cleared'); }
  player.last = performance.now();
}

// ------------------------------------------------------------------ settings
function siteLinks() {
  const m = location.hostname.match(/^([a-z0-9-]+)\.github\.io$/i);
  const repo = location.pathname.split('/').filter(Boolean)[0];
  return { source: m && repo ? `https://github.com/${m[1]}/${repo}` : '', home: prefs.get('home', '') };
}
// Arrived from a console-picker page on the same site? Remember it so Settings can link back.
(function rememberHome() {
  try {
    const r = document.referrer && new URL(document.referrer);
    const here = new URL('.', location.href).pathname;
    if (r && r.origin === location.origin && !r.pathname.startsWith(here)) prefs.set('home', r.origin + r.pathname);
  } catch { /* no referrer */ }
})();

async function settingsMenu() {
  const { source, home } = siteLinks();
  const body = `<p>Brick games, keychain games and other LCD handhelds, running from their original chip programs in your browser. ROMs, saved games and settings are kept in this browser's storage on this device; clearing the browser's site data erases them.</p>
    <p class="about">The chip emulation, device definitions and faceplate drawings come from <a href="${esc(index.source || 'https://github.com/azya52/BrickEmuPy')}" target="_blank" rel="noopener">BrickEmuPy</a> by azya52 (public domain, CC0), ported here to C++/WebAssembly.${
      source ? ` This app is free software (GPL-3.0): <a href="${esc(source)}" target="_blank" rel="noopener">source code</a>.` : ''}</p>`;
  const acts = [];
  if (index.devices.some((d) => d.roms.some((r) => canFetch(r) && !have.has(r.file)))) acts.push({ label: 'Download ROMs from BrickEmuPy', value: 'fetch', cls: 'primary' });
  acts.push({ label: 'Import ROM files…', value: 'import' });
  if (home) acts.push({ label: '‹ All consoles', value: 'home' });
  acts.push({ label: 'Erase everything on this device', value: 'wipe', cls: 'danger' }, { label: 'Close', value: null });
  const v = await sheet('Settings', body, acts);
  if (v === 'fetch') fetchUpstream();
  if (v === 'import') $('#import-input').click();
  if (v === 'home') location.href = home;
  if (v === 'wipe') {
    const ok = await sheet('Erase all local data?', '<p>Removes the ROMs and saved games stored in this browser.</p>', [{ label: 'Erase', value: true, cls: 'danger' }, { label: 'Cancel', value: false }]);
    if (ok) { for (const k of await db.keys()) await db.del(k); location.reload(); }
  }
}

// ------------------------------------------------------------------ start
async function boot() {
  try {
    index = await fetch('devices/index.json').then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); });
  } catch (e) { $('#lib-status').textContent = 'Could not load the device list'; return; }
  await refreshStore();
  renderLibrary();
  loadCore().catch(() => {});
}

$('#btn-menu').onclick = settingsMenu;
for (const b of document.querySelectorAll('.filter button')) b.onclick = () => { prefs.set('filter', b.dataset.filter); renderLibrary(); };
$('#btn-fetch').onclick = () => fetchUpstream();
$('#import-input').onchange = async (e) => { const f = [...e.target.files]; e.target.value = ''; if (f.length) await importFiles(f); };
$('#btn-back').onclick = closePlayer;
$('#btn-pmenu').onclick = playerMenu;
$('#btn-sound').onclick = () => { player.muted = !player.muted; prefs.set('muted', player.muted); applySound(); };
window.addEventListener('keydown', (e) => onKey(e, true));
window.addEventListener('keyup', (e) => onKey(e, false));
window.addEventListener('blur', releaseEverything);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { player.hiddenAt = Date.now(); saveNow(); return; }
  player.last = performance.now();
  // the page was in the background, where animation frames stop: a pet catches up on the time it missed
  if (player.m && player.dev && player.hiddenAt && livesOn(player.dev)) catchUp(Date.now() - player.hiddenAt);
  player.hiddenAt = 0;
});
window.addEventListener('pagehide', () => { saveNow(); });
document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', (e) => { e.preventDefault(); if (!player.m && e.dataTransfer && e.dataTransfer.files.length) importFiles([...e.dataTransfer.files]); });

if ('serviceWorker' in navigator && (location.protocol === 'https:' || location.hostname === 'localhost')) {
  navigator.serviceWorker.register('sw.js').catch(() => {});
}
window.webBrick = { player, prefs };   // for tests and the browser console
boot();
