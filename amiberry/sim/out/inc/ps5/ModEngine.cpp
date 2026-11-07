// ModEngine.cpp
// ProTracker MOD replayer. See ModEngine.h.
//
// Follows ProTracker 2's way of playing: a row every "speed" ticks, a tick
// every 2.5 / tempo seconds; notes start on tick 0, slides, vibrato and the
// like work on the other ticks. Periods are Paula's (PAL clock 3546895 Hz).

#include "ModEngine.h"
#include <cmath>
#include <cstring>

static const double PAULA_CLOCK = 3546895.0;

// ProTracker's periods, finetune 0, C-1 to B-3
static const int16_t PERIODS0[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};
static int16_t periodTab[16][36]; // [finetune + 8][note]
static bool tabReady = false;

static void makeTables() {
  for (int ft = -8; ft < 8; ft++) {
    for (int n = 0; n < 36; n++) {
      periodTab[ft + 8][n] = (int16_t)lround(PERIODS0[n] * pow(2.0, -ft / 96.0));
    }
  }
  tabReady = true;
}

static const uint8_t SINE[32] = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                 224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                 212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

static int nearestNote(const int16_t *tab, int period) {
  int best = 0, bestD = 1 << 30;
  for (int i = 0; i < 36; i++) {
    int d = period - tab[i];
    if (d < 0) d = -d;
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

void ModEngine::noteName(int period, char out[4]) {
  static const char *names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  if (period <= 0) {
    strcpy(out, "---");
    return;
  }
  int n = nearestNote(PERIODS0, period);
  out[0] = names[n % 12][0];
  out[1] = names[n % 12][1];
  out[2] = (char)('1' + n / 12);
  out[3] = 0;
}

int ModEngine::periodFor(int note, int finetune, int semitones) const {
  int n = nearestNote(PERIODS0, note) + semitones;
  if (n < 0) n = 0;
  if (n > 35) n = 35;
  return periodTab[finetune + 8][n];
}

const char *ModEngine::sampleName(int s) const {
  if (s < 1 || s > nsamples || !samples[s].name) return "";
  return samples[s].name;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

const char *ModEngine::load(const uint8_t *data, size_t len) {
  if (!tabReady) makeTables();
  unload();
  if (data == nullptr || len < 600) return "NOT A MOD FILE (TOO SHORT)";

  // which kind: the tag at 1080 says how many channels; no tag = 15 samples
  int chans = 0;
  char tag[5] = {0};
  if (len >= 1084) {
    memcpy(tag, data + 1080, 4);
    if (!strcmp(tag, "M.K.") || !strcmp(tag, "M!K!") || !strcmp(tag, "FLT4") ||
        !strcmp(tag, "4CHN") || !strcmp(tag, "M&K!") || !strcmp(tag, "N.T.")) {
      chans = 4;
    } else if (!strcmp(tag, "FLT8") || !strcmp(tag, "OKTA") || !strcmp(tag, "CD81")) {
      chans = 8;
    } else if (tag[0] >= '1' && tag[0] <= '9' && !strcmp(tag + 1, "CHN")) {
      chans = tag[0] - '0';
    } else if (tag[0] >= '0' && tag[0] <= '9' && tag[1] >= '0' && tag[1] <= '9' && tag[2] == 'C' &&
               tag[3] == 'H') {
      chans = (tag[0] - '0') * 10 + (tag[1] - '0');
    }
  }
  int ns;
  size_t ordOff, patOff;
  if (chans > 0) {
    if (chans > MAX_CH) return "MORE THAN 8 CHANNELS - NOT SUPPORTED";
    ns = 31;
    ordOff = 952;
    patOff = 1084;
  } else {
    chans = 4; // Soundtracker: 15 samples, no tag
    ns = 15;
    ordOff = 472;
    patOff = 600;
    strcpy(tag, "ST15");
  }
  int songLen = data[ordOff - 2];
  int restart = data[ordOff - 1];
  if (songLen < 1 || songLen > 128) return "NOT A MOD FILE";

  // patterns: as many as the highest number in the order list (all 128, as
  // ProTracker does, unless that runs past the end of the file)
  const uint8_t *ord = data + ordOff;
  size_t patBytes = (size_t)64 * chans * 4;
  int maxAll = 0, maxSong = 0;
  for (int i = 0; i < 128; i++) {
    if (ord[i] > maxAll) maxAll = ord[i];
    if (i < songLen && ord[i] > maxSong) maxSong = ord[i];
  }
  if (ns == 15 && maxAll > 63) return "NOT A MOD FILE";
  int npat = maxAll + 1;
  if (patOff + (size_t)npat * patBytes > len) npat = maxSong + 1;
  if (patOff + (size_t)npat * patBytes > len) return "MOD FILE IS CUT SHORT";

  // samples: their data follows the patterns, one after the other
  size_t cur = patOff + (size_t)npat * patBytes;
  for (int s = 1; s <= ns; s++) {
    const uint8_t *h = data + 20 + (s - 1) * 30;
    Sample &S = samples[s];
    S.name = (const char *)h;
    uint32_t length = (uint32_t)be16(h + 22) * 2;
    int ft = h[24] & 15;
    S.finetune = ft > 7 ? ft - 16 : ft;
    S.volume = h[25] > 64 ? 64 : h[25];
    uint32_t ls = be16(h + 26), ll = (uint32_t)be16(h + 28) * 2;
    S.loopStart = ns == 15 ? ls : ls * 2; // Soundtracker counts this one in bytes
    S.loopLength = ll;
    S.data = (const int8_t *)(data + cur);
    uint32_t avail = cur < len ? (uint32_t)(len - cur) : 0;
    cur += length;
    if (length > avail) length = avail; // cut-short file: play what is there
    S.length = length;
    if (S.loopLength > 2) {
      if (S.loopStart >= S.length) {
        S.loopLength = 0; // the loop is past the end: no loop
      } else {
        if (S.loopStart + S.loopLength > S.length) S.loopLength = S.length - S.loopStart;
        S.length = S.loopStart + S.loopLength; // after the loop end is never played
      }
    }
    if (S.length == 0) S.data = nullptr;
  }

  mod = data;
  modLen = len;
  orders = ord;
  patterns = data + patOff;
  numPatterns = npat;
  nsamples = ns;
  channels = chans;
  songLength = songLen;
  restartPos = restart < songLen ? restart : 0;
  memcpy(format, tag, 5);
  for (int i = 0; i < 20; i++) {
    char c = (char)data[i];
    title[i] = (c >= 32 && c < 127) ? c : (c == 0 ? 0 : ' ');
  }
  title[20] = 0;
  // trailing spaces off
  for (int i = (int)strlen(title) - 1; i >= 0 && title[i] == ' '; i--) title[i] = 0;
  reset();
  return nullptr;
}

void ModEngine::unload() {
  mod = nullptr;
  channels = 0;
  title[0] = 0;
  format[0] = 0;
  memset(samples, 0, sizeof(samples));
  memset(view, 0, sizeof(view));
}

void ModEngine::reset() {
  memset(ch, 0, sizeof(ch));
  memset(view, 0, sizeof(view));
  memset(visited, 0, sizeof(visited));
  for (int c = 0; c < MAX_CH; c++) {
    ch[c].delayTick = -1;
    ch[c].cutTick = -1;
  }
  tick = 0;
  speed = 6;
  tempo = 125;
  position = 0;
  row = 0;
  pattern = orders[0];
  samplesLeft = 0;
  patternDelay = patternDelayLeft = 0;
  breakPending = false;
  breakRow = 0;
  jumpOrder = -1;
  loopJumpRow = -1;
  inDelay = false;
  stopped = false;
  ended = false;
  playedSamples = 0;
  visited[0] = true;
}

void ModEngine::setPosition(int order) {
  if (!mod) return;
  if (order < 0) order = 0;
  if (order >= songLength) order = songLength - 1;
  for (int c = 0; c < channels; c++) {
    ch[c].loopRow = 0;
    ch[c].loopCount = 0;
  }
  memset(visited, 0, sizeof(visited));
  visited[order] = true;
  position = order;
  pattern = orders[order];
  row = 0;
  tick = 0;
  samplesLeft = 0;
  patternDelayLeft = 0;
  inDelay = false;
  breakPending = false;
  jumpOrder = -1;
  loopJumpRow = -1;
  stopped = false;
  ended = false;
}

// ---------------------------------------------------------------- notes
void ModEngine::triggerNote(int c) {
  Channel &C = ch[c];
  const Sample *s = (C.sampleNum >= 1 && C.sampleNum <= nsamples) ? &samples[C.sampleNum] : nullptr;
  C.period = periodFor(C.note, C.finetune, 0);
  C.smp = s;
  C.pos = 0;
  C.frac = 0;
  C.playing = s != nullptr && s->data != nullptr && s->length > 0;
  if (!(C.vibWave & 4)) C.vibPos = 0;
  if (!(C.tremWave & 4)) C.tremPos = 0;
}

void ModEngine::startRow() {
  pattern = orders[position];
  const uint8_t *p = patterns + ((size_t)pattern * 64 + row) * channels * 4;
  for (int c = 0; c < channels; c++, p += 4) {
    Channel &C = ch[c];
    int sampleNum = (p[0] & 0xF0) | (p[2] >> 4);
    int note = ((p[0] & 0x0F) << 8) | p[1];
    C.effect = p[2] & 0x0F;
    C.param = p[3];
    C.delayTick = -1;
    C.cutTick = -1;
    C.newNote = false;
    if (sampleNum >= 1 && sampleNum <= nsamples) {
      // a sample number sets the volume and finetune; the sample itself
      // starts with the next note
      C.sampleNum = sampleNum;
      C.volume = samples[sampleNum].volume;
      C.finetune = samples[sampleNum].finetune;
    }
    if (note > 0) {
      if (C.effect == 0xE && (C.param >> 4) == 5) {
        int ft = C.param & 15;
        C.finetune = ft > 7 ? ft - 16 : ft;
      }
      C.note = note;
      if (C.effect == 3 || C.effect == 5) {
        C.portaTarget = periodFor(note, C.finetune, 0); // slide to it, don't start it
      } else if (C.effect == 0xE && (C.param >> 4) == 0xD && (C.param & 15) > 0) {
        C.delayTick = C.param & 15; // note delay
      } else {
        triggerNote(c);
        C.newNote = true;
      }
    }
    rowEffectsTick0(c);
  }
}

static int clampVol(int v) { return v < 0 ? 0 : (v > 64 ? 64 : v); }

void ModEngine::rowEffectsTick0(int c) {
  Channel &C = ch[c];
  int x = C.param >> 4, y = C.param & 15;
  switch (C.effect) {
  case 0x3:
    if (C.param) C.portaSpeed = C.param;
    break;
  case 0x4:
    if (x) C.vibSpeed = (uint8_t)x;
    if (y) C.vibDepth = (uint8_t)y;
    break;
  case 0x7:
    if (x) C.tremSpeed = (uint8_t)x;
    if (y) C.tremDepth = (uint8_t)y;
    break;
  case 0x9: // sample offset
    if (C.param) C.offsetMem = (uint32_t)C.param << 8;
    if (C.newNote && C.smp) {
      C.pos = C.offsetMem;
      if (C.pos >= C.smp->length) {
        if (C.smp->loopLength > 2) C.pos = C.smp->loopStart;
        else C.playing = false;
      }
    }
    break;
  case 0xB: // position jump
    jumpOrder = C.param;
    breakPending = true;
    break;
  case 0xC:
    C.volume = clampVol(C.param);
    break;
  case 0xD: // pattern break (row in decimal)
    breakRow = x * 10 + y;
    if (breakRow > 63) breakRow = 0;
    breakPending = true;
    break;
  case 0xE:
    switch (x) {
    case 0x1:
      C.period -= y;
      if (C.period < 113) C.period = 113;
      break;
    case 0x2:
      C.period += y;
      if (C.period > 856) C.period = 856;
      break;
    case 0x4:
      C.vibWave = (uint8_t)y;
      break;
    case 0x6: // pattern loop
      if (y == 0) {
        C.loopRow = row;
      } else {
        if (C.loopCount == 0) C.loopCount = y;
        else C.loopCount--;
        if (C.loopCount > 0) loopJumpRow = C.loopRow;
      }
      break;
    case 0x7:
      C.tremWave = (uint8_t)y;
      break;
    case 0xA:
      C.volume = clampVol(C.volume + y);
      break;
    case 0xB:
      C.volume = clampVol(C.volume - y);
      break;
    case 0xC:
      C.cutTick = y;
      if (y == 0) C.volume = 0;
      break;
    case 0xE:
      if (!inDelay && patternDelayLeft == 0) patternDelayLeft = y;
      break;
    }
    break;
  case 0xF:
    if (C.param == 0) {
      ended = true; // F00: the end of the song
    } else if (C.param < 32) {
      speed = C.param;
    } else {
      tempo = C.param;
    }
    break;
  }
}

static int waveValue(uint8_t wave, uint8_t pos) {
  // 0 sine, 1 ramp down, 2 square, 3 (random) as sine; -255..255
  int i = pos & 31;
  int v;
  switch (wave & 3) {
  case 1:
    v = i * 8;
    return (pos & 32) ? 255 - v : -v; // ProTracker's ramp
  case 2:
    v = 255;
    break;
  default:
    v = SINE[i];
    break;
  }
  return (pos & 32) ? -v : v;
}

void ModEngine::effectsTick(int c) {
  Channel &C = ch[c];
  int x = C.param >> 4, y = C.param & 15;
  auto volSlide = [&](uint8_t prm) {
    int a = prm >> 4, b = prm & 15;
    if (a) C.volume = clampVol(C.volume + a);
    else C.volume = clampVol(C.volume - b);
  };
  auto tonePorta = [&]() {
    if (!C.portaTarget) return;
    if (C.period < C.portaTarget) {
      C.period += C.portaSpeed;
      if (C.period > C.portaTarget) C.period = C.portaTarget;
    } else if (C.period > C.portaTarget) {
      C.period -= C.portaSpeed;
      if (C.period < C.portaTarget) C.period = C.portaTarget;
    }
    C.outPeriod = C.period;
  };
  auto vibrato = [&]() {
    int delta = waveValue(C.vibWave, C.vibPos) * C.vibDepth / 128;
    C.outPeriod = C.period + delta;
    C.vibPos = (uint8_t)((C.vibPos + C.vibSpeed) & 63);
  };
  switch (C.effect) {
  case 0x0:
    if (C.param) {
      int semis = (tick % 3 == 1) ? x : (tick % 3 == 2) ? y : 0;
      int n = nearestNote(periodTab[C.finetune + 8], C.period) + semis;
      if (n > 35) n = 35;
      C.outPeriod = periodTab[C.finetune + 8][n];
    }
    break;
  case 0x1:
    C.period -= C.param;
    if (C.period < 113) C.period = 113;
    C.outPeriod = C.period;
    break;
  case 0x2:
    C.period += C.param;
    if (C.period > 856) C.period = 856;
    C.outPeriod = C.period;
    break;
  case 0x3:
    tonePorta();
    break;
  case 0x4:
    vibrato();
    break;
  case 0x5:
    tonePorta();
    volSlide(C.param);
    break;
  case 0x6:
    vibrato();
    volSlide(C.param);
    break;
  case 0x7: {
    int delta = waveValue(C.tremWave, C.tremPos) * C.tremDepth / 64;
    C.outVolume = clampVol(C.volume + delta);
    C.tremPos = (uint8_t)((C.tremPos + C.tremSpeed) & 63);
    return; // outVolume set
  }
  case 0xA:
    volSlide(C.param);
    break;
  case 0xE:
    if (x == 0x9 && y && tick % y == 0) { // retrigger
      C.pos = 0;
      C.frac = 0;
      C.playing = C.smp != nullptr && C.smp->data != nullptr;
    } else if (x == 0xC && tick == C.cutTick) {
      C.volume = 0;
    } else if (x == 0xD && tick == C.delayTick) {
      if (C.sampleNum >= 1 && C.sampleNum <= nsamples) C.volume = samples[C.sampleNum].volume;
      triggerNote(c);
    }
    break;
  }
  C.outVolume = C.volume;
}

void ModEngine::nextRow() {
  int newOrder;
  if (loopJumpRow >= 0) { // pattern loop: back within this pattern
    row = loopJumpRow;
    loopJumpRow = -1;
    breakPending = false;
    jumpOrder = -1;
    breakRow = 0;
    return;
  }
  if (breakPending) {
    newOrder = jumpOrder >= 0 ? jumpOrder : position + 1;
    row = breakRow;
    breakPending = false;
    jumpOrder = -1;
    breakRow = 0;
  } else {
    if (++row < 64) return;
    row = 0;
    newOrder = position + 1;
  }
  if (newOrder >= songLength) {
    newOrder = restartPos;
    ended = true; // back round to the start
  }
  if (visited[newOrder]) ended = true; // a jump back to a part already played
  visited[newOrder] = true;
  for (int c = 0; c < channels; c++) ch[c].loopRow = 0;
  position = newOrder;
  pattern = orders[position];
}

void ModEngine::doTick() {
  for (int c = 0; c < channels; c++) {
    ch[c].outPeriod = ch[c].period;
    ch[c].outVolume = ch[c].volume;
  }
  if (tick == 0) {
    if (inDelay) {
      // a row repeated by EEx: no new notes, nothing on tick 0
    } else {
      startRow();
    }
    for (int c = 0; c < channels; c++) {
      ch[c].outPeriod = ch[c].period;
      ch[c].outVolume = ch[c].volume;
    }
  } else {
    for (int c = 0; c < channels; c++) effectsTick(c);
  }
  for (int c = 0; c < channels; c++) {
    ChannelView &v = view[c];
    v.sample = ch[c].sampleNum;
    v.period = ch[c].playing ? ch[c].outPeriod : 0;
    v.volume = ch[c].outVolume;
    v.effect = ch[c].effect;
    v.param = ch[c].param;
  }
  if (++tick >= speed) {
    tick = 0;
    if (patternDelayLeft > 0) {
      patternDelayLeft--;
      inDelay = true; // the same row again
    } else {
      inDelay = false;
      nextRow();
    }
  }
}

// ---------------------------------------------------------------- sound
void ModEngine::render(int16_t *out, int n) {
  if (!mod || paused) {
    memset(out, 0, (size_t)n * 2);
    for (int c = 0; c < MAX_CH; c++) view[c].level = 0;
    return;
  }
  static int32_t mix[512];
  int done = 0;
  while (done < n) {
    if (samplesLeft <= 0) {
      doTick();
      samplesLeft = RATE * 5 / (tempo * 2); // 2.5 / tempo seconds
    }
    int k = n - done;
    if (k > samplesLeft) k = samplesLeft;
    if (k > 512) k = 512;
    memset(mix, 0, (size_t)k * 4);
    for (int c = 0; c < channels; c++) {
      Channel &C = ch[c];
      ChannelView &v = view[c];
      int level = 0;
      if (C.playing && C.smp && C.outPeriod > 0) {
        int per = C.outPeriod < 28 ? 28 : C.outPeriod;
        uint32_t step = (uint32_t)(PAULA_CLOCK * 65536.0 / ((double)per * RATE));
        const int8_t *d = C.smp->data;
        uint32_t len = C.smp->length, ls = C.smp->loopStart, ll = C.smp->loopLength;
        int vol = C.outVolume;
        for (int i = 0; i < k; i++) {
          int s = d[C.pos] * vol; // -8192..8128
          mix[i] += s;
          int a = s < 0 ? -s : s;
          if (a > level) level = a;
          if (!(i & 1)) {
            v.scope[v.scopePos] = (int8_t)(s >> 6);
            v.scopePos = (v.scopePos + 1) & (SCOPE_N - 1);
          }
          C.frac += step;
          C.pos += C.frac >> 16;
          C.frac &= 0xFFFF;
          if (C.pos >= len) {
            if (ll > 2) {
              C.pos = ls + (C.pos - len) % ll;
            } else {
              C.playing = false;
              break;
            }
          }
        }
      } else {
        for (int i = 0; i < k; i += 2) {
          v.scope[v.scopePos] = 0;
          v.scopePos = (v.scopePos + 1) & (SCOPE_N - 1);
        }
      }
      v.level = level >> 6;
    }
    // mono: every channel together, a little below full scale
    int shift = channels > 4 ? 1 : 0;
    for (int i = 0; i < k; i++) {
      int32_t s = (mix[i] * 3 / 4) >> shift;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      out[done + i] = (int16_t)s;
    }
    done += k;
    samplesLeft -= k;
    playedSamples += (uint32_t)k;
  }
}
