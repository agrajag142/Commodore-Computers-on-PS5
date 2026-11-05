// PS5Drivers.cpp
// The emulator's drivers on the PS5 (made by the factories in src/core):
//
//   display   keeps the C64 picture for ps5_main.cpp to draw onto the TV
//   sound     the SID's samples (48 kHz, mono) through a ring buffer to a
//             thread that feeds AudioOut (stereo, 256 frames at a time)
//   keyboard  the key and joysticks the controller gives (ps5_app.h)
//   joystick  none of its own: the joysticks come through the keyboard
//             driver (getTouchJoy), as the Waveshare port's on-screen one did

#include "../core/display/DisplayFactory.h"
#include "../core/joystick/JoystickFactory.h"
#include "../core/keyboard/KeyboardFactory.h"
#include "../core/sound/SoundFactory.h"
#include "../core/Config.h"
#include "ps5_app.h"
#include "ps5_sce.h"

#include <stdio.h>
#include <string.h>

uint8_t g_c64Pixels[320 * 200];
uint8_t g_c64Border = 14;
std::atomic<uint32_t> g_c64Key{0x00ffff};
std::atomic<uint8_t> g_c64Joy[2] = {{0xff}, {0xff}};

// ============================================================ display
namespace {
class PS5Display : public DisplayDriver {
public:
  void init() override {}
  void drawFrame(uint8_t frameColor) override { g_c64Border = frameColor & 15; }
  void drawBitmap(const uint8_t *bitmap) override { memcpy(g_c64Pixels, bitmap, sizeof(g_c64Pixels)); }
};
} // namespace

namespace Display {
DisplayDriver *create() { return new PS5Display(); }
} // namespace Display

// ============================================================ sound
namespace {
const int kGrain = 256;                 // frames per AudioOut call
const int kRing = 8192;                 // samples (about 170 ms)
const int kKeep = 2400;                 // most kept waiting (50 ms): older ones go
int16_t ring[kRing];
std::atomic<int> ringHead{0}, ringTail{0}; // head: next to write, tail: next to play
std::atomic<int> volumePct{80};
int audioPort = -1;

void audioThread(void *) {
  static int16_t out[kGrain * 2];
  int16_t last = 0;
  while (true) {
    int vol = volumePct.load(std::memory_order_relaxed);
    int tail = ringTail.load(std::memory_order_relaxed);
    int head = ringHead.load(std::memory_order_acquire);
    for (int i = 0; i < kGrain; i++) {
      int16_t s;
      if (tail != head) {
        s = ring[tail];
        tail = (tail + 1) % kRing;
        last = s;
      } else {
        s = last = (int16_t)(last * 15 / 16); // (ran dry: fade, no click)
      }
      int v = s * vol / 100;
      out[i * 2] = out[i * 2 + 1] = (int16_t)v;
    }
    ringTail.store(tail, std::memory_order_release);
    if (audioPort < 0 || sceAudioOutOutput(audioPort, out) < 0) {
      sceKernelUsleep(kGrain * 1000000 / 48000); // no output: keep time anyway
    }
  }
}

class PS5Sound : public SoundDriver {
public:
  void init() override {
    int r = sceAudioOutInit();
    if (r != 0 && (unsigned)r != 0x8026000eu) { // (already started is fine)
      printf("C64: AudioOut init failed %x\n", (unsigned)r);
    }
    audioPort = sceAudioOutOpen(0xff, 0, 0, kGrain, 48000, 1); // the main port, s16 stereo
    if (audioPort < 0) ps5Notify("Commodore 64: no sound output");
    ps5StartThread(audioThread, nullptr, 64 * 1024);
  }

  // One frame of SID samples (mono, 48 kHz), from the emulator's thread
  void playAudio(int16_t *samples, size_t size) override {
    int n = (int)(size / sizeof(int16_t));
    int head = ringHead.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
      ring[head] = samples[i];
      head = (head + 1) % kRing;
    }
    ringHead.store(head, std::memory_order_release);
    // too far ahead of the speaker (the emulator's clock and the sound's
    // drift apart a little): let the oldest go, so the delay stays short
    int tail = ringTail.load(std::memory_order_acquire);
    int queued = (head - tail + kRing) % kRing;
    if (queued > kKeep + 1200) {
      int drop = queued - kKeep;
      ringTail.compare_exchange_strong(tail, (tail + drop) % kRing);
    }
  }
};
} // namespace

// The last n samples the emulator made (oldest first), for the music
// player's oscilloscope
int ps5SoundRecent(int16_t *out, int n) {
  int head = ringHead.load(std::memory_order_acquire);
  if (n > kRing) n = kRing;
  for (int i = 0; i < n; i++) out[i] = ring[(head - n + i + kRing) % kRing];
  return n;
}

void ps5SoundSetVolume(int percent) {
  volumePct.store(percent < 0 ? 0 : percent > 100 ? 100 : percent, std::memory_order_relaxed);
}

namespace Sound {
SoundDriver *create() { return new PS5Sound(); }
} // namespace Sound

// ============================================================ keyboard
namespace {
class PS5Keyboard : public KeyboardDriver {
public:
  void init() override {}
  void scanKeyboard() override {}
  uint8_t getKBCodeDC01() override { return (uint8_t)(g_c64Key.load(std::memory_order_acquire) >> 8); }
  uint8_t getKBCodeDC00() override { return (uint8_t)g_c64Key.load(std::memory_order_acquire); }
  uint8_t getShiftctrlcode() override { return (uint8_t)(g_c64Key.load(std::memory_order_acquire) >> 16); }
  uint8_t getTouchJoy(uint8_t port) override {
    return g_c64Joy[port == 1 ? 0 : 1].load(std::memory_order_acquire);
  }
};
} // namespace

namespace Keyboard {
KeyboardDriver *create() { return new PS5Keyboard(); }
} // namespace Keyboard

// ============================================================ joystick
namespace Joystick {
JoystickDriver *create() { return new JoystickDriver(); }
} // namespace Joystick
