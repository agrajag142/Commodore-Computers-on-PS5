/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 For the complete text of the GNU General Public License see
 http://www.gnu.org/licenses/.
*/
#include "CIA.h"

// bit 4 of ciareg[0x0e] and ciareg[0x0f] is handled in CPUC64::setMem

void CIA::checkAlarm() {
  if (isAlarm.load(std::memory_order_acquire)) {
    isAlarm.store(false, std::memory_order_release);
    latchdc0d |= 0x04;
    if (ciareg[0x0d] & 4) {
      latchdc0d |= 0x80;
    }
  }
}

// Changed October 2026 in the Waveshare ESP32-P4 port: the timers are
// advanced in steps of about 32 cycles (half a raster line). The original
// treated "reached 0 during this step" as the underflow and threw away the
// rest of the step, so every timer period was rounded up to the next step:
// a timer set to 100 cycles ran every 128. Short timers (sample players,
// fast music routines) ran at the wrong rate. Now the cycles left over are
// carried into the next period (a period is latch + 1 cycles, as on the
// real chip) and several underflows in one step are counted.

// timer A underflowed "count" times in this step
void CIA::timerAUnderflow(uint8_t count) {
  uint8_t reg0e = ciareg[0x0e];
  underflowTimerA += count;
  if (reg0e & 0x02) {
    if (!(reg0e & 0x04)) {
      if (count & 1) {
        ciareg[0x01] ^= 0x40;
      }
    }
    // ignore "toggle bit for one cycle"
  }
  latchdc0d |= 0x01;
  if (ciareg[0x0d] & 1) {
    latchdc0d |= 0x80;
  }
  if (reg0e & 0x40) {
    for (uint8_t i = 0; (i < count) && (serbitnr != 0); i++) {
      serbitnr--;
      if (serbitnr == 0) {
        latchdc0d |= 0x08;
        if (ciareg[0x0d] & 8) {
          latchdc0d |= 0x80;
        }
        if (serbitnrnext != 0) {
          serbitnr = serbitnrnext;
          serbitnrnext = 0;
        }
      }
    }
  }
}

void CIA::checkTimerA(uint8_t deltaT) {
  uint8_t reg0e = ciareg[0x0e];
  if (!(reg0e & 1)) {
    // timer stopped
    return;
  }
  if (reg0e & 0x20) {
    // timer clocked by CNT pin
    return;
  }
  int32_t tmp = (int32_t)timerA - deltaT;
  if (tmp >= 0) {
    timerA = tmp;
    return;
  }
  uint16_t latch = (latchdc05 << 8) + latchdc04;
  if (reg0e & 8) {
    // one-shot: reload and stop
    timerA = latch;
    ciareg[0x0e] &= 0xfe;
    timerAUnderflow(1);
    return;
  }
  int32_t period = (int32_t)latch + 1;
  uint8_t count = 1;
  tmp += period;
  if (tmp < 0) {
    int32_t more = (-tmp + period - 1) / period;
    tmp += more * period;
    count += (more > 254) ? 254 : more;
  }
  timerA = tmp;
  timerAUnderflow(count);
}

void CIA::checkTimerB(uint8_t deltaT) {
  uint8_t reg0f = ciareg[0x0f];
  uint8_t countsA = underflowTimerA;
  underflowTimerA = 0;
  if (!(reg0f & 1)) {
    // timer stopped
    return;
  }
  uint8_t bit56 = reg0f & 0x60;
  int32_t steps;
  if (bit56 == 0) {
    steps = deltaT;
  } else if (bit56 == 0x40) {
    steps = countsA;
  } else {
    return;
  }
  if (steps == 0) {
    return;
  }
  int32_t tmp = (int32_t)timerB - steps;
  if (tmp >= 0) {
    timerB = tmp;
    return;
  }
  uint16_t latch = (latchdc07 << 8) + latchdc06;
  uint8_t count = 1;
  if (reg0f & 8) {
    timerB = latch;
    ciareg[0x0f] &= 0xfe;
  } else {
    int32_t period = (int32_t)latch + 1;
    tmp += period;
    if (tmp < 0) {
      int32_t more = (-tmp + period - 1) / period;
      tmp += more * period;
      count += (more > 254) ? 254 : more;
    }
    timerB = tmp;
  }
  if (reg0f & 0x02) {
    if (!(reg0f & 0x04)) {
      if (count & 1) {
        ciareg[0x01] ^= 0x80;
      }
    }
    // ignore "toggle bit for one cycle"
  }
  latchdc0d |= 0x02;
  if (ciareg[0x0d] & 2) {
    latchdc0d |= 0x80;
  }
}

void CIA::init(bool isCIA1) {
  for (uint8_t i = 0; i < 0x10; i++) {
    ciareg[i] = 0;
  }

  underflowTimerA = 0;
  serbitnr = 0;
  serbitnrnext = 0;
  latchdc04 = 0;
  latchdc05 = 0;
  latchdc06 = 0;
  latchdc07 = 0;
  latchdc0d = 0;
  timerA = 0xffff;
  timerB = 0xffff;

  isTODFreezed = false;
  isAlarm.store(false, std::memory_order_release);
  latchrundc08.store(0, std::memory_order_release);
  latchrundc09.store(0, std::memory_order_release);
  latchrundc0a.store(0, std::memory_order_release);
  latchrundc0b.store(0, std::memory_order_release);
  latchalarmdc08.store(0, std::memory_order_release);
  latchalarmdc09.store(0, std::memory_order_release);
  latchalarmdc0a.store(0, std::memory_order_release);
  latchalarmdc0b.store(0, std::memory_order_release);
  todSeq.store(0, std::memory_order_release);

  if (isCIA1) {
    ciareg[0] = 127;
    ciareg[1] = 255;
  } else {
    ciareg[0] = 151;
    ciareg[1] = 255;
  }
}

CIA::CIA(bool isCIA1) {
  init(isCIA1);
  isTODRunning.store(false, std::memory_order_release);
}

uint8_t CIA::getCommonCIAReg(uint8_t ciaidx) {
  if (ciaidx == 0x04) {
    return timerA & 0xff;
  } else if (ciaidx == 0x05) {
    return (timerA >> 8) & 0xff;
  } else if (ciaidx == 0x06) {
    return timerB & 0xff;
  } else if (ciaidx == 0x07) {
    return (timerB >> 8) & 0xff;
  } else if (ciaidx == 0x08) {
    uint8_t val;
    if (isTODFreezed) {
      val = ciareg[ciaidx];
    } else {
      val = latchrundc08.load(std::memory_order_acquire);
    }
    isTODFreezed = false;
    return val;
  } else if (ciaidx == 0x09) {
    if (isTODFreezed) {
      return ciareg[ciaidx];
    } else {
      return latchrundc09.load(std::memory_order_acquire);
    }
  } else if (ciaidx == 0x0a) {
    if (isTODFreezed) {
      return ciareg[ciaidx];
    } else {
      return latchrundc0a.load(std::memory_order_acquire);
    }
  } else if (ciaidx == 0x0b) {
    uint16_t s;
    do {
      s = todSeq.load(std::memory_order_acquire);
      ciareg[0x08] = latchrundc08.load(std::memory_order_acquire);
      ciareg[0x09] = latchrundc09.load(std::memory_order_acquire);
      ciareg[0x0a] = latchrundc0a.load(std::memory_order_acquire);
      ciareg[0x0b] = latchrundc0b.load(std::memory_order_acquire);
    } while (s != todSeq.load(std::memory_order_acquire) || (s & 1));
    isTODFreezed = true;
    return ciareg[ciaidx];
  } else if (ciaidx == 0x0d) {
    uint8_t val = latchdc0d;
    latchdc0d = 0;
    return val;
  } else {
    return ciareg[ciaidx];
  }
}

void CIA::setCommonCIAReg(uint8_t ciaidx, uint8_t val) {
  if (ciaidx == 0x04) {
    latchdc04 = val;
  } else if (ciaidx == 0x05) {
    latchdc05 = val;
    // timerA stopped? if yes, write timerA
    if (!(ciareg[0x0e] & 1)) {
      timerA = (latchdc05 << 8) + latchdc04;
    }
  } else if (ciaidx == 0x06) {
    latchdc06 = val;
  } else if (ciaidx == 0x07) {
    latchdc07 = val;
    // timerB stopped? if yes, write timerB
    if (!(ciareg[0x0f] & 1)) {
      timerB = (latchdc07 << 8) + latchdc06;
    }
  } else if (ciaidx == 0x08) {
    if (ciareg[0x0f] & 128) {
      latchalarmdc08.store(val, std::memory_order_release);
    } else {
      ciareg[0x08] = val;
      latchrundc08.store(val, std::memory_order_release);
      latchrundc09.store(ciareg[0x09], std::memory_order_release);
      latchrundc0a.store(ciareg[0x0a], std::memory_order_release);
      latchrundc0b.store(ciareg[0x0b], std::memory_order_release);
      isTODRunning.store(true, std::memory_order_release);
    }
  } else if (ciaidx == 0x09) {
    if (ciareg[0x0f] & 128) {
      latchalarmdc09.store(val, std::memory_order_release);
    } else {
      ciareg[0x09] = val;
    }
  } else if (ciaidx == 0x0a) {
    if (ciareg[0x0f] & 128) {
      latchalarmdc0a.store(val, std::memory_order_release);
    } else {
      ciareg[0x0a] = val;
    }
  } else if (ciaidx == 0x0b) {
    if (ciareg[0x0f] & 128) {
      latchalarmdc0b.store(val, std::memory_order_release);
    } else {
      isTODRunning.store(false, std::memory_order_release);
      ciareg[0x0b] = val;
    }
  } else if (ciaidx == 0x0c) {
    if (serbitnr == 0) {
      serbitnr = 8;
    } else {
      serbitnrnext = 8;
    }
    ciareg[ciaidx] = val;
  } else if (ciaidx == 0x0d) {
    if (val & 0x80) {
      ciareg[ciaidx] |= val;
    } else {
      ciareg[ciaidx] &= ~(val | 0x80);
    }
  } else if (ciaidx == 0x0e) {
    ciareg[ciaidx] = val;
    if (val & 0x10) {
      timerA = (latchdc05 << 8) + latchdc04;
    }
  } else if (ciaidx == 0x0f) {
    ciareg[ciaidx] = val;
    if (val & 0x10) {
      timerB = (latchdc07 << 8) + latchdc06;
    }
  } else {
    ciareg[ciaidx] = val;
  }
}

bool CIA::updateTODInt() {
  todSeq.fetch_add(1, std::memory_order_release);
  uint8_t dc08 = latchrundc08.load(std::memory_order_acquire);
  dc08++;
  if (dc08 > 9) {
    dc08 = 0;
    uint8_t dc09 = latchrundc09.load(std::memory_order_acquire);
    uint8_t dc09one = dc09 & 15;
    uint8_t dc09ten = dc09 >> 4;
    dc09one++;
    if (dc09one > 9) {
      dc09one = 0;
      dc09ten++;
      if (dc09ten > 5) {
        dc09ten = 0;
        uint8_t dc0a = latchrundc0a.load(std::memory_order_acquire);
        uint8_t dc0aone = dc0a & 15;
        uint8_t dc0aten = dc0a >> 4;
        dc0aone++;
        if (dc0aone > 9) {
          dc0aone = 0;
          dc0aten++;
          if (dc0aten > 5) {
            dc0aten = 0;
            uint8_t dc0b = latchrundc0b.load(std::memory_order_acquire);
            uint8_t dc0bone = dc0b & 15;
            uint8_t dc0bten = dc0b >> 4;
            bool pm = dc0b & 128;
            dc0bone++;
            if (((dc0bten == 0) && (dc0bone > 9)) ||
                (dc0bten && (dc0bone > 1))) {
              dc0bone = 0;
              dc0bten++;
              if (dc0bten > 1) {
                dc0bten = 0;
                pm = !pm;
              }
            }
            latchrundc0b.store(dc0bone | (dc0bten << 4) | (pm ? 128 : 0),
                               std::memory_order_release);
          }
        }
        latchrundc0a.store(dc0aone | (dc0aten << 4), std::memory_order_release);
      }
    }
    latchrundc09.store(dc09one | (dc09ten << 4), std::memory_order_release);
  }
  latchrundc08.store(dc08, std::memory_order_release);
  todSeq.fetch_add(1, std::memory_order_release);
  uint8_t alarmdc08 = latchalarmdc08.load(std::memory_order_acquire);
  if (dc08 == alarmdc08) {
    uint8_t dc09 = latchrundc09.load(std::memory_order_acquire);
    uint8_t alarmdc09 = latchalarmdc09.load(std::memory_order_acquire);
    if (dc09 == alarmdc09) {
      uint8_t dc0a = latchrundc0a.load(std::memory_order_acquire);
      uint8_t alarmdc0a = latchalarmdc0a.load(std::memory_order_acquire);
      if (dc0a == alarmdc0a) {
        uint8_t dc0b = latchrundc0b.load(std::memory_order_acquire);
        uint8_t alarmdc0b = latchalarmdc0b.load(std::memory_order_acquire);
        if (dc0b == alarmdc0b) {
          return true;
        }
      }
    }
  }
  return false;
}

void CIA::updateTOD() {
  if (isTODRunning.load(std::memory_order_acquire)) {
    if (updateTODInt()) {
      isAlarm.store(true, std::memory_order_release);
    }
  }
}

// ---------------------------------------------------------------- state
// Added for the Waveshare ESP32-P4 port (SID player screen)
#include "StateBuf.h"

void CIA::saveState(StateWriter &w) const {
  w.put(ciareg);
  w.put(underflowTimerA);
  w.put(serbitnr);
  w.put(serbitnrnext);
  w.put(latchdc04);
  w.put(latchdc05);
  w.put(latchdc06);
  w.put(latchdc07);
  w.put(latchdc0d);
  w.put(timerA);
  w.put(timerB);
  w.put(isTODRunning);
  w.put(isTODFreezed);
  w.put(isAlarm);
  w.put(latchrundc08);
  w.put(latchrundc09);
  w.put(latchrundc0a);
  w.put(latchrundc0b);
  w.put(latchalarmdc08);
  w.put(latchalarmdc09);
  w.put(latchalarmdc0a);
  w.put(latchalarmdc0b);
}

void CIA::loadState(StateReader &r) {
  r.get(ciareg);
  r.get(underflowTimerA);
  r.get(serbitnr);
  r.get(serbitnrnext);
  r.get(latchdc04);
  r.get(latchdc05);
  r.get(latchdc06);
  r.get(latchdc07);
  r.get(latchdc0d);
  r.get(timerA);
  r.get(timerB);
  r.get(isTODRunning);
  r.get(isTODFreezed);
  r.get(isAlarm);
  r.get(latchrundc08);
  r.get(latchrundc09);
  r.get(latchrundc0a);
  r.get(latchrundc0b);
  r.get(latchalarmdc08);
  r.get(latchalarmdc09);
  r.get(latchalarmdc0a);
  r.get(latchalarmdc0b);
}
