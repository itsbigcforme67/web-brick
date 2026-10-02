// web-brick tests - run the built web/core/brick.wasm in Node and print checkpoints in the same
// format as tests/ref_trace.py, to prove the WebAssembly build behaves like the original.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
//   node tests/wasm_trace.mjs <BrickEmuPy dir> <device> <script file> <total cycles> <every N steps>
import { readFileSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compileCore, Machine, directButtons } from '../web/core/brick.js';

const [brickemu, device, script, totalS, everyS] = process.argv.slice(2);
const here = dirname(fileURLToPath(import.meta.url));
const brick = JSON.parse(readFileSync(join(brickemu, 'assets', device + '.brick'), 'utf8'));
const file = (p) => new Uint8Array(readFileSync(resolve(brickemu, p)));
const mod = await compileCore(readFileSync(join(here, '..', 'web', 'core', 'brick.wasm')));
const m = await Machine.create(mod, { brick, rom: file(brick.mask_options.rom_path), soundRom: brick.mask_options.sound_rom_path ? file(brick.mask_options.sound_rom_path) : null, sampleRate: 16000 });
const names = directButtons(brick).map((b) => b.name);
const events = readFileSync(script, 'utf8').trim().split('\n').filter(Boolean).map((l) => l.split(' ').map(Number));
const total = Number(totalS), every = Number(everyS);
let ei = 0, cyc = 0, step = 0, flow = 2166136261;
const out = [];
while (cyc < total) {
  while (ei < events.length && events[ei][0] <= cyc) { m.press(names[events[ei][1]], events[ei][2] === 1); ei++; }
  const k = m.step();
  cyc += k; step++;
  flow = Math.imul(flow ^ m.pc, 16777619) >>> 0;
  flow = Math.imul(flow ^ k, 16777619) >>> 0;
  if (step % every === 0) {
    out.push(`C ${step} ${cyc} ${flow.toString(16).padStart(8, '0')} ${m.stateHash().toString(16).padStart(8, '0')}`);
    m.readAudio();
  }
}
process.stdout.write(out.join('\n') + '\n');
