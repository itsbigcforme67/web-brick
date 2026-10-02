// web-brick core - WebAssembly interface (one machine per module instance).
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Built with plain clang (no Emscripten, no C library); see build.sh. web/core/brick.js wraps it.
#include "brick.h"

using namespace brick;

#define EXPORT(name) __attribute__((export_name(#name))) name

extern "C" {
// the compiler may emit calls to these even though nothing here includes a C library
void* memset(void* d, int c, size_t n) { uint8_t* p = (uint8_t*)d; while (n--) *p++ = (uint8_t)c; return d; }
void* memcpy(void* d, const void* s, size_t n) { uint8_t* p = (uint8_t*)d; const uint8_t* q = (const uint8_t*)s; while (n--) *p++ = *q++; return d; }
void* memmove(void* d, const void* s, size_t n) {
  uint8_t* p = (uint8_t*)d; const uint8_t* q = (const uint8_t*)s;
  if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}
void __cxa_pure_virtual() { __builtin_trap(); }
}

static const uint32_t ARENA_SIZE = 256 * 1024, CORE_MEM_SIZE = 16 * 1024, RING_SIZE = 16384;
alignas(16) static uint8_t g_arena[ARENA_SIZE];
alignas(16) static uint8_t g_coreMem[CORE_MEM_SIZE];
alignas(16) static uint8_t g_machineMem[sizeof(Machine)];
static int16_t g_ring[RING_SIZE];
static uint32_t g_arenaUsed = 0;
static Machine* g_m = nullptr;
static uint32_t g_clock = 0;

extern "C" {

// Must be called first. Returns the interface version.
int EXPORT(brick_init)() {
  g_arenaUsed = 0;
  g_m = new (PlaceTag(), g_machineMem) Machine();
  return 1;
}

// Scratch memory for passing ROMs, strings and save states in. Lives until the next brick_init().
void* EXPORT(brick_alloc)(uint32_t n) {
  n = (n + 7) & ~7u;
  if (g_arenaUsed + n > ARENA_SIZE) return nullptr;
  void* p = g_arena + g_arenaUsed;
  g_arenaUsed += n;
  return p;
}

int EXPORT(brick_create)(const char* core, uint32_t clockHz) {
  if (!g_m || coreSize(core) > CORE_MEM_SIZE) return 0;
  g_clock = clockHz;
  return g_m->create(core, clockHz, g_coreMem, CORE_MEM_SIZE) ? 1 : 0;
}

void EXPORT(brick_set_rom)(int which, const uint8_t* data, uint32_t len) { if (g_m && g_m->core) g_m->core->setRom(which, data, len); }
int EXPORT(brick_set_option)(const char* key, int index, int value) { return g_m && g_m->core && g_m->core->setOption(key, index, value); }
int EXPORT(brick_add_button)(const char* port, int pin, int level) { return g_m ? g_m->addButton(port, pin, level) : -1; }
void EXPORT(brick_audio)(uint32_t sampleRate, int gain) { if (g_m) { g_m->audio.gain = gain; g_m->audio.init(g_clock, sampleRate, g_ring, RING_SIZE); } }
void EXPORT(brick_reset)() { if (g_m) g_m->reset(); }
void EXPORT(brick_press)(int button, int down) { if (g_m) g_m->press(button, down != 0); }
void EXPORT(brick_release_all)() { if (g_m) g_m->releaseAll(); }
uint32_t EXPORT(brick_run)(uint32_t cycles) { return g_m ? g_m->run(cycles) : 0; }
uint32_t EXPORT(brick_vram)(uint8_t* out, uint32_t cap) { return g_m && g_m->core ? (uint32_t)g_m->core->vram(out, cap) : 0; }
uint32_t EXPORT(brick_audio_read)(int16_t* out, uint32_t max) { return g_m ? g_m->audio.read(out, max) : 0; }
uint32_t EXPORT(brick_state_save)(uint8_t* out, uint32_t cap) { return g_m ? (uint32_t)g_m->saveState(out, cap) : 0; }
int EXPORT(brick_state_load)(const uint8_t* in, uint32_t len) { return g_m && g_m->loadState(in, len); }
int EXPORT(brick_step)() { return g_m ? g_m->step() : 0; }

// FNV-1a over the chip's registers, RAM and LCD: the same number tests/ref_trace.py prints for BrickEmuPy.
uint32_t EXPORT(brick_state_hash)() {
  if (!g_m || !g_m->core) return 0;
  static uint32_t regs[1024]; static uint8_t lcd[1024];
  uint32_t h = 2166136261u;
  size_t n = g_m->core->debugRegs(regs, 1024);
  for (size_t i = 0; i < n && i < 1024; i++) h = (h ^ regs[i]) * 16777619u;
  size_t v = g_m->core->vram(lcd, 1024);
  for (size_t i = 0; i < v; i++) h = (h ^ lcd[i]) * 16777619u;
  return h;
}

uint32_t EXPORT(brick_pc)() { return g_m && g_m->core ? g_m->core->pc() : 0; }

}  // extern "C"
