// web-brick core - shared types.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Ported from BrickEmuPy by azya52 (CC0), https://github.com/azya52/BrickEmuPy
//
// Portable C++11 with no heap, no libc and no floating point, so the same source builds as
// WebAssembly for the website and as a library inside ESP32 firmware (including a task on the
// second core of a Meshtastic build). Everything a machine needs lives in memory the host hands
// over; nothing here allocates or keeps global state.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brick { struct PlaceTag {}; }
// placement new without <new> (the WebAssembly build has no C++ library)
inline void* operator new(size_t, brick::PlaceTag, void* p) noexcept { return p; }

namespace brick {

bool strEq(const char* a, const char* b);
uint32_t fnv1a(const char* s);

// ---- save states: one sync() function per object both saves and loads, so they cannot drift apart
class State {
 public:
  enum Mode { Measure, Save, Load };
  State(Mode m, uint8_t* buf, size_t cap) : mode_(m), buf_(buf), cap_(cap), n_(0), ok_(true) {}
  template <typename T> void v(T& x) {
    uint64_t u = mode_ == Load ? 0 : (uint64_t)x;
    for (size_t i = 0; i < sizeof(T); i++) {
      if (mode_ == Load) { if (n_ < cap_) u |= (uint64_t)buf_[n_] << (8 * i); else ok_ = false; }
      else if (mode_ == Save) { if (n_ < cap_) buf_[n_] = (uint8_t)(u >> (8 * i)); else ok_ = false; }
      n_++;
    }
    if (mode_ == Load) x = (T)u;
  }
  void b(bool& x) { uint8_t t = x; v(t); x = t != 0; }
  void bytes(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) v(p[i]); }
  bool loading() const { return mode_ == Load; }
  size_t size() const { return n_; }
  bool ok() const { return ok_; }
 private:
  Mode mode_; uint8_t* buf_; size_t cap_, n_; bool ok_;
};

// ---- sound: the chips describe sound as "this channel now plays frequency f" (or holds a level);
// this turns those events into 16-bit samples in a ring buffer the host drains.
class Audio {
 public:
  enum { CHANNELS = 4, AMP_ONE = 256 };
  void init(uint32_t clockHz, uint32_t sampleRate, int16_t* ring, uint32_t ringSize);
  // freqQ8 = Hz * 256 (0 = hold a constant level); ampQ8 = amplitude * 256 (1.0 = full swing;
  // above 1.0 the sine is clipped, which squares it off, as in BrickEmuPy).
  void tone(int ch, uint32_t freqQ8, bool noise, int32_t ampQ8);
  void stop(int ch);
  void advance(uint32_t cycles);
  uint32_t available() const { return ringSize_ ? (w_ - r_ + ringSize_) % ringSize_ : 0; }
  uint32_t read(int16_t* dst, uint32_t max);
  void sync(State& s);
  int32_t gain = 8192;           // output level of a full-swing signal (of 32767)
  // optional hook for tests: called on every tone()/stop()
  void (*onEvent)(void* ctx, int ch, bool on, uint32_t freqQ8, bool noise, int32_t ampQ8) = nullptr;
  void* eventCtx = nullptr;
 private:
  struct Chan { uint32_t inc = 0, phase = 0; int32_t amp = 0; uint32_t freqQ8 = 0; uint8_t on = 0, noise = 0; int8_t sign = 1; };
  Chan ch_[CHANNELS];
  uint32_t clock_ = 0, rate_ = 0, acc_ = 0, lfsr_ = 0x1234567;
  int16_t* ring_ = nullptr; uint32_t ringSize_ = 0, w_ = 0; volatile uint32_t r_ = 0;
  int16_t sample();
};

// ---- one emulated chip
class Core {
 public:
  Audio* audio = nullptr;
  uint32_t clockHz = 0;
  virtual void setRom(int which, const uint8_t* data, size_t len) = 0;   // 0 = program (not copied), 1 = sound ROM (copied)
  virtual bool setOption(const char* key, int index, int32_t value) = 0; // mask options from the .brick file
  virtual void reset() = 0;
  virtual int step() = 0;                                                // run one instruction, return clock cycles used
  virtual int portId(const char* name) const = 0;                        // -1 if the chip has no such port
  virtual void setPin(int port, int pin, int level) = 0;                 // level < 0 releases the pin
  virtual size_t vram(uint8_t* out, size_t cap) const = 0;               // LCD segment bits as the faceplate indexes them
  virtual void sync(State& s) = 0;
  virtual uint32_t pc() const = 0;
  virtual size_t debugRegs(uint32_t* out, size_t cap) const = 0;         // for trace comparison with BrickEmuPy
};

size_t coreSize(const char* name);                                      // bytes of storage create() needs, 0 = unknown core
Core* createCore(const char* name, void* mem, size_t memSize);

// ---- a whole device: chip + buttons + sound + timekeeping
class Machine {
 public:
  enum { MAX_BUTTONS = 24, STATE_VERSION = 1 };
  Core* core = nullptr;
  Audio audio;
  uint64_t cycles = 0;

  bool create(const char* coreName, uint32_t clockHz, void* coreMem, size_t coreMemSize);
  int addButton(const char* port, int pin, int level);                   // returns the button index, -1 on error
  void press(int button, bool down);
  void releaseAll();
  void reset();
  uint32_t run(uint32_t cycleBudget);                                    // returns cycles actually run
  int step();                                                            // exactly one instruction (tests, debuggers)
  size_t saveState(uint8_t* out, size_t cap);                            // out = nullptr measures; returns bytes, 0 on error
  bool loadState(const uint8_t* in, size_t len);
 private:
  struct Btn { int8_t port = 0; uint8_t pin = 0; int8_t level = 0; uint8_t down = 0; };
  Btn btn_[MAX_BUTTONS];
  int nBtn_ = 0;
  int32_t debt_ = 0;
  uint32_t tag_ = 0;
  size_t syncAll(State& s);
};

}  // namespace brick
