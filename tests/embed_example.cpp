// web-brick - how firmware uses the core: no heap, no files, no C++ library calls from the core.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
// This is the shape of the code for an ESP32 build (including a task on the second core of a
// Meshtastic firmware). It is compiled and run on the PC by tests/compare.py so it cannot rot:
//   g++ -std=c++11 -O2 -o embed_example tests/embed_example.cpp core/brick.cpp core/ht4bit.cpp
//   ./embed_example E23PlusMarkII96in1.bin E23PlusMarkII96in1.srom
#include "../core/brick.h"
#include "../core/ht4bit.h"
#include <cstdio>

using namespace brick;

// ---- everything the emulator needs, as static storage (about 2 KB plus the audio ring)
static Machine machine;
alignas(8) static uint8_t coreMem[sizeof(HT4Bit)];
static int16_t audioRing[2048];
static uint8_t rom[4096], soundRom[640];      // in firmware: point at the ROM in flash instead of copying
static uint8_t lcd[256];
static uint8_t snapshot[1024];

static bool setup(size_t romLen, size_t sromLen) {
  // values from the device's .brick file (web/devices/E23PlusMarkII96in1.json)
  if (!machine.create("HT943", 1000000, coreMem, sizeof coreMem)) return false;
  machine.core->setRom(0, rom, romLen);
  machine.core->setRom(1, soundRom, sromLen);
  machine.core->setOption("port_pullup.PP", 0, 15);
  machine.core->setOption("port_pullup.PM", 0, 15);
  machine.core->setOption("port_pullup.PS", 0, 15);
  machine.core->setOption("port_wakeup.PS", 0, 4);
  machine.core->setOption("timer_clock_div", 0, 16);
  machine.core->setOption("sound_freq_div", 0, 64);
  static const uint8_t speed[16] = {7, 41, 103, 31, 63, 63, 109, 31, 123, 119, 1, 37, 17, 114, 11, 11};
  static const uint8_t effect[16] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
  for (int i = 0; i < 16; i++) { machine.core->setOption("sound_speed_div", i, speed[i]); machine.core->setOption("sound_effect", i, effect[i]); }
  machine.audio.init(1000000, 16000, audioRing, 2048);   // leave this call out for a silent build
  machine.addButton("PS", 0, 0);   // 0 START
  machine.addButton("PS", 2, 0);   // 1 ON/OFF
  machine.addButton("PS", 1, 0);   // 2 MUTE
  machine.addButton("PP", 3, 0);   // 3 LEFT
  machine.addButton("PP", 1, 0);   // 4 DOWN
  machine.addButton("PP", 2, 0);   // 5 RIGHT
  machine.addButton("PP", 0, 0);   // 6 ROTATE
  machine.reset();
  return true;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: embed_example <rom> <sound rom>\n"); return 2; }
  FILE* f = fopen(argv[1], "rb"); if (!f) return 2; size_t romLen = fread(rom, 1, sizeof rom, f); fclose(f);
  f = fopen(argv[2], "rb"); if (!f) return 2; size_t sromLen = fread(soundRom, 1, sizeof soundRom, f); fclose(f);
  if (!setup(romLen, sromLen)) return 1;

  // the task loop: every 10 ms, run 10 ms of chip time (clock / 100 cycles), then look at the screen
  int16_t pcm[256]; unsigned samples = 0, lit = 0;
  for (int tick = 0; tick < 500; tick++) {                 // 5 seconds
    if (tick == 200) machine.press(0, true);               // START held for 100 ms
    if (tick == 210) machine.press(0, false);
    machine.run(1000000 / 100);
    samples += machine.audio.read(pcm, 256);               // -> I2S / DAC / buzzer
    size_t n = machine.core->vram(lcd, sizeof lcd);        // -> draw the segments whose bit is set
    lit = 0;
    for (size_t i = 0; i < n; i++) for (int b = 0; b < 4; b++) lit += (lcd[i] >> b) & 1;
  }
  size_t len = machine.saveState(snapshot, sizeof snapshot);   // -> flash; loadState() + releaseAll() to resume
  bool back = machine.loadState(snapshot, len);
  printf("machine %u + core %u bytes of RAM, %u lit segments, %u audio samples, %u byte save state, reload %s\n",
         (unsigned)sizeof(Machine), (unsigned)sizeof(HT4Bit), lit, samples, (unsigned)len, back ? "ok" : "FAILED");
  return lit > 0 && len > 0 && back ? 0 : 1;
}
