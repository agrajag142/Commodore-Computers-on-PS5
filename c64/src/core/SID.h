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
// Rewritten October 2026 for the Waveshare ESP32-P4 port: the SID sound chip
// now works the way the real one does inside (see the notes at the top of
// SID.cpp). The class keeps its old public interface for the rest of the
// emulator.
#ifndef SID_H
#define SID_H

#include "Config.h"
#include "sound/SoundDriver.h"
#include <cstdint>

// Which SID chip to sound like at start-up: 6581 (the original C64, which
// most games were written for) or 8580 (the later C64C). Can be set in
// P4Settings.h, and changed while running with SID::setModel().
#ifndef SID_MODEL
#define SID_MODEL 6581
#endif

// Each output sample is the average of this many steps of the oscillators,
// which takes the harsh edge (aliasing) off high notes. 1, 2, 4 or 8.
#ifndef SID_OVERSAMPLE
#define SID_OVERSAMPLE 4
#endif

class SIDVoice {
public:
  enum EnvState : uint8_t { ATTACK, DECAY_SUSTAIN, RELEASE };

  // oscillator: the SID's 24 bit phase accumulator, kept here shifted
  // left by 8 so that one full wave is exactly 2^32
  uint32_t acc;
  uint32_t inc;      // added each oscillator step
  uint32_t prevAcc;  // value before the last step (for sync)
  uint16_t pw;       // pulse width, 12 bits
  uint8_t control;
  uint32_t lfsr;     // noise shift register, 23 bits
  uint16_t noiseOut; // 12 bit noise output
  uint16_t waveOut;  // last 12 bit waveform output (for $D41B)

  // envelope generator
  EnvState state;
  uint16_t rateCounter;
  uint16_t ratePeriod;
  uint8_t expCounter;
  uint8_t expPeriod;
  uint8_t env; // 0-255
  bool holdZero;
  uint8_t attack, decay, sustain, release;

  SIDVoice *syncSource; // the voice that syncs / ring-modulates this one

  SIDVoice();
  void init();
  void setFrequency(uint16_t freq);
  void setPulseWidth(uint16_t p);
  void setControl(uint8_t val);
  void setAD(uint8_t val);
  void setSR(uint8_t val);
  void clockEnvelope(uint32_t cycles);
  inline void clockOsc() __attribute__((always_inline));
  inline uint16_t output() __attribute__((always_inline));
};

struct CutoffPoint;
struct StateWriter;
struct StateReader;

class SID {
private:
  static constexpr uint8_t VOLUME_MULTIPLICATOR = 120;
  static const uint16_t NUMSAMPLESPERFRAME = AUDIO_SAMPLE_RATE / 50;
  static const uint32_t CYCLESPERFRAME = 312 * 63;
  int16_t samples[NUMSAMPLESPERFRAME];
  SoundDriver *sound;
  uint16_t actSampleIdx;

  // envelope timing: SID cycles per output sample, in 1/65536ths
  uint32_t envCycleStep;
  uint32_t envCycleFrac;

  // filter (state variable filter, coefficients worked out when the
  // filter registers change)
  bool filterDirty;
  float fG, fK, fA1, fA2, fA3;
  float fIc1, fIc2;
  // output DC block (the C64's audio output is AC coupled)
  float dcX1, dcY1;
  float volume; // master volume, 0-1
  // chip model (setModel)
  int model = 6581;
  const CutoffPoint *curve = nullptr;
  size_t curveLen = 0;
  float resStep = 0.0667f;
  float digiLevel = 0.4f;

  void updateFilter();
  int16_t generateSample();

public:
  SIDVoice sidVoice[3];
  float emuVolume;
  uint8_t emuVolumeScaled;
  uint8_t sidreg[0x20];

  SID();
  void initSID();
  void init();
  // write / read a SID register (0x00-0x1f)
  void writeReg(uint8_t idx, uint8_t val);
  uint8_t readReg(uint8_t idx);
  // produce the sound up to this point of the frame
  void clockTo(uint16_t rasterline, uint8_t cycle);
  void fillBuffer(uint16_t rasterline);
  void playAudio();
  uint8_t getEmuVolume();
  // 6581 or 8580 (SID_MODEL at start); can be changed while running
  void setModel(int model);
  int getModel() const { return model; }
  void setEmuVolume(uint8_t volume);
  // the chip's state, for C64Sys::saveState (the volume knob is kept)
  void saveState(StateWriter &w) const;
  void loadState(StateReader &r);
};
#endif // SID_H
