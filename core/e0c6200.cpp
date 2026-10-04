// Seiko Epson E0C6200 4-bit microcontrollers: Tamagotchi, Digimon and other Bandai pets, Alien Fever.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Port of BrickEmuPy cores/E0C6200.py and E0C6200sound.py (azya52, CC0).
// Behaviour follows the Python instruction for instruction (tests/ compares traces).
//
// Timekeeping: the Python keeps its counters in floating point. All of them stay exact in binary
// (the CPU/OSC1 ratio is clock/2^15, and the stopwatch's 327.68 always lands on a 2^-44 grid), so
// they are kept here as integers in those units and give bit-identical results without any floats.
#include "e0c6200.h"

namespace brick {

static const uint32_t OSC1_CLOCK = 32768;
static const int32_t TIMER_CLOCK_DIV = 128;                            // OSC1 / 256
static const int64_t SW_ONE = (int64_t)1 << 44;
static const int64_t STOPWATCH_CLOCK_DIV = 5764607523034235LL;         // the double 327.68, in 2^-44 units
static const int32_t PTIMER_CLOCK_DIV[8] = {0, 0, 128, 64, 32, 16, 8, 4};

static const uint32_t VRAM_PART1 = 0xE00, VRAM_PART2 = 0xE80, VRAM_PART_SIZE = 0x50;
static const uint32_t IORAM_OFFSET = 0xF00, IORAM_SIZE = 0x7F;

static const uint8_t IO_CLKCHG = 8, IO_ALOFF = 8, IO_ALON = 4, IO_SVDDT = 8;
static const uint8_t IO_SHOTPW = 8, IO_BZFQ = 7, IO_BZSHOT = 8, IO_ENVRST = 4, IO_ENVRT = 2, IO_ENVON = 1;
static const uint8_t IO_TMRST = 2, IO_SWRST = 2, IO_SWRUN = 1, IO_PTRST = 2, IO_PTRUN = 1, IO_PTCOUT = 8, IO_PTC = 7;

// buzzer
static const uint8_t BUZZER_FREQ_DIV[8] = {8, 10, 12, 14, 16, 20, 24, 28};
static const int32_t SOUND_CLOCK_DIV = 128;
static const int32_t ONE_SHOT_PULSE_WIDTH_DIV[2] = {8 * SOUND_CLOCK_DIV, 16 * SOUND_CLOCK_DIV};
static const uint32_t ENVELOPE_CYCLE_DIV[2] = {16 * SOUND_CLOCK_DIV, 32 * SOUND_CLOCK_DIV};
static const int32_t ENVELOPE_AMP[8] = {0, 183, 366, 549, 731, 914, 1097, 1280};   // step / 7 * 5, Q8

E0C6200::E0C6200() {
  static const uint8_t noRom[2] = {0x0F, 0xFB};                        // NOP5, until setRom() is called
  rom_ = noRom; romSize_ = 2;
  for (int i = 0; i < PORT_COUNT; i++) { pullupExt_[i] = 0; pushpullMask_[i] = 0; pushpullSet_[i] = 0; }
  reset();
}

void E0C6200::setRom(int which, const uint8_t* data, size_t len) {
  if (which == 0 && data && len) { rom_ = data; romSize_ = (uint32_t)len; }
}

static int portIndex(const char* name) {
  static const char* const names[] = {"K0", "K1", "R0", "R1", "R2", "R3", "R4", "P0", "P1", "P2", "P3"};
  for (int i = 0; i < E0C6200::PORT_COUNT; i++) if (strEq(name, names[i])) return i;
  return -1;
}

bool E0C6200::setOption(const char* key, int, int32_t value) {
  static const char PULLUP[] = "port_pullup.", PUSHPULL[] = "port_pushpull.";
  const char* k = key; const char* p = PULLUP;
  while (*p && *k == *p) { k++; p++; }
  if (!*p) { int i = portIndex(k); if (i < 0) return false; pullupExt_[i] = (uint8_t)value; return true; }
  k = key; p = PUSHPULL;
  while (*p && *k == *p) { k++; p++; }
  if (!*p) { int i = portIndex(k); if (i < 0) return false; pushpullMask_[i] = (uint8_t)value; pushpullSet_[i] = 1; return true; }
  if (strEq(key, "p3_dedicated")) return true;                          // not emulated by BrickEmuPy either
  return false;
}

int E0C6200::portId(const char* name) const {
  if (strEq(name, "RES")) return RES;
  return portIndex(name);
}

// ------------------------------------------------------------------------------------------ reset
void E0C6200::initRegisters() {
  a_ = b_ = 0; ix_ = iy_ = 0; sp_ = 0;
  pc_ = npc_ = 0x100;
  cf_ = zf_ = df_ = if_ = 0;
  for (int i = 0; i < RAM_SIZE; i++) ram_[i] = 0;
  for (int i = 0; i < VRAM_SIZE; i++) vram_[i] = 0;
  halt_ = 0;
  for (int i = 0; i < PORT_COUNT; i++) {
    bool r = i >= R0 && i <= R4, pp = i >= P0;
    dir_[i] = r ? 0xF : 0;
    pushpull_[i] = pushpullSet_[i] ? pushpullMask_[i] : (pp ? 0xF : 0);
    latch_[i] = 0;
    pullup_[i] = pp ? 0xF : 0;
  }
  it_ = isw_ = ipt_ = isio_ = ik0_ = ik1_ = 0;
  eit_ = eisw_ = eipt_ = eisio_ = eik0_ = eik1_ = 0;
  tm_ = swl_ = swh_ = pt_ = rd_ = sd_ = 0;
  dfk0_ = 0xF;
  ctrlOsc_ = 0; ctrlLcd_ = IO_ALOFF; lc_ = 0; ctrlSvd_ = IO_SVDDT; ctrlBz1_ = ctrlBz2_ = 0;
  ctrlSw_ = ctrlPt_ = ptc_ = sc_ = hzr_ = ioc_ = pup_ = 0;
}

// what the RES pin does (BrickEmuPy's _reset): timers and the prescaler for the programmable timer keep going
void E0C6200::partialReset() {
  initRegisters();
  osc1Counter_ = 0; timerCounter_ = 0; stopwatchCounter_ = 0;
  sndBuzzerOff();
  sndEnvelopeOff();
}

// power on: everything as freshly built
void E0C6200::reset() {
  for (int i = 0; i < PORT_COUNT; i++) inLo_[i] = inHi_[i] = 0;
  initRegisters();
  osc1Counter_ = 0; timerCounter_ = 0; ptimerCounter_ = 0; stopwatchCounter_ = 0;
  instr_ = 0; ifDelay_ = 0; resetLine_ = 0; cycleFrac_ = 0;
  oneShot_ = 0; bzFreqDiv_ = BUZZER_FREQ_DIV[0]; envStep_ = 7; envCycle_ = ENVELOPE_CYCLE_DIV[0];
  envCounter_ = 0; envOn_ = 0; soundOn_ = 0;
  if (audio) audio->stop(0);
}

// ------------------------------------------------------------------------------------------ ports
uint8_t E0C6200::portRead(int p) const {
  uint8_t pu = pullupExt_[p] | pullup_[p];
  return (uint8_t)(((~dir_[p] & (inHi_[p] | (pu & ~inLo_[p]))) | (dir_[p] & latch_[p] & (pu | pushpull_[p]))) & 0xF);
}

void E0C6200::setPin(int port, int mask, int level) {
  if (port == RES) {
    if (level < 0) resetLine_ = 0;
    else { partialReset(); resetLine_ = 1; }
    return;
  }
  if (port < 0 || port >= PORT_COUNT) return;
  uint8_t m = (uint8_t)mask;
  uint8_t prev = portRead(port) & m;
  inLo_[port] &= (uint8_t)~m; inHi_[port] &= (uint8_t)~m;
  if (level == 0) inLo_[port] |= m;
  else if (level > 0) inHi_[port] |= m;
  if (port == K0) {
    uint8_t now = portRead(port) & m;
    if ((prev & eik0_) != (now & eik0_) && (dfk0_ & m) != now) {
      ik0_ |= 1;
      if (m & 8) processPtimer();
    }
  } else if (port == K1) {
    uint8_t now = portRead(port) & m;
    if ((prev & eik1_) != (now & eik1_) && m != now) ik1_ |= 1;
  }
}

void E0C6200::setPdir(uint8_t v) { for (int i = 0; i < 4; i++) dir_[P0 + i] = (v >> i & 1) ? 0xF : 0; }
void E0C6200::setPullup(uint8_t v) { for (int i = 0; i < 4; i++) pullup_[P0 + i] = (v >> i & 1) ? 0 : 0xF; }

size_t E0C6200::vram(uint8_t* out, size_t cap) const {
  size_t n = 0;
#define PUT(x) { if (n < cap) out[n] = (uint8_t)(x); n++; }
  if ((ctrlLcd_ & IO_ALOFF) | resetLine_) { for (int i = 0; i < VRAM_SIZE; i++) PUT(0); }
  else if (ctrlLcd_ & IO_ALON) { for (int i = 0; i < VRAM_SIZE; i++) PUT(1); }
  else {
    for (int i = 0; i < VRAM_SIZE; i++) PUT(vram_[i]);
    for (int i = P0; i <= P3; i++) PUT(latch_[i]);
    for (int i = R0; i <= R4; i++) PUT(portRead(i));
  }
#undef PUT
  return n < cap ? n : cap;
}

// ------------------------------------------------------------------------------------------ memory
uint8_t E0C6200::getMem(uint32_t addr) {
  if (addr < RAM_SIZE) return ram_[addr];
  if (addr >= VRAM_PART1 && addr < VRAM_PART1 + VRAM_PART_SIZE) return vram_[addr - VRAM_PART1];
  if (addr >= VRAM_PART2 && addr < VRAM_PART2 + VRAM_PART_SIZE) return vram_[addr - VRAM_PART2 + VRAM_PART_SIZE];
  if (addr < IORAM_OFFSET || addr >= IORAM_OFFSET + IORAM_SIZE) return 0;
  uint8_t r;
  switch (addr) {
    case 0xF00: r = it_; it_ = 0; return r;                             // interrupt factor flags clear on read
    case 0xF01: r = isw_; isw_ = 0; return r;
    case 0xF02: r = ipt_; ipt_ = 0; return r;
    case 0xF03: r = isio_; isio_ = 0; return r;
    case 0xF04: r = ik0_; ik0_ = 0; return r;
    case 0xF05: r = ik1_; ik1_ = 0; return r;
    case 0xF10: return eit_;
    case 0xF11: return eisw_;
    case 0xF12: return eipt_;
    case 0xF13: return eisio_;
    case 0xF14: return eik0_;
    case 0xF15: return eik1_;
    case 0xF20: return tm_ & 0xF;
    case 0xF21: return tm_ >> 4;
    case 0xF22: return swl_ & 0xF;
    case 0xF23: return swh_ & 0xF;
    case 0xF24: return pt_ & 0xF;
    case 0xF25: return pt_ >> 4;
    case 0xF26: return rd_ & 0xF;
    case 0xF27: return rd_ >> 4;
    case 0xF30: return sd_ & 0xF;
    case 0xF31: return sd_ >> 4;
    case 0xF40: return portRead(K0);
    case 0xF41: return dfk0_;
    case 0xF42: return portRead(K1);
    case 0xF50: case 0xF51: case 0xF52: case 0xF53: case 0xF54: return latch_[R0 + (addr - 0xF50)];
    case 0xF60: case 0xF61: case 0xF62: case 0xF63: return portRead(P0 + (addr - 0xF60));
    case 0xF70: return ctrlOsc_;
    case 0xF71: return ctrlLcd_;
    case 0xF72: return lc_;
    case 0xF74: return ctrlBz1_;
    case 0xF75: return (uint8_t)((ctrlBz2_ & (IO_ENVRT | IO_ENVON)) | (oneShot_ > 0 ? IO_BZSHOT : 0));
    case 0xF77: return ctrlSw_ & IO_SWRUN;
    case 0xF78: return ctrlPt_ & IO_PTRUN;
    case 0xF79: return ptc_;
    case 0xF7D: return ioc_;
    case 0xF7E: return pup_;
  }
  return 0;
}

void E0C6200::setMem(uint32_t addr, uint8_t v) {
  if (addr < RAM_SIZE) { ram_[addr] = v & 0xF; return; }
  if (addr >= VRAM_PART1 && addr < VRAM_PART1 + VRAM_PART_SIZE) { vram_[addr - VRAM_PART1] = v & 0xF; return; }
  if (addr >= VRAM_PART2 && addr < VRAM_PART2 + VRAM_PART_SIZE) { vram_[addr - VRAM_PART2 + VRAM_PART_SIZE] = v & 0xF; return; }
  if (addr < IORAM_OFFSET || addr >= IORAM_OFFSET + IORAM_SIZE) return;
  switch (addr) {
    case 0xF10: eit_ = v; break;
    case 0xF11: eisw_ = v & 3; break;
    case 0xF12: eipt_ = v & 1; break;
    case 0xF13: eisio_ = v & 1; break;
    case 0xF14: eik0_ = v; break;
    case 0xF15: eik1_ = v; break;
    case 0xF26: rd_ = (uint8_t)((rd_ & 0xF0) | (v & 0x0F)); break;
    case 0xF27: rd_ = (uint8_t)((rd_ & 0x0F) | (v << 4 & 0xF0)); break;
    case 0xF30: sd_ = (uint8_t)((sd_ & 0xF0) | (v & 0x0F)); break;
    case 0xF31: sd_ = (uint8_t)((sd_ & 0x0F) | (v << 4 & 0xF0)); break;
    case 0xF41: dfk0_ = v; break;
    case 0xF50: case 0xF51: case 0xF52: case 0xF53: latch_[R0 + (addr - 0xF50)] = v; break;
    case 0xF54:
      latch_[R4] = v;
      if (v & 8) sndBuzzerOff(); else sndBuzzerOn();                    // R43 drives the buzzer, active low
      break;
    case 0xF60: case 0xF61: case 0xF62: case 0xF63: latch_[P0 + (addr - 0xF60)] = v; break;
    case 0xF70: ctrlOsc_ = v; break;
    case 0xF71: ctrlLcd_ = v; break;
    case 0xF72: lc_ = v; break;
    case 0xF74: ctrlBz1_ = v; sndSetFreq(v & IO_BZFQ); break;
    case 0xF75:
      ctrlBz2_ = v & (IO_ENVRT | IO_ENVON);
      envCycle_ = ENVELOPE_CYCLE_DIV[(v & IO_ENVRT) ? 1 : 0];
      if ((v & IO_BZSHOT) && oneShot_ == 0) {
        oneShot_ = ONE_SHOT_PULSE_WIDTH_DIV[(ctrlBz1_ & IO_SHOTPW) ? 1 : 0];
        if (!soundOn_) sndTone();
      }
      if (v & IO_ENVON) { envOn_ = 1; envStep_ = 7; }
      else sndEnvelopeOff();
      if (v & IO_ENVRST) envStep_ = 7;
      break;
    case 0xF76: if (v & IO_TMRST) tm_ = 0; break;
    case 0xF77: if (v & IO_SWRST) swl_ = swh_ = 0; ctrlSw_ = v & IO_SWRUN; break;
    case 0xF78: if (v & IO_PTRST) pt_ = rd_; ctrlPt_ = v & IO_PTRUN; break;
    case 0xF79: ptc_ = v; break;
    case 0xF7D: ioc_ = v; setPdir(v); break;
    case 0xF7E: pup_ = v; setPullup(v); break;
  }
}

// operand r/q of the ALU instructions: A, B, M(X), M(Y)
uint8_t E0C6200::getR(int r) {
  switch (r & 3) {
    case 0: return a_;
    case 1: return b_;
    case 2: return getMem(ix_);
    default: return getMem(iy_);
  }
}

void E0C6200::setR(int r, uint8_t v) {
  switch (r & 3) {
    case 0: a_ = v & 0xF; break;
    case 1: b_ = v & 0xF; break;
    case 2: setMem(ix_, v); break;
    default: setMem(iy_, v); break;
  }
}

// ------------------------------------------------------------------------------------------ sound
void E0C6200::sndTone() {
  if (audio) audio->tone(0, (OSC1_CLOCK << 8) / bzFreqDiv_, false, ENVELOPE_AMP[envStep_ & 7]);
}
void E0C6200::sndOff() { if (audio) audio->stop(0); }

void E0C6200::sndClock() {
  if (oneShot_ > 0 && --oneShot_ <= 0) sndOff();
  if (envCounter_ > 0 && --envCounter_ <= 0) {
    if (envStep_ > 0) { sndTone(); envStep_--; }
    envCounter_ = (int32_t)envCycle_;
  }
}

void E0C6200::sndSetFreq(uint8_t v) {
  bzFreqDiv_ = BUZZER_FREQ_DIV[v & 7];
  if (soundOn_) sndTone();
}

void E0C6200::sndEnvelopeOff() { envOn_ = 0; envStep_ = 7; envCounter_ = 0; sndOff(); }

void E0C6200::sndBuzzerOn() {
  soundOn_ = 1; oneShot_ = 0;
  sndTone();
  if (envOn_) envCounter_ = (int32_t)envCycle_;
}

void E0C6200::sndBuzzerOff() { soundOn_ = 0; envCounter_ = 0; oneShot_ = 0; sndOff(); }

// ------------------------------------------------------------------------------------------ timers
void E0C6200::processPtimer() {
  pt_ = (uint8_t)(pt_ - 1);
  if (pt_ == 0) { pt_ = rd_; ipt_ |= 1; }
  if (ptc_ & IO_PTCOUT) latch_[R3] ^= 8;
}

void E0C6200::clockOsc1() {
  sndClock();
  if ((ptc_ & IO_PTC) > 1 && --ptimerCounter_ <= 0) {
    ptimerCounter_ += PTIMER_CLOCK_DIV[ptc_ & IO_PTC];
    processPtimer();
  }
  stopwatchCounter_ -= SW_ONE;
  if (stopwatchCounter_ <= 0) {
    stopwatchCounter_ += STOPWATCH_CLOCK_DIV;
    if (ctrlSw_ & IO_SWRUN) {
      swl_ = (uint8_t)((swl_ + 1) % 10);
      if (swl_ == 0) {
        swh_ = (uint8_t)((swh_ + 1) % 10);
        isw_ |= 1;
        if (swh_ == 0) isw_ |= 2;
      }
    }
  }
  if (--timerCounter_ <= 0) {
    timerCounter_ += TIMER_CLOCK_DIV;
    uint8_t n = (uint8_t)(tm_ + 1);
    if ((n & 0x04) < (tm_ & 0x04)) it_ |= 1;                            // 32 Hz
    if ((n & 0x10) < (tm_ & 0x10)) it_ |= 2;                            // 8 Hz
    if ((n & 0x40) < (tm_ & 0x40)) it_ |= 4;                            // 2 Hz
    if ((n & 0x80) < (tm_ & 0x80)) it_ |= 8;                            // 1 Hz
    tm_ = n;
  }
}

int E0C6200::interrupt(uint32_t vector) {
  setMem((sp_ - 1) & 0xFF, pc_ >> 8 & 0xF);
  setMem((sp_ - 2) & 0xFF, pc_ >> 4 & 0xF);
  sp_ = (uint8_t)(sp_ - 3);
  setMem(sp_, pc_ & 0xF);
  if_ = 0; halt_ = 0;
  pc_ = npc_ = (npc_ & 0x1000) | 0x100 | vector;
  return 13;
}

// ------------------------------------------------------------------------------------------ CPU
int E0C6200::step() {
  uint32_t exec = 7;
  if (!resetLine_) {
    if (!halt_) {
      ifDelay_ = 0;
      uint32_t a = pc_ * 2;
      uint32_t op = ((uint32_t)rom_[a % romSize_] << 8 | rom_[(a + 1) % romSize_]) & 0xFFF;
      exec = (uint32_t)execute(op);
      instr_++;
    }
    if (if_ && !ifDelay_) {
      if (ipt_ & eipt_) exec += interrupt(0xC);
      else if (isio_ & eisio_) exec += interrupt(0xA);
      else if (ik1_) exec += interrupt(0x8);
      else if (ik0_) exec += interrupt(0x6);
      else if (isw_ & eisw_) exec += interrupt(0x4);
      else if (it_ & eit_) exec += interrupt(0x2);
    }
    // CLKCHG selects OSC3 (the device clock); otherwise the CPU runs from the 32768 Hz OSC1
    int64_t fine = (ctrlOsc_ & IO_CLKCHG) ? (int64_t)exec * OSC1_CLOCK : (int64_t)exec * clockHz;
    osc1Counter_ -= fine;
    while (osc1Counter_ <= 0) { osc1Counter_ += clockHz; clockOsc1(); }
    cycleFrac_ += (uint32_t)fine;
  } else {
    cycleFrac_ += exec * OSC1_CLOCK;
  }
  int whole = (int)(cycleFrac_ >> 15);
  cycleFrac_ &= OSC1_CLOCK - 1;
  return whole;
}

#define NEXT() (pc_ = npc_ = (pc_ & 0x1000) | ((pc_ + 1) & 0xFFF))
#define JUMP() (pc_ = (npc_ & 0x1F00) | (op & 0xFF))

int E0C6200::execute(uint32_t op) {
  const int r2 = (op >> 2) & 3, q = op & 3, r4 = (op >> 4) & 3;
  const int imm = op & 0xF;
  int res;
  switch (op >> 8) {
    case 0x0: JUMP(); return 5;                                                         // JP s
    case 0x1:                                                                           // RETD l
      pc_ = npc_ = (pc_ & 0x1000) | ((uint32_t)ram_[sp_ + 2] << 8) | ((uint32_t)ram_[sp_ + 1] << 4) | ram_[sp_];
      sp_ = (uint8_t)(sp_ + 3);
      setMem(ix_, op & 0xF);
      setMem((ix_ & 0xF00) | ((ix_ + 1) & 0xFF), op >> 4 & 0xF);
      ix_ = (ix_ & 0xF00) | ((ix_ + 2) & 0xFF);
      return 12;
    case 0x2: if (cf_) JUMP(); else NEXT(); return 5;                                   // JP C
    case 0x3: if (!cf_) JUMP(); else NEXT(); return 5;                                  // JP NC
    case 0x4: case 0x5:                                                                 // CALL / CALZ
      setMem((sp_ - 1) & 0xFF, (pc_ + 1) >> 8 & 0xF);
      setMem((sp_ - 2) & 0xFF, (pc_ + 1) >> 4 & 0xF);
      sp_ = (uint8_t)(sp_ - 3);
      setMem(sp_, (pc_ + 1) & 0xF);
      if (op >> 8 == 0x4) JUMP();
      else pc_ = npc_ = (npc_ & 0x1000) | (op & 0xFF);
      return 7;
    case 0x6: if (zf_) JUMP(); else NEXT(); return 5;                                   // JP Z
    case 0x7: if (!zf_) JUMP(); else NEXT(); return 5;                                  // JP NZ
    case 0x8: iy_ = (iy_ & 0xF00) | (op & 0xFF); NEXT(); return 5;                       // LD Y,y
    case 0x9:                                                                           // LBPX MX,l
      setMem(ix_, op & 0xF);
      setMem((ix_ & 0xF00) | ((ix_ + 1) & 0xFF), op >> 4 & 0xF);
      ix_ = (ix_ & 0xF00) | ((ix_ + 2) & 0xFF);
      NEXT(); return 5;
    case 0xA:
      switch ((op >> 4) & 0xF) {
        case 0x0: case 0x1: case 0x2: case 0x3: {                                       // ADC XH/XL/YH/YL,i
          uint32_t& reg = (op & 0x20) ? iy_ : ix_;
          int sh = (op & 0x10) ? 0 : 4;
          res = (int)((reg >> sh) & 0xF) + imm + cf_;
          zf_ = (res & 0xF) == 0; cf_ = res > 15;
          reg = (reg & ~(0xFu << sh)) | ((uint32_t)(res & 0xF) << sh);
          NEXT(); return 7;
        }
        case 0x4: case 0x5: case 0x6: case 0x7: {                                       // CP XH/XL/YH/YL,i
          uint32_t reg = (op & 0x20) ? iy_ : ix_;
          res = (int)((reg >> ((op & 0x10) ? 0 : 4)) & 0xF) - imm;
          zf_ = res == 0; cf_ = res < 0;
          NEXT(); return 7;
        }
        case 0x8: case 0x9: {                                                           // ADD / ADC r,q
          int x = getR(r2); int y = getR(q);
          res = x + y + ((op & 0x10) ? cf_ : 0);
          cf_ = res > 15;
          if (df_ && res > 9) { res += 6; cf_ = 1; }
          zf_ = (res & 0xF) == 0;
          setR(r2, res & 0xF);
          NEXT(); return 7;
        }
        case 0xA: case 0xB: {                                                           // SUB / SBC r,q
          int x = getR(r2); int y = getR(q);
          res = x - y - ((op & 0x10) ? cf_ : 0);
          cf_ = res < 0;
          if (df_ && res < 0) res += 10;
          zf_ = (res & 0xF) == 0;
          setR(r2, res & 0xF);
          NEXT(); return 7;
        }
        case 0xC: case 0xD: case 0xE: {                                                 // AND / OR / XOR r,q
          int x = getR(r2); int y = getR(q);
          res = ((op >> 4) & 0xF) == 0xC ? (x & y) : ((op >> 4) & 0xF) == 0xD ? (x | y) : (x ^ y);
          zf_ = res == 0;
          setR(r2, res);
          NEXT(); return 7;
        }
        default:                                                                        // RLC r
          res = (getR(q) << 1) + cf_;
          cf_ = res > 15;
          setR(q, res & 0xF);
          NEXT(); return 7;
      }
    case 0xB: ix_ = (ix_ & 0xF00) | (op & 0xFF); NEXT(); return 5;                       // LD X,x
    case 0xC: case 0xD: {
      const int kind = (op >> 6) & 7;                                                   // C00 ADD, C40 ADC, C80 AND, CC0 OR, D00 XOR, D40 SBC, D80 FAN, DC0 CP
      int x = getR(r4);
      switch (kind) {
        case 0: case 1:
          res = x + imm + (kind == 1 ? cf_ : 0);
          cf_ = res > 15;
          if (df_ && res > 9) { res += 6; cf_ = 1; }
          zf_ = (res & 0xF) == 0;
          setR(r4, res & 0xF);
          break;
        case 2: res = x & imm; zf_ = res == 0; setR(r4, res); break;
        case 3: res = x | imm; zf_ = res == 0; setR(r4, res); break;
        case 4: res = x ^ imm; zf_ = res == 0; setR(r4, res); break;
        case 5:
          res = x - imm - cf_;
          cf_ = res < 0;
          if (df_ && cf_) res += 10;
          zf_ = (res & 0xF) == 0;
          setR(r4, res & 0xF);
          break;
        case 6: zf_ = (x & imm) == 0; break;
        default: res = x - imm; zf_ = res == 0; cf_ = res < 0; break;
      }
      NEXT(); return 7;
    }
    case 0xE: {
      const uint32_t lo = op & 0xFF;
      if (lo < 0x40) { setR(r4, imm); NEXT(); return 5; }                               // LD r,i
      if (lo < 0x60) { ifDelay_ = 1; npc_ = op << 8 & 0x1F00; pc_ = (pc_ & 0x1000) | ((pc_ + 1) & 0xFFF); return 5; }   // PSET p
      if (lo < 0x70) { setMem(ix_, imm); ix_ = (ix_ & 0xF00) | ((ix_ + 1) & 0xFF); NEXT(); return 5; }   // LDPX MX,i
      if (lo < 0x80) { setMem(iy_, imm); iy_ = (iy_ & 0xF00) | ((iy_ + 1) & 0xFF); NEXT(); return 5; }   // LDPY MY,i
      if (lo < 0xC0) {
        switch ((lo >> 2) & 0xF) {
          case 0x0: ix_ = ((uint32_t)getR(q) << 8) | (ix_ & 0x0FF); break;                // LD XP,r
          case 0x1: ix_ = ((uint32_t)getR(q) << 4) | (ix_ & 0xF0F); break;                // LD XH,r
          case 0x2: ix_ = (uint32_t)getR(q) | (ix_ & 0xFF0); break;                       // LD XL,r
          case 0x3: res = getR(q) + (cf_ << 4); cf_ = res & 1; setR(q, res >> 1); break;  // RRC r
          case 0x4: iy_ = ((uint32_t)getR(q) << 8) | (iy_ & 0x0FF); break;                // LD YP,r
          case 0x5: iy_ = ((uint32_t)getR(q) << 4) | (iy_ & 0xF0F); break;                // LD YH,r
          case 0x6: iy_ = (uint32_t)getR(q) | (iy_ & 0xFF0); break;                       // LD YL,r
          case 0x8: setR(q, ix_ >> 8); break;                                             // LD r,XP
          case 0x9: setR(q, ix_ >> 4 & 0xF); break;                                       // LD r,XH
          case 0xA: setR(q, ix_ & 0xF); break;                                            // LD r,XL
          case 0xC: setR(q, iy_ >> 8); break;                                             // LD r,YP
          case 0xD: setR(q, iy_ >> 4 & 0xF); break;                                       // LD r,YH
          case 0xE: setR(q, iy_ & 0xF); break;                                            // LD r,YL
          default: return 5;                                                              // undefined: PC stays
        }
        NEXT(); return 5;
      }
      if (lo < 0xD0) { setR(r2, getR(q)); NEXT(); return 5; }                           // LD r,q
      if (lo < 0xE0) return 5;                                                          // undefined
      if (lo < 0xF0) { setR(r2, getR(q)); ix_ = (ix_ & 0xF00) | ((ix_ + 1) & 0xFF); NEXT(); return 5; }   // LDPX r,q
      setR(r2, getR(q)); iy_ = (iy_ & 0xF00) | ((iy_ + 1) & 0xFF); NEXT(); return 5;    // LDPY r,q
    }
    default: break;                                                                     // 0xF
  }

  const uint32_t lo = op & 0xFF;
  switch (lo >> 4) {
    case 0x0: { int x = getR(r2); int y = getR(q); res = x - y; zf_ = res == 0; cf_ = res < 0; NEXT(); return 7; }   // CP r,q
    case 0x1: { int x = getR(r2); int y = getR(q); zf_ = (x & y) == 0; NEXT(); return 7; }                            // FAN r,q
    case 0x2: case 0x3: {
      if ((lo & 0x8) == 0) return 5;                                                    // undefined
      uint32_t& reg = (lo & 4) ? iy_ : ix_;
      int m = getMem(reg); int x = getR(q);
      if (lo < 0x30) {                                                                  // ACPX / ACPY
        res = m + x + cf_;
        cf_ = res > 15;
        if (df_ && res > 9) { res += 6; cf_ = 1; }
      } else {                                                                          // SCPX / SCPY
        res = m - x - cf_;
        cf_ = res < 0;
        if (df_ && res < 0) res += 10;
      }
      zf_ = (res & 0xF) == 0;
      setMem(reg, res & 0xF);
      reg = (reg & 0xF00) | ((reg + 1) & 0xFF);
      NEXT(); return 7;
    }
    case 0x4: {                                                                         // SET F,i
      cf_ |= imm & 1; zf_ |= imm >> 1 & 1; df_ |= imm >> 2 & 1;
      uint8_t newIf = imm >> 3 & 1;
      ifDelay_ = newIf && !if_;
      if_ |= newIf;
      NEXT(); return 7;
    }
    case 0x5: cf_ &= imm & 1; zf_ &= imm >> 1 & 1; df_ &= imm >> 2 & 1; if_ &= imm >> 3 & 1; NEXT(); return 7;   // RST F,i
    case 0x6: res = getMem(imm) + 1; zf_ = res == 16; cf_ = res > 15; setMem(imm, res & 0xF); NEXT(); return 7;   // INC Mn
    case 0x7: res = getMem(imm) - 1; zf_ = res == 0; cf_ = res < 0; setMem(imm, res & 0xF); NEXT(); return 7;     // DEC Mn
    case 0x8: setMem(imm, a_); NEXT(); return 5;                                                                   // LD Mn,A
    case 0x9: setMem(imm, b_); NEXT(); return 5;                                                                   // LD Mn,B
    case 0xA: a_ = getMem(imm); NEXT(); return 5;                                                                  // LD A,Mn
    case 0xB: b_ = getMem(imm); NEXT(); return 5;                                                                  // LD B,Mn
    case 0xC: {                                                                         // PUSH
      uint8_t v = 0;
      switch (lo & 0xF) {
        case 0: case 1: case 2: case 3: break;
        case 4: v = ix_ >> 8; break;
        case 5: v = ix_ >> 4 & 0xF; break;
        case 6: v = ix_ & 0xF; break;
        case 7: v = iy_ >> 8; break;
        case 8: v = iy_ >> 4 & 0xF; break;
        case 9: v = iy_ & 0xF; break;
        case 0xA: v = (uint8_t)(if_ << 3 | df_ << 2 | zf_ << 1 | cf_); break;
        case 0xB: sp_ = (uint8_t)(sp_ - 1); NEXT(); return 5;                           // DEC SP
        default: return 5;                                                              // undefined
      }
      if ((lo & 0xF) < 4) {
        // PUSH r: the operand is read after SP moves (it may be M(X)/M(Y) pointing at the stack)
        sp_ = (uint8_t)(sp_ - 1);
        setMem(sp_, getR(q));
      } else {
        sp_ = (uint8_t)(sp_ - 1);
        setMem(sp_, v);
      }
      NEXT(); return 5;
    }
    case 0xD:                                                                           // POP, RET
      switch (lo & 0xF) {
        case 0: case 1: case 2: case 3: setR(q, getMem(sp_)); break;
        case 4: ix_ = (uint32_t)getMem(sp_) << 8 | (ix_ & 0x0FF); break;
        case 5: ix_ = (uint32_t)getMem(sp_) << 4 | (ix_ & 0xF0F); break;
        case 6: ix_ = getMem(sp_) | (ix_ & 0xFF0); break;
        case 7: iy_ = (uint32_t)getMem(sp_) << 8 | (iy_ & 0x0FF); break;
        case 8: iy_ = (uint32_t)getMem(sp_) << 4 | (iy_ & 0xF0F); break;
        case 9: iy_ = getMem(sp_) | (iy_ & 0xFF0); break;
        case 0xA: {
          uint8_t f = getMem(sp_);
          cf_ = f & 1; zf_ = f >> 1 & 1; df_ = f >> 2 & 1;
          uint8_t newIf = f >> 3 & 1;
          ifDelay_ = newIf && !if_;
          if_ = newIf;
          break;
        }
        case 0xB: break;                                                                // INC SP
        case 0xE: case 0xF: {                                                           // RETS / RET
          uint32_t l = getMem(sp_), m = getMem(sp_ + 1u), h = getMem(sp_ + 2u);
          pc_ = (pc_ & 0x1000) | l | m << 4 | h << 8;
          sp_ = (uint8_t)(sp_ + 3);
          if ((lo & 0xF) == 0xE) { NEXT(); return 12; }
          npc_ = pc_;
          return 7;
        }
        default: return 5;                                                              // undefined
      }
      sp_ = (uint8_t)(sp_ + 1);
      NEXT(); return 5;
    case 0xE:
      if (lo < 0xE4) sp_ = (uint8_t)(getR(q) << 4 | (sp_ & 0x0F));                      // LD SPH,r
      else if (lo < 0xE8) setR(q, sp_ >> 4);                                            // LD r,SPH
      else if (lo == 0xE8) { pc_ = (npc_ & 0x1F00) | (uint32_t)b_ << 4 | a_; return 5; } // JPBA
      else return 5;                                                                    // undefined
      NEXT(); return 5;
    default:                                                                            // 0xF
      if (lo < 0xF4) sp_ = (uint8_t)(getR(q) | (sp_ & 0xF0));                           // LD SPL,r
      else if (lo < 0xF8) setR(q, sp_ & 0x0F);                                          // LD r,SPL
      else if (lo == 0xF8) halt_ = 1;                                                   // HALT
      else if (lo == 0xFB) {}                                                           // NOP5
      else if (lo == 0xFF) { NEXT(); return 7; }                                        // NOP7
      else return 5;                                                                    // undefined
      NEXT(); return 5;
  }
}

#undef NEXT
#undef JUMP

// ------------------------------------------------------------------------------------------ state
void E0C6200::sync(State& s) {
  s.v(pc_); s.v(npc_); s.v(ix_); s.v(iy_); s.v(instr_);
  s.v(a_); s.v(b_); s.v(sp_); s.v(cf_); s.v(zf_); s.v(df_); s.v(if_); s.v(halt_); s.v(ifDelay_); s.v(resetLine_);
  s.bytes(ram_, RAM_SIZE); s.bytes(vram_, VRAM_SIZE);
  s.bytes(inLo_, PORT_COUNT); s.bytes(inHi_, PORT_COUNT); s.bytes(dir_, PORT_COUNT); s.bytes(pushpull_, PORT_COUNT);
  s.bytes(latch_, PORT_COUNT); s.bytes(pullup_, PORT_COUNT);
  uint8_t* io[] = {&it_, &isw_, &ipt_, &isio_, &ik0_, &ik1_, &eit_, &eisw_, &eipt_, &eisio_, &eik0_, &eik1_,
                   &tm_, &swl_, &swh_, &pt_, &rd_, &sd_, &dfk0_, &ctrlOsc_, &ctrlLcd_, &lc_, &ctrlSvd_, &ctrlBz1_, &ctrlBz2_,
                   &ctrlSw_, &ctrlPt_, &ptc_, &sc_, &hzr_, &ioc_, &pup_};
  for (size_t i = 0; i < sizeof io / sizeof io[0]; i++) s.v(*io[i]);
  s.v(osc1Counter_); s.v(timerCounter_); s.v(ptimerCounter_); s.v(stopwatchCounter_); s.v(cycleFrac_);
  s.v(bzFreqDiv_); s.v(envCycle_); s.v(oneShot_); s.v(envCounter_); s.v(envStep_); s.v(envOn_); s.v(soundOn_);
}

size_t E0C6200::debugRegs(uint32_t* out, size_t cap) const {
  // same order as BrickEmuPy's examine()
  size_t n = 0;
#define PUT(x) { if (n < cap) out[n] = (uint32_t)(x); n++; }
  PUT(pc_); PUT(npc_ & 0x1F00); PUT(a_); PUT(b_); PUT(ix_); PUT(iy_); PUT(sp_);
  PUT(cf_); PUT(zf_); PUT(df_); PUT(if_); PUT(halt_);
  for (int i = 0; i < 640; i++) PUT(ram_[i]);
  for (int i = 0; i < VRAM_SIZE; i++) PUT(vram_[i]);
  PUT(it_); PUT(isw_); PUT(ipt_); PUT(isio_); PUT(ik0_); PUT(ik1_);
  PUT(eit_); PUT(eisw_); PUT(eipt_); PUT(eisio_); PUT(eik0_); PUT(eik1_);
  PUT(tm_ & 0xF); PUT(tm_ >> 4); PUT(swl_); PUT(swh_); PUT(pt_ & 0xF); PUT(pt_ >> 4);
  PUT(rd_ & 0xF); PUT(rd_ >> 4); PUT(sd_ & 0xF); PUT(sd_ >> 4);
  PUT(portRead(K0)); PUT(dfk0_); PUT(portRead(K1));
  for (int i = R0; i <= R4; i++) PUT(latch_[i]);
  for (int i = P0; i <= P3; i++) PUT(portRead(i));
  PUT(ctrlOsc_); PUT(ctrlLcd_); PUT(lc_); PUT(ctrlSvd_); PUT(ctrlBz1_); PUT(ctrlBz2_); PUT(0);
  PUT(ctrlSw_); PUT(ctrlPt_); PUT(ptc_); PUT(sc_); PUT(hzr_); PUT(ioc_); PUT(pup_);
#undef PUT
  return n;
}

}  // namespace brick
