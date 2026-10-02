// web-brick - Copyright (C) 2026 the web-brick authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// JavaScript wrapper around core/brick.wasm (the C++ cores in ../../core, built by build.sh).
// No DOM use: works in browsers and in Node (tests/).

let compiled;
/** Fetch and compile brick.wasm once. */
export function loadCore(url = new URL('./brick.wasm', import.meta.url)) {
  if (!compiled) compiled = fetch(url).then((r) => { if (!r.ok) throw new Error('brick.wasm: ' + r.status); return r.arrayBuffer(); }).then((b) => WebAssembly.compile(b));
  return compiled;
}
export const compileCore = (bytes) => WebAssembly.compile(bytes);

/** Mask options of a .brick file as [key, index, value]: nested objects become "outer.inner", arrays are indexed. */
export function flattenOptions(mask) {
  const out = [];
  const put = (key, v) => {
    if (typeof v === 'boolean') out.push([key, 0, v ? 1 : 0]);
    else if (Number.isInteger(v)) out.push([key, 0, v]);
    else if (Array.isArray(v)) v.forEach((x, i) => { if (Number.isInteger(x)) out.push([key, i, x]); });
    else if (v && typeof v === 'object') for (const k of Object.keys(v)) put(key + '.' + k, v[k]);
  };
  for (const k of Object.keys(mask || {})) put(k, mask[k]);
  return out;
}

/** Buttons of a .brick file that are wired straight to a pin: [{ name, port, pin, level }]. */
export function directButtons(brick) {
  const direct = (brick.peripherals && brick.peripherals.direct_input) || {};
  return Object.keys(brick.buttons || {}).filter((n) => direct[n]).map((n) => ({ name: n, port: direct[n].port, pin: direct[n].mask, level: direct[n].level }));
}

export class Machine {
  /**
   * @param module compiled brick.wasm (loadCore / compileCore)
   * @param cfg { brick: parsed .brick file, rom: Uint8Array, soundRom?: Uint8Array, sampleRate?: number, gain?: number }
   */
  static async create(module, cfg) {
    const inst = await WebAssembly.instantiate(module, {});
    return new Machine(inst.exports, cfg);
  }

  constructor(x, cfg) {
    this.x = x;
    const brick = cfg.brick;
    this.clock = brick.clock;
    x.brick_init();
    const mem = () => new Uint8Array(x.memory.buffer);
    const alloc = (n) => { const p = x.brick_alloc(n); if (!p) throw new Error('emulator memory is full'); return p; };
    const str = (s) => { const p = alloc(s.length + 1); const m = mem(); for (let i = 0; i < s.length; i++) m[p + i] = s.charCodeAt(i) & 0x7F; m[p + s.length] = 0; return p; };
    const blob = (b) => { const p = alloc(b.length); mem().set(b, p); return p; };

    if (!x.brick_create(str(brick.core), brick.clock)) throw new Error(`the ${brick.core} chip is not supported yet`);
    x.brick_set_rom(0, blob(cfg.rom), cfg.rom.length);
    if (cfg.soundRom) x.brick_set_rom(1, blob(cfg.soundRom), cfg.soundRom.length);
    for (const [k, i, v] of flattenOptions(brick.mask_options)) x.brick_set_option(str(k), i, v);
    this.buttons = {};
    for (const b of directButtons(brick)) {
      const idx = x.brick_add_button(str(b.port), b.pin, b.level);
      if (idx >= 0) this.buttons[b.name] = idx;
    }
    this.vramPtr = alloc(1024);
    this.audioPtr = alloc(4096 * 2);
    x.brick_audio(cfg.sampleRate || 0, cfg.gain || 8192);
    x.brick_reset();
    this.stateSize = x.brick_state_save(0, 0);
    this.statePtr = alloc(this.stateSize + 64);
    this.cycleFrac = 0;
  }

  reset() { this.x.brick_reset(); }
  /** Press or release a button by its .brick name (btnLeft, btnStart, ...). */
  press(name, down) { const i = this.buttons[name]; if (i !== undefined) this.x.brick_press(i, down ? 1 : 0); }
  releaseAll() { this.x.brick_release_all(); }
  /** Run the chip for this many milliseconds of its own time. */
  runMs(ms) {
    const c = ms * this.clock / 1000 + this.cycleFrac;
    const whole = Math.floor(c);
    this.cycleFrac = c - whole;
    if (whole > 0) this.x.brick_run(whole);
  }
  run(cycles) { return this.x.brick_run(cycles); }
  step() { return this.x.brick_step(); }
  get pc() { return this.x.brick_pc() >>> 0; }
  stateHash() { return this.x.brick_state_hash() >>> 0; }
  /** LCD segment data: segment "<i>_<bit>" of the faceplate is lit when bit <bit> of byte <i> is set. */
  vram() { const n = this.x.brick_vram(this.vramPtr, 1024); return new Uint8Array(this.x.memory.buffer, this.vramPtr, n); }
  /** Sound produced since the last call: Int16Array view, valid until the next call. */
  readAudio() { const n = this.x.brick_audio_read(this.audioPtr, 4096); return new Int16Array(this.x.memory.buffer, this.audioPtr, n); }
  saveState() {
    const n = this.x.brick_state_save(this.statePtr, this.stateSize + 64);
    return n ? new Uint8Array(this.x.memory.buffer, this.statePtr, n).slice() : null;
  }
  /** Restore a snapshot. Buttons held when it was taken are released. Returns false if it does not fit this device. */
  loadState(bytes) {
    if (!bytes || bytes.length > this.stateSize + 64) return false;
    new Uint8Array(this.x.memory.buffer).set(bytes, this.statePtr);
    if (!this.x.brick_state_load(this.statePtr, bytes.length)) return false;
    this.x.brick_release_all();
    return true;
  }
}
