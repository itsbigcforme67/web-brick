// Seiko Epson E0C6200 4-bit microcontrollers: Tamagotchi, Digimon and other Bandai pets, Alien Fever.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Port of BrickEmuPy cores/E0C6200.py and E0C6200sound.py (azya52, CC0).
#pragma once
#include "brick.h"

namespace brick {

class E0C6200 : public Core {
 public:
  enum Port { K0, K1, R0, R1, R2, R3, R4, P0, P1, P2, P3, PORT_COUNT, RES = PORT_COUNT };
  E0C6200();

  void setRom(int which, const uint8_t* data, size_t len) override;
  bool setOption(const char* key, int index, int32_t value) override;
  void reset() override;
  int step() override;
  int portId(const char* name) const override;
  void setPin(int port, int mask, int level) override;                  // here "pin" is a bit mask, as in the .brick file
  size_t vram(uint8_t* out, size_t cap) const override;
  void sync(State& s) override;
  uint32_t pc() const override { return pc_ & 0x1FFF; }
  size_t debugRegs(uint32_t* out, size_t cap) const override;

 private:
  enum { RAM_SIZE = 0x300, VRAM_SIZE = 0x0A0 };

  // configuration (mask options)
  const uint8_t* rom_ = nullptr;
  uint32_t romSize_ = 2;
  uint8_t pullupExt_[PORT_COUNT], pushpullMask_[PORT_COUNT], pushpullSet_[PORT_COUNT];

  // CPU
  uint32_t pc_ = 0, npc_ = 0, ix_ = 0, iy_ = 0, instr_ = 0;
  uint8_t a_ = 0, b_ = 0, sp_ = 0, cf_ = 0, zf_ = 0, df_ = 0, if_ = 0, halt_ = 0, ifDelay_ = 0, resetLine_ = 0;
  uint8_t ram_[RAM_SIZE], vram_[VRAM_SIZE];

  // ports: input levels driven from outside (low/high masks), direction, latch, pull-ups
  uint8_t inLo_[PORT_COUNT], inHi_[PORT_COUNT], dir_[PORT_COUNT], pushpull_[PORT_COUNT], latch_[PORT_COUNT], pullup_[PORT_COUNT];

  // I/O registers
  uint8_t it_, isw_, ipt_, isio_, ik0_, ik1_, eit_, eisw_, eipt_, eisio_, eik0_, eik1_;
  uint8_t tm_, swl_, swh_, pt_, rd_, sd_, dfk0_;
  uint8_t ctrlOsc_, ctrlLcd_, lc_, ctrlSvd_, ctrlBz1_, ctrlBz2_, ctrlSw_, ctrlPt_, ptc_, sc_, hzr_, ioc_, pup_;

  // time: the OSC1 counter is kept in 1/32768ths of a machine cycle so it stays exact
  int64_t osc1Counter_ = 0;
  int32_t timerCounter_ = 0, ptimerCounter_ = 0;
  int64_t stopwatchCounter_ = 0;                                         // in 2^-44 units (see e0c6200.cpp)
  uint32_t cycleFrac_ = 0;                                               // machine cycles not yet returned, 1/32768ths

  // buzzer
  uint32_t bzFreqDiv_ = 8, envCycle_ = 0;
  int32_t oneShot_ = 0, envCounter_ = 0;
  uint8_t envStep_ = 7, envOn_ = 0, soundOn_ = 0;

  void initRegisters();
  void partialReset();
  uint8_t portRead(int p) const;
  uint8_t getMem(uint32_t addr);
  void setMem(uint32_t addr, uint8_t v);
  uint8_t getR(int r);
  void setR(int r, uint8_t v);
  int interrupt(uint32_t vector);
  void clockOsc1();
  void processPtimer();
  int execute(uint32_t op);
  void setPdir(uint8_t ioc);
  void setPullup(uint8_t pup);

  void sndTone();
  void sndOff();
  void sndClock();
  void sndSetFreq(uint8_t v);
  void sndEnvelopeOff();
  void sndBuzzerOn();
  void sndBuzzerOff();
};

}  // namespace brick
