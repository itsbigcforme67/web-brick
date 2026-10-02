// Holtek 4-bit LCD microcontrollers: HT943 and HTG12N0.
// Copyright (C) 2026 the web-brick authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Port of BrickEmuPy cores/HT4BIT.py, HT943.py, HTG12N0.py, HT4BITsound.py (azya52, CC0).
// Behaviour follows the Python instruction for instruction (tests/ compares traces).
#include "ht4bit.h"

namespace brick {

static const uint32_t TIMER_INT_LOCATION = 4, EXTERNAL_INT_LOCATION = 8;
static const uint32_t SUB_CLOCK = 32768;
static const uint8_t PC_RAM_BANK1 = 0x01, PC_LCD_ON = 0x02;
static const int32_t SOUND_AMP = 5 * Audio::AMP_ONE;             // "squareness factor" 5
static const uint32_t SINGLE_SIZE_CHANNEL_SIZE = 32, SINGLE_SIZE_CHANNEL_COUNT = 12;

static const uint8_t LFSR2DIV[128] = {
  0, 2, 123, 3, 124, 75, 117, 4, 125, 101, 111, 76, 118, 42, 69, 5, 126, 66, 63, 102, 112, 86, 36, 77, 119,
  21, 95, 43, 70, 25, 105, 6, 127, 115, 99, 67, 64, 34, 19, 103, 113, 17, 15, 87, 37, 55, 89, 78, 120, 39,
  60, 22, 96, 52, 57, 44, 71, 91, 30, 26, 106, 47, 80, 7, 1, 122, 74, 116, 100, 110, 41, 68, 65, 62, 85, 35,
  20, 94, 24, 104, 114, 98, 33, 18, 16, 14, 54, 88, 38, 59, 51, 56, 90, 29, 46, 79, 121, 73, 109, 40, 61, 84,
  93, 23, 97, 32, 13, 53, 58, 50, 28, 45, 72, 108, 83, 92, 31, 12, 49, 27, 107, 82, 11, 48, 81, 10, 9, 8
};

HT4Bit::HT4Bit(Variant v) : variant_(v) {
  for (int i = 0; i < SROM_SIZE; i++) srom_[i] = 0;
  for (int i = 0; i < 16; i++) { speedDiv_[i] = 0; effect_[i] = 0; }
  static const uint8_t noRom[1] = {0x3E};                        // NOP, until setRom() is called
  rom_ = noRom; romSize_ = 1;
  coreReset();
}

void HT4Bit::setRom(int which, const uint8_t* data, size_t len) {
  if (which == 0) { if (data && len) { rom_ = data; romSize_ = (uint32_t)len; } }
  else if (which == 1) { for (size_t i = 0; i < SROM_SIZE; i++) srom_[i] = i < len ? data[i] : 0; }
}

bool HT4Bit::setOption(const char* key, int index, int32_t value) {
  if (strEq(key, "port_pullup.PP")) pullup_[PORT_PP] = (uint8_t)value;
  else if (strEq(key, "port_pullup.PS")) pullup_[PORT_PS] = (uint8_t)value;
  else if (strEq(key, "port_pullup.PM")) pullup_[PORT_PM] = (uint8_t)value;
  else if (strEq(key, "port_wakeup.PP")) wakeup_[PORT_PP] = (uint8_t)value;
  else if (strEq(key, "port_wakeup.PS")) wakeup_[PORT_PS] = (uint8_t)value;
  else if (strEq(key, "port_wakeup.PM")) wakeup_[PORT_PM] = (uint8_t)value;
  else if (strEq(key, "timer_clock_div")) timerDiv_ = value > 0 ? (uint32_t)value : 1;
  else if (strEq(key, "sound_freq_div")) soundFreqDiv_ = value > 0 ? (uint32_t)value : 1;
  else if (strEq(key, "rtc_clock_div")) rtcClockDiv_ = (uint32_t)value;
  else if (strEq(key, "sound_speed_div") && index >= 0 && index < 16) speedDiv_[index] = (uint8_t)(value & 127);
  else if (strEq(key, "sound_effect") && index >= 0 && index < 16) effect_[index] = (uint8_t)value;
  else return false;
  return true;
}

void HT4Bit::coreReset() {
  acc_ = 0;
  for (int i = 0; i < 5; i++) wr_[i] = 0;
  pc_ = 0; stack_ = 0;
  ei_ = cf_ = tf_ = ef_ = halt_ = resetLine_ = timerOn_ = 0;
  tc_ = 0; timerCounter_ = 0; instr_ = 0;
  soundOff();
  sndRepeat_ = 0;
  for (int i = 0; i < 256; i++) ram_[i] = variant_ == HT943 ? 0xF : 0;
  for (int i = 0; i < VRAM_SIZE; i++) lcd_[i] = 0;
  rtcCounter_ = 0;
  pa_ = pb_ = pcPort_ = 0;
  for (int i = 0; i < 3; i++) port_[i] = pullup_[i];
}

void HT4Bit::reset() { coreReset(); }

int HT4Bit::portId(const char* name) const {
  if (strEq(name, "PS")) return PORT_PS;
  if (strEq(name, "PM")) return PORT_PM;
  if (strEq(name, "RES")) return PORT_RES;
  if (strEq(name, "PP") && variant_ == HT943) return PORT_PP;
  return -1;
}

void HT4Bit::setPin(int port, int pin, int level) {
  uint8_t bit = (uint8_t)(1u << pin);
  if (port == PORT_RES) {
    if (level >= 0) { coreReset(); resetLine_ = 1; } else resetLine_ = 0;
    return;
  }
  if (port < 0 || port > PORT_PM) return;
  if (level >= 0) {
    port_[port] = (uint8_t)((port_[port] & ~bit) | (level ? bit : 0));
    if (variant_ == HT943) {
      if (halt_ && (wakeup_[port] & bit) && !level) { ef_ = 1; halt_ = 0; }
    } else if (wakeup_[port] & bit) {
      halt_ = 0;
    }
  } else {
    port_[port] = (uint8_t)((port_[port] & ~bit) | (pullup_[port] & bit));
  }
}

size_t HT4Bit::vram(uint8_t* out, size_t cap) const {
  if (variant_ == HT943) {
    size_t n = cap < 256 ? cap : 256;
    bool blank = halt_ | resetLine_;
    for (size_t i = 0; i < n; i++) out[i] = blank ? 0 : ram_[i];
    return n;
  }
  size_t n = cap < VRAM_SIZE ? cap : (size_t)VRAM_SIZE;
  bool blank = !((pcPort_ & PC_LCD_ON) | resetLine_);
  for (size_t i = 0; i < n; i++) out[i] = blank ? 0 : lcd_[i];
  return n;
}

uint8_t HT4Bit::readRam(int rp) const {
  uint32_t i = ramIndex(rp);
  if (variant_ == HT943) return ram_[i];
  if (i >= 128) return lcd_[i - 128];
  return ram_[(pcPort_ & PC_RAM_BANK1) ? i + 128 : i];
}

void HT4Bit::writeRam(int rp, uint8_t v) {
  uint32_t i = ramIndex(rp);
  if (variant_ == HT943) ram_[i] = v;
  else if (i >= 128) lcd_[i - 128] = v;
  else ram_[(pcPort_ & PC_RAM_BANK1) ? i + 128 : i] = v;
}

void HT4Bit::interrupt(uint32_t location) {
  stack_ = ((uint32_t)cf_ << 12) | (pc_ & 0xFFF);
  pc_ = (pc_ & 0xF000) | location;
}

// ---- sound generator: 16 melodies stored in a sound ROM, one note per step
void HT4Bit::soundOff() {
  sndOn_ = 0;
  if (audio) audio->stop(0);
}

void HT4Bit::soundChannel(uint8_t ch) {
  sndOn_ = 1; sndNote_ = 0; sndChannel_ = ch;
}

void HT4Bit::soundClock(int cycles) {
  if (!sndOn_) return;
  sndCounter_ -= cycles;
  if (sndCounter_ > 0) return;
  uint32_t size = SINGLE_SIZE_CHANNEL_SIZE * ((sndChannel_ >= SINGLE_SIZE_CHANNEL_COUNT) + 1);
  if (sndNote_ >= size) {
    sndNote_ = 0;
    if (!sndRepeat_) { sndOn_ = 0; if (audio) audio->stop(0); }
    return;
  }
  sndCounter_ += (int32_t)(LFSR2DIV[speedDiv_[sndChannel_]] * soundFreqDiv_ * 16);
  uint32_t offset = sndChannel_ * SINGLE_SIZE_CHANNEL_SIZE;
  if (sndChannel_ > SINGLE_SIZE_CHANNEL_COUNT) offset += (sndChannel_ - SINGLE_SIZE_CHANNEL_COUNT) * SINGLE_SIZE_CHANNEL_SIZE;
  uint8_t note = srom_[offset + sndNote_];
  uint32_t div = note < 128 ? LFSR2DIV[note] : 0;
  if (audio) {
    if (div) audio->tone(0, (uint32_t)(((uint64_t)clockHz * 512) / ((uint64_t)soundFreqDiv_ * div)), effect_[sndChannel_] & 1, SOUND_AMP);
    else audio->stop(0);
  }
  sndNote_++;
}

// ---- one instruction
int HT4Bit::step() {
  int cycles = 8;
  if (!(halt_ | resetLine_)) {
    if (ei_ && stack_ == 0) {
      if (ef_) { ef_ = 0; interrupt(EXTERNAL_INT_LOCATION); }
      else if (tf_) { tf_ = 0; interrupt(TIMER_INT_LOCATION); }
    }

    const uint8_t op = fetch(pc_);
    cycles = 4;
#define IMM() (fetch(pc_ + 1))
#define ARITH(expr) { uint32_t r = (expr); cf_ = r > 15; acc_ = r & 0xF; }
#define JUMP_IF(cond) { uint8_t al = IMM(); pc_ += 2; if (cond) pc_ = (pc_ & 0xF800) | ((uint32_t)(op & 7) << 8) | al; cycles = 8; }
    switch (op) {
      case 0x00: cf_ = acc_ & 1; acc_ = (uint8_t)((cf_ << 3) | (acc_ >> 1)); pc_++; break;                 // RR A
      case 0x01: cf_ = acc_ >> 3; acc_ = (uint8_t)(cf_ | ((acc_ << 1) & 0xF)); pc_++; break;               // RL A
      case 0x02: { uint8_t c = acc_ & 1; acc_ = (uint8_t)((cf_ << 3) | (acc_ >> 1)); cf_ = c; pc_++; break; }   // RRC A
      case 0x03: { uint8_t c = acc_ >> 3; acc_ = (uint8_t)(cf_ | ((acc_ << 1) & 0xF)); cf_ = c; pc_++; break; } // RLC A
      case 0x04: acc_ = readRam(0); pc_++; break;
      case 0x05: writeRam(0, acc_); pc_++; break;
      case 0x06: acc_ = readRam(2); pc_++; break;
      case 0x07: writeRam(2, acc_); pc_++; break;
      case 0x08: ARITH(acc_ + readRam(0) + cf_); pc_++; break;                                             // ADC
      case 0x09: ARITH(acc_ + readRam(0)); pc_++; break;                                                   // ADD
      case 0x0A: ARITH(acc_ + (readRam(0) ^ 0xF) + cf_); pc_++; break;                                     // SBC
      case 0x0B: ARITH(acc_ + (readRam(0) ^ 0xF) + 1); pc_++; break;                                       // SUB
      case 0x0C: writeRam(0, (readRam(0) + 1) & 0xF); pc_++; break;
      case 0x0D: writeRam(0, (readRam(0) - 1) & 0xF); pc_++; break;
      case 0x0E: writeRam(2, (readRam(2) + 1) & 0xF); pc_++; break;
      case 0x0F: writeRam(2, (readRam(2) - 1) & 0xF); pc_++; break;
      case 0x10: case 0x12: case 0x14: case 0x16: case 0x18: { int i = (op >> 1) & 7; wr_[i] = (wr_[i] + 1) & 0xF; pc_++; break; }
      case 0x11: case 0x13: case 0x15: case 0x17: case 0x19: { int i = (op >> 1) & 7; wr_[i] = (wr_[i] - 1) & 0xF; pc_++; break; }
      case 0x1A: acc_ &= readRam(0); pc_++; break;
      case 0x1B: acc_ ^= readRam(0); pc_++; break;
      case 0x1C: acc_ |= readRam(0); pc_++; break;
      case 0x1D: writeRam(0, readRam(0) & acc_); pc_++; break;
      case 0x1E: writeRam(0, readRam(0) ^ acc_); pc_++; break;
      case 0x1F: writeRam(0, readRam(0) | acc_); pc_++; break;
      case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: wr_[(op >> 1) & 7] = acc_; pc_++; break;
      case 0x21: case 0x23: case 0x25: case 0x27: case 0x29: acc_ = wr_[(op >> 1) & 7]; pc_++; break;
      case 0x2A: cf_ = 0; pc_++; break;
      case 0x2B: cf_ = 1; pc_++; break;
      case 0x2C: ei_ = 1; pc_++; break;
      case 0x2D: ei_ = 0; pc_++; break;
      case 0x2E: pc_ = (pc_ & 0xF000) | (stack_ & 0xFFF); stack_ = 0; break;                               // RET
      case 0x2F: pc_ = (pc_ & 0xF000) | (stack_ & 0xFFF); cf_ = (uint8_t)(stack_ >> 12); stack_ = 0; break; // RETI
      case 0x30: pa_ = acc_; pc_++; break;                                                                 // OUT PA,A
      case 0x31: acc_ = (acc_ + 1) & 0xF; pc_++; break;
      case 0x32: acc_ = port_[PORT_PM]; pc_++; break;                                                      // IN A,PM
      case 0x33: acc_ = port_[PORT_PS]; pc_++; break;                                                      // IN A,PS
      case 0x34:
        if (variant_ == HT943) acc_ = port_[PORT_PP];                                                      // IN A,PP
        else pcPort_ = acc_;                                                                               // OUT PC,A
        pc_++; break;
      case 0x35:
        if (variant_ == HTG12N0) { pb_ = acc_; pc_ = ((uint32_t)(pb_ & 3) << 12) | (pc_ & 0xFFF); }        // OUT PB,A (ROM bank)
        pc_++; break;
      case 0x36: if (acc_ > 9 || cf_) { acc_ = (acc_ + 6) & 0xF; cf_ = 1; } pc_++; break;                  // DAA
      case 0x37: pc_ += 2; halt_ = 1; ef_ = 0; soundOff(); cycles = 8; break;                              // HALT
      case 0x38: timerOn_ = 1; pc_++; break;
      case 0x39: timerOn_ = 0; pc_++; break;
      case 0x3A: acc_ = tc_ & 0xF; pc_++; break;
      case 0x3B: acc_ = (tc_ >> 4) & 0xF; pc_++; break;
      case 0x3C: tc_ = (uint8_t)((tc_ & 0xF0) | acc_); pc_++; break;
      case 0x3D: tc_ = (uint8_t)((tc_ & 0x0F) | (acc_ << 4)); pc_++; break;
      case 0x3E: pc_++; break;                                                                             // NOP
      case 0x3F: acc_ = (acc_ - 1) & 0xF; pc_++; break;
      case 0x40: ARITH(acc_ + (IMM() & 0xF)); pc_ += 2; cycles = 8; break;
      case 0x41: ARITH(acc_ + ((IMM() ^ 0xF) & 0xFF) + 1); pc_ += 2; cycles = 8; break;
      case 0x42: acc_ &= IMM() & 0xF; pc_ += 2; cycles = 8; break;
      case 0x43: acc_ ^= IMM() & 0xF; pc_ += 2; cycles = 8; break;
      case 0x44: acc_ |= IMM() & 0xF; pc_ += 2; cycles = 8; break;
      case 0x45: soundChannel(IMM() & 0xF); pc_ += 2; cycles = 8; break;
      case 0x46: wr_[4] = IMM() & 0xF; pc_ += 2; cycles = 8; break;
      case 0x47: tc_ = IMM(); pc_ += 2; cycles = 8; break;
      case 0x48: sndRepeat_ = 0; pc_++; break;
      case 0x49: sndRepeat_ = 1; pc_++; break;
      case 0x4A: soundOff(); pc_++; break;
      case 0x4B: soundChannel(acc_); pc_++; break;
      case 0x4C: { pc_++; uint8_t b = fetch((pc_ & 0xFF00) | ((uint32_t)acc_ << 4) | readRam(0)); acc_ = b & 0xF; wr_[4] = b >> 4; cycles = 8; break; }
      case 0x4D: { pc_++; uint8_t b = fetch((pc_ & 0xF000) | 0xF00 | ((uint32_t)acc_ << 4) | readRam(0)); acc_ = b & 0xF; wr_[4] = b >> 4; cycles = 8; break; }
      case 0x4E: { pc_++; uint8_t b = fetch((pc_ & 0xFF00) | ((uint32_t)acc_ << 4) | wr_[4]); acc_ = b & 0xF; writeRam(0, b >> 4); cycles = 8; break; }
      case 0x4F: { pc_++; uint8_t b = fetch((pc_ & 0xF000) | 0xF00 | ((uint32_t)acc_ << 4) | wr_[4]); acc_ = b & 0xF; writeRam(0, b >> 4); cycles = 8; break; }
      default:
        switch (op >> 4) {
          case 0x5: wr_[0] = op & 0xF; wr_[1] = IMM() & 0xF; pc_ += 2; cycles = 8; break;
          case 0x6: wr_[2] = op & 0xF; wr_[3] = IMM() & 0xF; pc_ += 2; cycles = 8; break;
          case 0x7: acc_ = op & 0xF; pc_++; break;
          case 0x8: case 0x9: JUMP_IF(acc_ & (1 << ((op >> 3) & 3))); break;
          case 0xA: if (op & 8) JUMP_IF(wr_[1]) else JUMP_IF(wr_[0]); break;
          case 0xB: if (op & 8) JUMP_IF(acc_) else JUMP_IF(acc_ == 0); break;
          case 0xC: if (op & 8) JUMP_IF(!cf_) else JUMP_IF(cf_); break;
          case 0xD:
            if (op & 8) JUMP_IF(wr_[4])
            else { uint8_t al = IMM(); pc_ += 2; if (tf_) { pc_ = (pc_ & 0xF800) | ((uint32_t)(op & 7) << 8) | al; tf_ = 0; } cycles = 8; }
            break;
          case 0xE: pc_ = (pc_ & 0xF000) | ((uint32_t)(op & 0xF) << 8) | IMM(); cycles = 8; break;         // JMP
          case 0xF: stack_ = (pc_ + 2) & 0xFFF; pc_ = (pc_ & 0xF000) | ((uint32_t)(op & 0xF) << 8) | IMM(); cycles = 8; break;  // CALL
        }
    }
#undef IMM
#undef ARITH
#undef JUMP_IF

    soundClock(cycles);

    timerCounter_ -= cycles;
    while (timerCounter_ <= 0) {
      timerCounter_ += (int32_t)timerDiv_;
      if (timerOn_ && ++tc_ == 0) tf_ = 1;
    }
    instr_++;
  }

  if (variant_ == HTG12N0) {
    // real-time clock interrupt from the 32768 Hz crystal; runs even while halted
    rtcCounter_ -= (int64_t)cycles * SUB_CLOCK;
    const int64_t period = (int64_t)clockHz * rtcClockDiv_;
    while (rtcCounter_ <= 0) {
      rtcCounter_ += period > 0 ? period : 1;
      ef_ = 1; halt_ = 0;
    }
  }
  return cycles;
}

void HT4Bit::sync(State& s) {
  s.v(pc_); s.v(stack_); s.v(instr_);
  s.v(acc_); s.bytes(wr_, 5); s.v(ei_); s.v(cf_); s.v(tf_); s.v(ef_); s.v(halt_); s.v(resetLine_); s.v(timerOn_); s.v(tc_);
  s.v(timerCounter_);
  s.bytes(ram_, 256); s.bytes(lcd_, VRAM_SIZE);
  s.v(pa_); s.v(pb_); s.v(pcPort_); s.bytes(port_, 3);
  s.v(rtcCounter_);
  s.v(sndCounter_); s.v(sndNote_); s.v(sndChannel_); s.v(sndRepeat_); s.v(sndOn_);
}

size_t HT4Bit::debugRegs(uint32_t* out, size_t cap) const {
  // same order as BrickEmuPy's examine() for each chip
  size_t n = 0;
#define PUT(x) { if (n < cap) out[n] = (uint32_t)(x); n++; }
  PUT(acc_); PUT(pc_ & (variant_ == HT943 ? 0xFFF : 0x3FFF)); PUT(stack_); PUT(tc_); PUT(cf_); PUT(ef_); PUT(tf_); PUT(ei_); PUT(halt_);
  for (int i = 0; i < 5; i++) PUT(wr_[i]);
  if (variant_ == HT943) { PUT(port_[PORT_PP]); PUT(port_[PORT_PM]); PUT(port_[PORT_PS]); PUT(pa_); }
  else { PUT(port_[PORT_PM]); PUT(port_[PORT_PS]); PUT(pa_); PUT(pb_); PUT(pcPort_); }
  for (int i = 0; i < 256; i++) PUT(ram_[i]);
  if (variant_ == HTG12N0) for (int i = 0; i < VRAM_SIZE; i++) PUT(lcd_[i]);
#undef PUT
  return n;
}

}  // namespace brick
