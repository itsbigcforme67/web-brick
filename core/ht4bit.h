// Holtek 4-bit LCD microcontrollers: HT943 (brick games, keychain games) and HTG12N0.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Port of BrickEmuPy cores/HT4BIT.py, HT943.py, HTG12N0.py, HT4BITsound.py (azya52, CC0).
#pragma once
#include "brick.h"

namespace brick {

class HT4Bit : public Core {
 public:
  enum Variant { HT943, HTG12N0 };
  enum Port { PORT_PP, PORT_PS, PORT_PM, PORT_RES };
  explicit HT4Bit(Variant v);

  void setRom(int which, const uint8_t* data, size_t len) override;
  bool setOption(const char* key, int index, int32_t value) override;
  void reset() override;
  int step() override;
  int portId(const char* name) const override;
  void setPin(int port, int pin, int level) override;
  size_t vram(uint8_t* out, size_t cap) const override;
  void sync(State& s) override;
  uint32_t pc() const override { return pc_; }
  size_t debugRegs(uint32_t* out, size_t cap) const override;

 private:
  enum { SROM_SIZE = 640, VRAM_SIZE = 128 };
  Variant variant_;

  // configuration (mask options)
  const uint8_t* rom_ = nullptr;
  uint32_t romSize_ = 1;
  uint8_t srom_[SROM_SIZE];
  uint8_t pullup_[3] = {0, 0, 0}, wakeup_[3] = {0, 0, 0};       // indexed by Port
  uint32_t timerDiv_ = 16, soundFreqDiv_ = 64, rtcClockDiv_ = 0;
  uint8_t speedDiv_[16], effect_[16];

  // CPU
  uint32_t pc_ = 0, stack_ = 0, instr_ = 0;
  uint8_t acc_ = 0, wr_[5], ei_ = 0, cf_ = 0, tf_ = 0, ef_ = 0, halt_ = 0, resetLine_ = 0, timerOn_ = 0, tc_ = 0;
  int32_t timerCounter_ = 0;
  uint8_t ram_[256], lcd_[VRAM_SIZE];
  uint8_t pa_ = 0, pb_ = 0, pcPort_ = 0, port_[3];               // port_[]: input pins PP, PS, PM
  int64_t rtcCounter_ = 0;                                       // HTG12N0 only, in 1/32768 clock cycles

  // sound generator
  int32_t sndCounter_ = 0;
  uint32_t sndNote_ = 0;
  uint8_t sndChannel_ = 0, sndRepeat_ = 0, sndOn_ = 0;

  void coreReset();
  uint8_t fetch(uint32_t a) const { return rom_[a % romSize_]; }
  uint32_t ramIndex(int rp) const { return ((uint32_t)wr_[rp + 1] << 4) | wr_[rp]; }
  uint8_t readRam(int rp) const;
  void writeRam(int rp, uint8_t v);
  void interrupt(uint32_t location);
  void soundClock(int cycles);
  void soundOff();
  void soundChannel(uint8_t ch);
};

}  // namespace brick
