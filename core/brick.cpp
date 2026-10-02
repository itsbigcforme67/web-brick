// web-brick core - sound synthesis, machine wrapper, core registry.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
#include "brick.h"
#include "ht4bit.h"

namespace brick {

bool strEq(const char* a, const char* b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

// ------------------------------------------------------------------------------------------ Audio
static const int16_t SINE[256] = {
#include "sine_table.inc"
};

void Audio::init(uint32_t clockHz, uint32_t sampleRate, int16_t* ring, uint32_t ringSize) {
  clock_ = clockHz; rate_ = sampleRate; ring_ = ring; ringSize_ = ring ? ringSize : 0;
  acc_ = 0; w_ = 0; r_ = 0;
  for (int i = 0; i < CHANNELS; i++) {
    Chan& c = ch_[i];
    if (c.on && rate_) c.inc = (uint32_t)(((uint64_t)c.freqQ8 << 24) / rate_);
  }
}

void Audio::tone(int ch, uint32_t freqQ8, bool noise, int32_t ampQ8) {
  if (onEvent) onEvent(eventCtx, ch, true, freqQ8, noise, ampQ8);
  if (ch < 0 || ch >= CHANNELS) return;
  Chan& c = ch_[ch];
  c.freqQ8 = freqQ8; c.noise = noise; c.amp = ampQ8; c.on = 1;
  if (!c.sign) c.sign = 1;
  c.inc = rate_ ? (uint32_t)(((uint64_t)freqQ8 << 24) / rate_) : 0;
}

void Audio::stop(int ch) {
  if (onEvent) onEvent(eventCtx, ch, false, 0, false, 0);
  if (ch < 0 || ch >= CHANNELS) return;
  ch_[ch].on = 0; ch_[ch].phase = 0; ch_[ch].sign = 1;
}

int16_t Audio::sample() {
  int32_t mix = 0;
  for (int i = 0; i < CHANNELS; i++) {
    Chan& c = ch_[i];
    if (!c.on) continue;
    int32_t v;
    if (c.freqQ8 == 0) {
      v = c.amp * 128;                                   // constant level (pin held high/low)
    } else {
      v = (SINE[c.phase >> 24] * c.amp) >> 8;
      uint32_t next = c.phase + c.inc;
      if (c.noise && ((next ^ c.phase) & 0x80000000u)) {   // every half wave: random polarity
        lfsr_ ^= lfsr_ << 13; lfsr_ ^= lfsr_ >> 17; lfsr_ ^= lfsr_ << 5;
        c.sign = (lfsr_ & 1) ? 1 : -1;
      }
      if (c.noise) v *= c.sign;
      c.phase = next;
    }
    mix += v;
  }
  if (mix > 32767) mix = 32767; else if (mix < -32767) mix = -32767;
  return (int16_t)((mix * gain) >> 15);
}

void Audio::advance(uint32_t cycles) {
  if (!rate_) return;
  acc_ += cycles * rate_;
  while (acc_ >= clock_) {
    acc_ -= clock_;
    int16_t s = sample();
    if (ringSize_) {
      uint32_t nw = (w_ + 1) % ringSize_;
      if (nw != r_) { ring_[w_] = s; w_ = nw; }           // ring full: drop (host fell behind)
    }
  }
}

uint32_t Audio::read(int16_t* dst, uint32_t max) {
  uint32_t n = 0, r = r_;
  while (n < max && r != w_) { dst[n++] = ring_[r]; r = (r + 1) % ringSize_; }
  r_ = r;
  return n;
}

void Audio::sync(State& s) {
  for (int i = 0; i < CHANNELS; i++) {
    Chan& c = ch_[i];
    s.v(c.phase); s.v(c.amp); s.v(c.freqQ8); s.v(c.on); s.v(c.noise); s.v(c.sign);
    if (s.loading()) c.inc = rate_ ? (uint32_t)(((uint64_t)c.freqQ8 << 24) / rate_) : 0;
  }
  s.v(lfsr_);
  if (s.loading()) acc_ = 0;
}

// ------------------------------------------------------------------------------------------ cores
size_t coreSize(const char* name) {
  if (strEq(name, "HT943") || strEq(name, "HTG12N0")) return sizeof(HT4Bit);
  return 0;
}

Core* createCore(const char* name, void* mem, size_t memSize) {
  size_t need = coreSize(name);
  if (!need || memSize < need || ((uintptr_t)mem & (alignof(HT4Bit) - 1))) return nullptr;
  if (strEq(name, "HT943")) return new (PlaceTag(), mem) HT4Bit(HT4Bit::HT943);
  if (strEq(name, "HTG12N0")) return new (PlaceTag(), mem) HT4Bit(HT4Bit::HTG12N0);
  return nullptr;
}

// ------------------------------------------------------------------------------------------ Machine
bool Machine::create(const char* coreName, uint32_t clockHz, void* coreMem, size_t coreMemSize) {
  core = createCore(coreName, coreMem, coreMemSize);
  if (!core) return false;
  core->audio = &audio;
  core->clockHz = clockHz;
  tag_ = fnv1a(coreName);
  nBtn_ = 0; debt_ = 0; cycles = 0;
  audio.init(clockHz, 0, nullptr, 0);
  for (int i = 0; i < Audio::CHANNELS; i++) audio.stop(i);
  return true;
}

int Machine::addButton(const char* port, int pin, int level) {
  if (!core || nBtn_ >= MAX_BUTTONS) return -1;
  int p = core->portId(port);
  if (p < 0) return -1;
  btn_[nBtn_].port = (int8_t)p; btn_[nBtn_].pin = (uint8_t)pin; btn_[nBtn_].level = (int8_t)level; btn_[nBtn_].down = 0;
  return nBtn_++;
}

void Machine::press(int i, bool down) {
  if (!core || i < 0 || i >= nBtn_) return;
  Btn& b = btn_[i];
  b.down = down;
  core->setPin(b.port, b.pin, down ? b.level : -1);
}

void Machine::releaseAll() { for (int i = 0; i < nBtn_; i++) if (btn_[i].down) press(i, false); }

void Machine::reset() {
  if (!core) return;
  for (int i = 0; i < Audio::CHANNELS; i++) audio.stop(i);
  core->reset();
  for (int i = 0; i < nBtn_; i++) btn_[i].down = 0;
  debt_ = 0; cycles = 0;
}

uint32_t Machine::run(uint32_t budget) {
  if (!core) return 0;
  int32_t left = (int32_t)budget + debt_;
  uint32_t ran = 0;
  while (left > 0) {
    int c = core->step();
    left -= c; ran += (uint32_t)c;
    audio.advance((uint32_t)c);
  }
  debt_ = left;                                           // overshoot is paid back next time
  cycles += ran;
  return ran;
}

int Machine::step() {
  if (!core) return 0;
  int c = core->step();
  audio.advance((uint32_t)c);
  cycles += (uint32_t)c;
  return c;
}

size_t Machine::syncAll(State& s) {
  uint32_t magic = 0x314B5242u /* "BRK1" */, version = STATE_VERSION, tag = tag_;
  s.v(magic); s.v(version); s.v(tag);
  if (s.loading() && (magic != 0x314B5242u || version != STATE_VERSION || tag != tag_)) return 0;
  s.v(cycles); s.v(debt_);
  uint32_t nb = (uint32_t)nBtn_;
  s.v(nb);
  if (s.loading() && nb != (uint32_t)nBtn_) return 0;
  for (int i = 0; i < nBtn_; i++) s.v(btn_[i].down);
  core->sync(s);
  audio.sync(s);
  return s.ok() ? s.size() : 0;
}

size_t Machine::saveState(uint8_t* out, size_t cap) {
  if (!core) return 0;
  State s(out ? State::Save : State::Measure, out, cap);
  return syncAll(s);
}

bool Machine::loadState(const uint8_t* in, size_t len) {
  // Restores the machine exactly, held buttons included. A host resuming a saved game should
  // call releaseAll() afterwards, since nobody is holding a button any more.
  if (!core) return false;
  State s(State::Load, const_cast<uint8_t*>(in), len);
  return syncAll(s) != 0;
}

}  // namespace brick
