// web-brick tests - run the C++ core on the PC and print a trace in the same format as
// tests/ref_trace.py (the BrickEmuPy original), or check a save-state round trip.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
//   host_trace <machine file> <script file> <total cycles> <checkpoint every N steps> [--state-test]
#include "../core/brick.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace brick;

struct ChanState { int on; uint32_t f; int noise; int32_t amp; };
static ChanState chan[16];
static uint64_t g_cyc = 0;
static std::string g_out;
static bool g_quiet = false;

static void onAudio(void*, int ch, bool on, uint32_t f, bool noise, int32_t amp) {
  ChanState n = {on ? 1 : 0, on ? f : 0, on ? (int)noise : 0, on ? amp : 0};
  ChanState& c = chan[ch & 15];
  if (c.on == n.on && c.f == n.f && c.noise == n.noise && c.amp == n.amp) return;
  c = n;
  if (g_quiet) return;
  char b[128];
  snprintf(b, sizeof b, "A %llu %d %d %u %d %d\n", (unsigned long long)g_cyc, ch, n.on, n.f, n.noise, n.amp);
  g_out += b;
}

static std::vector<uint8_t> readFile(const char* p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
  uint8_t buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  fclose(f);
  return v;
}

struct Rig {
  Machine m;
  std::vector<uint8_t> rom, srom, mem;
  int16_t ring[4096];
  bool build(const char* machineFile) {
    FILE* f = fopen(machineFile, "r");
    if (!f) return false;
    char line[1024], core[64] = "", a[512];
    unsigned clock = 0;
    std::vector<std::string> lines;
    while (fgets(line, sizeof line, f)) lines.push_back(line);
    fclose(f);
    for (auto& l : lines) { if (sscanf(l.c_str(), "core %63s", core) == 1) {} else sscanf(l.c_str(), "clock %u", &clock); }
    mem.resize(coreSize(core) + 64);
    void* p = (void*)(((uintptr_t)mem.data() + 15) & ~(uintptr_t)15);
    if (!m.create(core, clock, p, coreSize(core))) { fprintf(stderr, "unknown core %s\n", core); return false; }
    m.audio.onEvent = onAudio;
    m.audio.init(clock, 16000, ring, 4096);
    for (auto& l : lines) {
      char key[128]; int idx, val, pin, level;
      if (sscanf(l.c_str(), "rom %511[^\n]", a) == 1) { rom = readFile(a); m.core->setRom(0, rom.data(), rom.size()); }
      else if (sscanf(l.c_str(), "srom %511[^\n]", a) == 1) { srom = readFile(a); m.core->setRom(1, srom.data(), srom.size()); }
      else if (sscanf(l.c_str(), "opt %127s %d %d", key, &idx, &val) == 3) m.core->setOption(key, idx, val);
      else if (sscanf(l.c_str(), "button %127s %d %d", key, &pin, &level) == 3) {
        if (m.addButton(key, pin, level) < 0) { fprintf(stderr, "bad button port %s\n", key); return false; }
      }
    }
    m.reset();
    return true;
  }
};

struct Ev { uint64_t cyc; int btn; int down; };

static uint32_t stateHash(Machine& m) {
  static uint32_t regs[1024]; static uint8_t vram[1024];
  uint32_t h = 2166136261u;
  size_t n = m.core->debugRegs(regs, 1024);
  for (size_t i = 0; i < n; i++) h = (h ^ regs[i]) * 16777619u;
  size_t v = m.core->vram(vram, 1024);
  for (size_t i = 0; i < v; i++) h = (h ^ vram[i]) * 16777619u;
  return h;
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: host_trace <machine> <script> <cycles> <every> [--state-test]\n"); return 2; }
  const uint64_t total = strtoull(argv[3], nullptr, 10);
  const uint32_t every = (uint32_t)atoi(argv[4]);
  const bool stateTest = argc > 5 && !strcmp(argv[5], "--state-test");

  std::vector<Ev> evs;
  { FILE* f = fopen(argv[2], "r"); if (!f) return 2; unsigned long long c; int b, d; while (fscanf(f, "%llu %d %d", &c, &b, &d) == 3) evs.push_back({c, b, d}); fclose(f); }

  Rig* rig = new Rig();
  if (!rig->build(argv[1])) return 2;
  Rig* twin = nullptr;                      // --state-test: a second machine restored from snapshots must stay identical
  std::vector<uint8_t> snap;
  int restores = 0;

  size_t ei = 0; uint64_t cyc = 0; uint32_t step = 0, flow = 2166136261u;
  char b[128];
  while (cyc < total) {
    while (ei < evs.size() && evs[ei].cyc <= cyc) { rig->m.press(evs[ei].btn, evs[ei].down); ei++; }
    g_cyc = cyc;
    int k = rig->m.core->step();
    rig->m.audio.advance((uint32_t)k);
    cyc += (uint64_t)k; step++;
    flow = (flow ^ rig->m.core->pc()) * 16777619u;
    flow = (flow ^ (uint32_t)cyc) * 16777619u;
    if (step % every == 0) {
      snprintf(b, sizeof b, "C %u %llu %08x %08x\n", step, (unsigned long long)cyc, flow, stateHash(rig->m));
      g_out += b;
      if (stateTest) {
        // every checkpoint: snapshot, throw the machine away, build a fresh one, restore, and carry on with it
        snap.resize(rig->m.saveState(nullptr, 0));
        if (!snap.size() || rig->m.saveState(snap.data(), snap.size()) != snap.size()) { fprintf(stderr, "saveState failed\n"); return 1; }
        ChanState keep[16]; memcpy(keep, chan, sizeof chan);
        g_quiet = true;
        twin = new Rig();
        if (!twin->build(argv[1])) return 2;
        if (!twin->m.loadState(snap.data(), snap.size())) { fprintf(stderr, "loadState failed\n"); return 1; }
        g_quiet = false;
        memcpy(chan, keep, sizeof chan);
        if (stateHash(twin->m) != stateHash(rig->m)) { fprintf(stderr, "state differs right after load at step %u\n", step); return 1; }
        delete rig; rig = twin; twin = nullptr; restores++;
      }
    }
  }
  fwrite(g_out.data(), 1, g_out.size(), stdout);
  if (stateTest) fprintf(stderr, "state test: %d save/load round trips\n", restores);
  return 0;
}
