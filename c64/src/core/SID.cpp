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

// Rewritten October 2026 for the Waveshare ESP32-P4 port.
//
// The SID emulation now follows the structure of the real chip (as described
// by its designer Bob Yannes and reproduced in the reSID emulator by Dag
// Lem, whose filter cutoff curves are used below). What changed, and what it
// fixes:
//
// Oscillators
// - 24 bit phase accumulators as on the chip, so pitch is exact
// - the noise generator is clocked 16 times per wave, as on the chip (it was
//   clocked once per wave, so noise - drums, explosions, hi-hats - sounded
//   four octaves too low and rumbly)
// - waveform combinations (e.g. triangle + pulse) are ANDed like the chip
//   does, instead of averaged
// - changing the waveform no longer restarts the note and its envelope:
//   music players change waveforms every frame (drums, wave tables), and
//   each change used to click and restart the attack
// - the test bit holds the oscillator only while it is set (it used to stop
//   the voice until the frequency was written again)
// - hard sync works for all three voices, voice 1 included
// - each output sample averages SID_OVERSAMPLE oscillator steps, which
//   removes most of the harsh aliasing on high notes
//
// Envelopes
// - the real envelope generator: an 8 bit level, rate counters with the
//   chip's periods, exponential decay and release. Notes no longer restart
//   from silence when retriggered (that clicked), the decay rate no longer
//   depends on the order the registers were written in, and the "ADSR bug"
//   timing music players are written around behaves as on the chip
//
// Mixing
// - the three voices are mixed at fixed levels. Before, the output was
//   divided by the number of voices playing, so a voice suddenly got half as
//   loud when another one started, with a click at every change
// - the filter ($D415-$D418: cutoff, resonance, routing, low/band/high pass)
//   is emulated; it was missing, and most C64 music uses it
// - the master volume register adds a small DC level, as on the 6581: this
//   is how games play speech and sampled drums, which were silent before
// - register writes take effect at the right moment within the raster line
//   (the sound is produced up to the moment of each write), instead of all
//   at the end of the line
// - $D41B (oscillator 3) and $D41C (envelope 3) read back real values;
//   $D41C always returned 0 or 255 before

#include "SID.h"
#include "platform/PlatformManager.h"
#include "sound/SoundFactory.h"
#include <cmath>

static const double SID_CLOCK = 985248.0; // PAL

// rate counter periods (in cycles) for the 16 attack / decay / release rates
static const uint16_t ratePeriods[16] = {9,    32,   63,   95,    149,   220,
                                         267,  313,  392,  977,   1954,  3126,
                                         3907, 11720, 19532, 31251};

// Filter cutoff in Hz for the 11 bit cutoff register value (from reSID)
struct CutoffPoint {
  uint16_t fc;
  uint16_t hz;
};
static const CutoffPoint cutoff8580[] = {
    {0, 0},        {128, 800},    {256, 1600},   {384, 2500},  {512, 3300},
    {640, 4100},   {768, 4800},   {896, 5600},   {1024, 6500}, {1152, 7500},
    {1280, 8400},  {1408, 9200},  {1536, 9800},  {1664, 10500},
    {1792, 11000}, {1920, 11700}, {2047, 12500}};
static const CutoffPoint cutoff6581[] = {
    {0, 220},      {128, 230},    {256, 250},    {384, 300},    {512, 420},
    {640, 780},    {768, 1600},   {832, 2300},   {896, 3200},   {960, 4300},
    {992, 5000},   {1008, 5400},  {1016, 5700},  {1023, 6000},  {1024, 4600},
    {1032, 4800},  {1056, 5300},  {1088, 6000},  {1120, 6600},  {1152, 7200},
    {1280, 9500},  {1408, 12000}, {1536, 14500}, {1664, 16000}, {1792, 17100},
    {1920, 17700}, {2047, 18100}};

// The two chips differ in filter cutoff curve, resonance range (Q 0.7-1.7
// on the 6581, 0.7-3.7 on the 8580) and the DC level that makes sampled
// sound audible (8580 samples are very quiet).
void SID::setModel(int m) {
  model = (m == 8580) ? 8580 : 6581;
  if (model == 8580) {
    curve = cutoff8580;
    curveLen = sizeof(cutoff8580) / sizeof(cutoff8580[0]);
    resStep = 0.2f;
    digiLevel = 0.04f;
  } else {
    curve = cutoff6581;
    curveLen = sizeof(cutoff6581) / sizeof(cutoff6581[0]);
    resStep = 0.0667f;
    digiLevel = 0.4f;
  }
  filterDirty = true;
}

// overall level: one voice at full volume gives this fraction of the old
// emulation's output, which leaves room for three voices plus the filter
static const float OUTPUT_GAIN = 0.6f;

static const float DC_BLOCK_R = 0.999f; // high pass at about 7 Hz

// --------------------------------------------------------------- SIDVoice

SIDVoice::SIDVoice() { init(); }

void SIDVoice::init() {
  acc = 0;
  inc = 0;
  prevAcc = 0;
  pw = 0;
  control = 0;
  lfsr = 0x7ffff8;
  noiseOut = 0;
  waveOut = 0;
  state = RELEASE;
  rateCounter = 0;
  ratePeriod = ratePeriods[0];
  expCounter = 0;
  expPeriod = 1;
  env = 0;
  holdZero = true;
  attack = decay = sustain = release = 0;
}

void SIDVoice::setFrequency(uint16_t freq) {
  double step = SID_CLOCK / ((double)AUDIO_SAMPLE_RATE * SID_OVERSAMPLE);
  inc = (uint32_t)((double)freq * step * 256.0 + 0.5);
}

void SIDVoice::setPulseWidth(uint16_t p) { pw = p & 0x0fff; }

void SIDVoice::setControl(uint8_t val) {
  bool oldGate = control & 0x01;
  bool newGate = val & 0x01;
  if (!oldGate && newGate) {
    // attack starts from the current level, not from silence
    state = ATTACK;
    ratePeriod = ratePeriods[attack];
    holdZero = false;
  } else if (oldGate && !newGate) {
    state = RELEASE;
    ratePeriod = ratePeriods[release];
  }
  if (val & 0x08) {
    // test bit: oscillator held at zero, noise register reset
    acc = 0;
    lfsr = 0x7ffff8;
  }
  control = val;
}

void SIDVoice::setAD(uint8_t val) {
  attack = val >> 4;
  decay = val & 0x0f;
  if (state == ATTACK) {
    ratePeriod = ratePeriods[attack];
  } else if (state == DECAY_SUSTAIN) {
    ratePeriod = ratePeriods[decay];
  }
}

void SIDVoice::setSR(uint8_t val) {
  sustain = val >> 4;
  release = val & 0x0f;
  if (state == RELEASE) {
    ratePeriod = ratePeriods[release];
  }
}

// Run the envelope generator for the given number of SID cycles.
// The 15 bit rate counter counts cycles; each time it reaches the period of
// the current rate the envelope steps (in decay and release only every
// expPeriod-th time, which makes them exponential). If the period is set
// below the counter, the counter runs round through 0x7fff first - the
// "ADSR bug" of the real chip.
void SIDVoice::clockEnvelope(uint32_t cycles) {
  while (cycles) {
    uint32_t toHit = ((uint32_t)(ratePeriod - rateCounter - 1) & 0x7fff) + 1;
    if (cycles < toHit) {
      rateCounter = (rateCounter + cycles) & 0x7fff;
      return;
    }
    cycles -= toHit;
    rateCounter = 0;
    if ((state != ATTACK) && (++expCounter != expPeriod)) {
      continue;
    }
    expCounter = 0;
    if (holdZero) {
      continue;
    }
    switch (state) {
    case ATTACK:
      env++;
      if (env == 0xff) {
        state = DECAY_SUSTAIN;
        ratePeriod = ratePeriods[decay];
      }
      break;
    case DECAY_SUSTAIN:
      if (env != sustain * 0x11) {
        env--;
      }
      break;
    case RELEASE:
      env--;
      break;
    }
    switch (env) {
    case 0xff:
      expPeriod = 1;
      break;
    case 0x5d:
      expPeriod = 2;
      break;
    case 0x36:
      expPeriod = 4;
      break;
    case 0x1a:
      expPeriod = 8;
      break;
    case 0x0e:
      expPeriod = 16;
      break;
    case 0x06:
      expPeriod = 30;
      break;
    case 0x00:
      expPeriod = 1;
      holdZero = true;
      break;
    }
  }
}

// one oscillator step
void SIDVoice::clockOsc() {
  prevAcc = acc;
  if (control & 0x08) {
    acc = 0;
    return;
  }
  uint32_t n = acc + inc; // wraps round at the end of each wave
  // the noise register is clocked when bit 19 of the 24 bit accumulator
  // (bit 27 here) goes from 0 to 1: 16 times per wave. Shifted by half a
  // step, those moments are where bits 28-31 change.
  uint32_t e = (((n + 0x08000000u) >> 28) - ((acc + 0x08000000u) >> 28)) & 0x0f;
  if (e > 0) {
    for (; e > 0; e--) {
      uint32_t bit0 = ((lfsr >> 22) ^ (lfsr >> 17)) & 1;
      lfsr = ((lfsr << 1) & 0x7fffff) | bit0;
    }
    noiseOut = ((lfsr & 0x400000) >> 11) | ((lfsr & 0x100000) >> 10) |
               ((lfsr & 0x010000) >> 7) | ((lfsr & 0x002000) >> 5) |
               ((lfsr & 0x000800) >> 4) | ((lfsr & 0x000080) >> 1) |
               ((lfsr & 0x000010) << 1) | ((lfsr & 0x000004) << 2);
  }
  acc = n;
}

// 12 bit waveform output; 0x800 is the middle
uint16_t SIDVoice::output() {
  uint8_t w = control >> 4;
  if (w == 0) {
    waveOut = 0;
    return 0x800;
  }
  uint32_t a = acc >> 8;
  uint16_t o = 0x0fff;
  if (w & 1) {
    // triangle; ring modulation flips it with the other voice's top bit
    uint32_t m = (control & 0x04) ? (a ^ (syncSource->acc >> 8)) : a;
    o &= (((m & 0x800000) ? ~a : a) >> 11) & 0x0fff;
  }
  if (w & 2) {
    o &= a >> 12; // sawtooth
  }
  if (w & 4) {
    o &= ((control & 0x08) || ((a >> 12) >= pw)) ? 0x0fff : 0; // pulse
  }
  if (w & 8) {
    o &= noiseOut;
  }
  waveOut = o;
  return o;
}

// -------------------------------------------------------------------- SID

void SID::initSID() {
  setEmuVolume(Config::DEFAULT_VOLUME);
  actSampleIdx = 0;
  for (uint16_t i = 0; i < NUMSAMPLESPERFRAME; i++) {
    samples[i] = 0;
  }
  for (uint8_t i = 0; i < 0x20; i++) {
    sidreg[i] = 0;
  }
  for (int i = 0; i < 3; i++) {
    sidVoice[i].init();
  }
  // voice 1 is synced by voice 3, voice 2 by voice 1, voice 3 by voice 2
  sidVoice[0].syncSource = &sidVoice[2];
  sidVoice[1].syncSource = &sidVoice[0];
  sidVoice[2].syncSource = &sidVoice[1];
  envCycleStep = (uint32_t)(SID_CLOCK / AUDIO_SAMPLE_RATE * 65536.0 + 0.5);
  envCycleFrac = 0;
  volume = 0.0f;
  fIc1 = fIc2 = 0.0f;
  dcX1 = dcY1 = 0.0f;
  filterDirty = true;
  updateFilter();
}

SID::SID() : sound(nullptr) {
  setModel(SID_MODEL);
  initSID();
}

void SID::init() {
  sound = Sound::create();
  sound->init();
  initSID();
}

void SID::updateFilter() {
  filterDirty = false;
  uint16_t fc = ((uint16_t)sidreg[0x16] << 3) | (sidreg[0x15] & 7);
  const size_t n = curveLen;
  const CutoffPoint *cutoffCurve = curve;
  float hz = cutoffCurve[n - 1].hz;
  for (size_t i = 1; i < n; i++) {
    if (fc <= cutoffCurve[i].fc) {
      const CutoffPoint &p0 = cutoffCurve[i - 1];
      const CutoffPoint &p1 = cutoffCurve[i];
      float t = (p1.fc == p0.fc) ? 1.0f
                                 : (float)(fc - p0.fc) / (float)(p1.fc - p0.fc);
      hz = p0.hz + t * ((float)p1.hz - (float)p0.hz);
      break;
    }
  }
  if (hz < 30.0f) {
    hz = 30.0f;
  }
  float maxHz = 0.45f * AUDIO_SAMPLE_RATE;
  if (hz > maxHz) {
    hz = maxHz;
  }
  fG = tanf(3.14159265f * hz / AUDIO_SAMPLE_RATE);
  float q = 0.707f + (float)(sidreg[0x17] >> 4) * resStep;
  fK = 1.0f / q;
  fA1 = 1.0f / (1.0f + fG * (fG + fK));
  fA2 = fG * fA1;
  fA3 = fG * fA2;
}

int16_t SID::generateSample() {
  int32_t sum[3] = {0, 0, 0};
  for (uint8_t s = 0; s < SID_OVERSAMPLE; s++) {
    sidVoice[0].clockOsc();
    sidVoice[1].clockOsc();
    sidVoice[2].clockOsc();
    // hard sync: reset a voice when its sync source's top bit goes high
    for (uint8_t v = 0; v < 3; v++) {
      SIDVoice &dst = sidVoice[v];
      if (dst.control & 0x02) {
        SIDVoice *src = dst.syncSource;
        if (!(src->prevAcc & 0x80000000u) && (src->acc & 0x80000000u)) {
          dst.acc = 0;
        }
      }
    }
    sum[0] += (int32_t)sidVoice[0].output() - 0x800;
    sum[1] += (int32_t)sidVoice[1].output() - 0x800;
    sum[2] += (int32_t)sidVoice[2].output() - 0x800;
  }

  // envelopes, run for the SID cycles this sample covers
  envCycleFrac += envCycleStep;
  uint32_t cycles = envCycleFrac >> 16;
  envCycleFrac &= 0xffff;
  const float scale = 1.0f / (2048.0f * SID_OVERSAMPLE * 255.0f);
  float vo[3];
  for (uint8_t v = 0; v < 3; v++) {
    sidVoice[v].clockEnvelope(cycles);
    vo[v] = (float)sum[v] * (float)sidVoice[v].env * scale;
  }

  // routing: through the filter or straight to the output
  uint8_t route = sidreg[0x17];
  uint8_t mode = sidreg[0x18];
  float filtIn = 0.0f;
  float direct = 0.0f;
  for (uint8_t v = 0; v < 3; v++) {
    if (route & (1 << v)) {
      filtIn += vo[v];
    } else if (!((v == 2) && (mode & 0x80))) { // bit 7: voice 3 off
      direct += vo[v];
    }
  }

  // state variable filter (trapezoidal integration: stable at any cutoff)
  if (filterDirty) {
    updateFilter();
  }
  float v3 = filtIn - fIc2;
  float v1 = fA1 * fIc1 + fA2 * v3;
  float v2 = fIc2 + fA2 * fIc1 + fA3 * v3;
  fIc1 = 2.0f * v1 - fIc1;
  fIc2 = 2.0f * v2 - fIc2;
  if (fabsf(fIc1) < 1e-12f) {
    fIc1 = 0.0f;
  }
  if (fabsf(fIc2) < 1e-12f) {
    fIc2 = 0.0f;
  }
  float filtered = 0.0f;
  if (mode & 0x10) {
    filtered += v2; // low pass
  }
  if (mode & 0x20) {
    filtered += v1; // band pass
  }
  if (mode & 0x40) {
    filtered += filtIn - fK * v1 - v2; // high pass
  }

  float out = (direct + filtered + digiLevel) * volume;

  // remove the DC level (the C64's output is AC coupled)
  float y = out - dcX1 + DC_BLOCK_R * dcY1;
  dcX1 = out;
  dcY1 = (fabsf(y) < 1e-12f) ? 0.0f : y;

  float f = y * emuVolume * OUTPUT_GAIN;
  if (f > 32767.0f) {
    f = 32767.0f;
  } else if (f < -32767.0f) {
    f = -32767.0f;
  }
  return (int16_t)f;
}

void SID::writeReg(uint8_t idx, uint8_t val) {
  idx &= 0x1f;
  sidreg[idx] = val;
  if (idx <= 0x14) {
    uint8_t voice = idx / 7;
    uint8_t base = voice * 7;
    SIDVoice &v = sidVoice[voice];
    switch (idx - base) {
    case 0:
    case 1:
      v.setFrequency(sidreg[base] | (sidreg[base + 1] << 8));
      break;
    case 2:
    case 3:
      v.setPulseWidth(sidreg[base + 2] | (sidreg[base + 3] << 8));
      break;
    case 4:
      v.setControl(val);
      break;
    case 5:
      v.setAD(val);
      break;
    case 6:
      v.setSR(val);
      break;
    }
  } else if (idx <= 0x17) {
    filterDirty = true;
  } else if (idx == 0x18) {
    volume = (float)(val & 0x0f) / 15.0f;
  }
}

uint8_t SID::readReg(uint8_t idx) {
  idx &= 0x1f;
  if (idx == 0x1b) {
    // oscillator 3: top 8 bits of its waveform. Without a waveform the
    // value is random, as programs often use this register for random
    // numbers.
    if ((sidVoice[2].control & 0xf0) == 0) {
      return PlatformManager::getInstance().getRandomByte();
    }
    return sidVoice[2].waveOut >> 4;
  } else if (idx == 0x1c) {
    return sidVoice[2].env; // envelope 3
  }
  return sidreg[idx];
}

// Produce the samples due up to the given moment of the frame, so that a
// register write that follows takes effect at the right time.
void SID::clockTo(uint16_t rasterline, uint8_t cycle) {
  if (cycle > 63) {
    cycle = 63;
  }
  uint32_t pos = (uint32_t)rasterline * 63 + cycle;
  uint32_t target = pos * NUMSAMPLESPERFRAME / CYCLESPERFRAME;
  if (target > NUMSAMPLESPERFRAME) {
    target = NUMSAMPLESPERFRAME;
  }
  while (actSampleIdx < target) {
    samples[actSampleIdx++] = generateSample();
  }
}

void SID::fillBuffer(uint16_t rasterline) { clockTo(rasterline, 63); }

void SID::playAudio() {
  while (actSampleIdx < NUMSAMPLESPERFRAME) {
    samples[actSampleIdx++] = generateSample();
  }
  if (sound != nullptr) {
    sound->playAudio(samples, NUMSAMPLESPERFRAME * sizeof(int16_t));
  }
  actSampleIdx = 0;
}

uint8_t SID::getEmuVolume() { return emuVolumeScaled; }

void SID::setEmuVolume(uint8_t volume) {
  emuVolumeScaled = volume;
  emuVolume = (float)(emuVolumeScaled * VOLUME_MULTIPLICATOR);
}

// ---------------------------------------------------------------- state
// Added for the Waveshare ESP32-P4 port (SID player screen): the whole chip
// as bytes. Only the emulator task uses it, and the voices' sync pointers
// point inside the object, so a copy back into the same object is exact.
// The volume knob and the sound driver stay as they are now.
#include "StateBuf.h"

void SID::saveState(StateWriter &w) const { w.bytes(this, sizeof(SID)); }

void SID::loadState(StateReader &r) {
  SoundDriver *keepSound = sound;
  float keepVol = emuVolume;
  uint8_t keepVolScaled = emuVolumeScaled;
  r.bytes((void *)this, sizeof(SID));
  sound = keepSound;
  emuVolume = keepVol;
  emuVolumeScaled = keepVolScaled;
}
