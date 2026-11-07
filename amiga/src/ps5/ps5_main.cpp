// ps5_main.cpp
// Amiga 500 for the PS5 - the program.
//
// The emulator (src/core: amiga500-esp32 by Massitux, with the Waveshare
// ESP32-P4 port's work - AmigaCore) runs one PAL frame every 20 ms on its own
// thread. This file:
//
//   the TV picture   the Amiga's 640 x 256 (lores pixels doubled) at 2 x 4:
//                    1280 x 1024, the full height of the TV; help at the sides
//   the DualSense    the mouse (touchpad or right stick; R1/R2/Square left
//                    button, L1/L2/Circle right) and the joystick in port 2
//                    (D-pad / left stick, Cross fires)
//   the menu         the disks (boot from one, or swap it in), reset, volume
//   the sound        Paula's stereo samples (48 kHz) to AudioOut
//
// The video output follows the PS5 starter's screen code
// (ps5-native-app-boilerplate's demo renderer, Copyright (C) 2026
// BlackBearReloaded; changes for Breeze, MiladRTX), as NUCLEAR MISSION's does.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../core/AmigaCore.h"
#include "../core/chips/a500.h"
#include "../stubs/esp_heap_caps.h"
#include "AmigaFont.h"
#include "ModEngine.h"
#include "MusicList.h"
#include "Visuals.h"
#include "UsbKeyboard.h"
#include "ps5_sce.h"
#include "ps5_system.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef AMIBERRY
// Amiberry's adapter: the models it can be, from the Kickstarts in the app
void amigaAddKickstart(const char *name, const uint8_t *data, size_t len);
int amigaModelCount();
const char *amigaModelName(int i);
const char *amigaModelKickstart(int i);
bool amigaModelAvailable(int i);
int amigaModel();
void amigaSetModel(int i);
static const char *modelLabel() { return amigaModelName(amigaModel()) + 1; } // (A1200: 1200)
#else
static const char *modelLabel() { return "500"; }
#endif
static const char *modelTitle() {
  static char t[32];
  snprintf(t, sizeof t, "Amiga %s on PS5", modelLabel());
  return t;
}

int paula_ring_avail(void);
uint8_t *drive_alloc_adf(void);
void drive_mount_ready(void);

// ============================================================ colours
// RGBA as the TV buffer takes it: 0xAABBGGRR
static constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | b << 16 | g << 8 | r; }
// Workbench 1.3: blue, white, black, orange
static const uint32_t kWbBlue = rgb(0x00, 0x55, 0xaa), kWbWhite = rgb(0xff, 0xff, 0xff),
                      kWbBlack = rgb(0x00, 0x00, 0x22), kWbOrange = rgb(0xff, 0x88, 0x00);
static const uint32_t kSideBg = rgb(0x12, 0x16, 0x22);
static const uint32_t kText = rgb(0xe4, 0xe8, 0xf0), kDim = rgb(0x8c, 0x96, 0xaa);
static const uint32_t kIconCross = rgb(0x7c, 0xb2, 0xe8), kIconCircle = rgb(0xff, 0x66, 0x6e),
                      kIconSquare = rgb(0xe0, 0x8c, 0xd2), kIconTriangle = rgb(0x40, 0xe0, 0xb0);

// ============================================================ the TV picture
static const int kW = 1920, kH = 1080;
static const int kBlock = 128, kBlocksX = kW / kBlock, kBlocksY = (kH + kBlock - 1) / kBlock;
static const size_t kFrameBytes = 0x1000000, kMemBytes = kFrameBytes * 2, kMemAlign = 0x200000;
static const size_t kUsedBytes = (size_t)kBlocksX * kBlocksY << 16;
uint32_t g_frame[kW * kH]; // the TV picture, row by row
static uint32_t lut565[65536];
static void *scanout[2];
static int video = -1;

static inline size_t tiledOffset(unsigned x, unsigned y) {
  const uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                          ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                          ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
  const uint32_t blockIndex = (y >> 7) * kBlocksX + (x >> 7);
  return ((size_t)blockIndex << 16) + offset;
}
static void copyBlocks(void *dst, int bx0, int bx1) {
  uint8_t *bytes = (uint8_t *)dst;
  for (int by = 0; by < kBlocksY; ++by)
    for (int bx = bx0; bx <= bx1; ++bx)
      for (int y0 = 0; y0 < kBlock; y0 += 8) {
        const int top = by * kBlock + y0;
        if (top >= kH) break;
        for (int x0 = 0; x0 < kBlock; x0 += 4) {
          const int left = bx * kBlock + x0;
          for (int yy = 0; yy < 8 && top + yy < kH; ++yy) {
            const uint32_t *src = g_frame + (top + yy) * kW + left;
            for (int xx = 0; xx < 4; ++xx)
              *(uint32_t *)(bytes + tiledOffset((unsigned)(left + xx), (unsigned)(top + yy))) = src[xx];
          }
        }
      }
}
static void flushRange(void *address, size_t length) {
#if defined(__x86_64__) && !defined(PS5_SIM)
  uint8_t *at = (uint8_t *)address, *end = at + length;
  for (; at < end; at += 64) __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
  __asm__ volatile("mfence" ::: "memory");
#else
  (void)address;
  (void)length;
#endif
}
static void flushBlocks(void *dst, int bx0, int bx1) {
  for (int by = 0; by < kBlocksY; ++by)
    flushRange((uint8_t *)dst + ((size_t)(by * kBlocksX + bx0) << 16), (size_t)(bx1 - bx0 + 1) << 16);
}

[[noreturn]] static void halt(const char *msg) {
  ps5Notify(msg);
  for (;;) sceKernelUsleep(1000000);
}

static void videoInit() {
  video = sceVideoOutOpen(0xff, 0, 0, nullptr);
  if (video < 0) halt("Amiga 500: sceVideoOutOpen failed");
  size_t pool = sceKernelGetDirectMemorySize();
  if (pool < kMemBytes) halt("Amiga 500: not enough direct memory");
  int64_t phys = 0;
  if (sceKernelAllocateDirectMemory(0, (int64_t)pool, kMemBytes, kMemAlign, 3 /* wc garlic */, &phys) < 0)
    halt("Amiga 500: direct-memory allocation failed");
  void *mapped = nullptr;
  if (sceKernelMapDirectMemory(&mapped, kMemBytes, 0x33, 0, phys, kMemAlign) < 0)
    halt("Amiga 500: direct-memory mapping failed");
  scanout[0] = mapped;
  scanout[1] = (uint8_t *)mapped + kFrameBytes;
  struct {
    void *data, *metadata, *r0, *r1;
  } bufs[2] = {{scanout[0], nullptr, nullptr, nullptr}, {scanout[1], nullptr, nullptr, nullptr}};
  alignas(16) uint8_t attribute[80] = {};
  sceVideoOutSetFlipRate(video, 0);
  sceVideoOutSetBufferAttribute2(attribute, UINT64_C(0x8000000022000000) /* rgba8 srgb */, 0, kW, kH, 0, 0, 0);
  if (sceVideoOutRegisterBuffers2(video, 0, 0, bufs, 2, attribute, 0, nullptr) < 0)
    halt("Amiga 500: buffer registration failed");
  for (int i = 0; i < 65536; i++) {
    uint32_t r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
    lut565[i] = rgb((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
  }
}

// ============================================================ drawing
static void fillRect(int x, int y, int w, int h, uint32_t c) {
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > kW) x1 = kW;
  if (y1 > kH) y1 = kH;
  for (int yy = y; yy < y1; yy++) {
    uint32_t *row = g_frame + yy * kW;
    for (int xx = x; xx < x1; xx++) row[xx] = c;
  }
}
static void frameRect(int x, int y, int w, int h, int t, uint32_t c) {
  fillRect(x, y, w, t, c);
  fillRect(x, y + h - t, w, t, c);
  fillRect(x, y, t, h, c);
  fillRect(x + w - t, y, t, h, c);
}
// The board's 8 x 16 font; scale x scale each pixel
static void text(int x, int y, const char *s, int scale, uint32_t col) {
  for (; *s; s++, x += 8 * scale) {
    unsigned ch = (unsigned char)*s;
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = amigaFont[ch - 32];
    for (int gy = 0; gy < 16; gy++)
      for (int gx = 0; gx < 8; gx++)
        if (g[gy] & (0x80 >> gx)) fillRect(x + gx * scale, y + gy * scale, scale, scale, col);
  }
}
static int textWidth(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

enum Icon { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE };
static void icon(Icon kind, int cx, int cy, int size) {
  int r = size / 2, t = size >= 24 ? 3 : 2;
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      bool on = false;
      uint32_t col = 0;
      int k = r * 7 / 10;
      switch (kind) {
      case ICON_CROSS:
        on = (abs(dx - dy) <= t / 2 + 1 || abs(dx + dy) <= t / 2 + 1) && abs(dx) <= k && abs(dy) <= k;
        col = kIconCross;
        break;
      case ICON_CIRCLE: {
        int d2 = dx * dx + dy * dy, ro = r * 8 / 10, ri = ro - t - 1;
        on = d2 <= ro * ro && d2 >= ri * ri;
        col = kIconCircle;
        break;
      }
      case ICON_SQUARE:
        on = (abs(dx) <= k && abs(dy) <= k) && (abs(dx) > k - t - 1 || abs(dy) > k - t - 1);
        col = kIconSquare;
        break;
      case ICON_TRIANGLE: {
        int top = -r * 7 / 10, bot = r * 6 / 10;
        if (dy < top || dy > bot) break;
        float f = (float)(dy - top) / (bot - top);
        float half = f * r * 0.8f;
        float edge = half - (dx < 0 ? -dx : dx);
        on = edge >= 0 && (edge <= t + 0.5f || dy >= bot - t);
        col = kIconTriangle;
        break;
      }
      }
      if (on) g_frame[(cy + dy) * kW + cx + dx] = col;
    }
}
static void buttonLabel(const char *name, int cx, int cy) {
  int w = textWidth(name, 1) + 10, h = 20;
  frameRect(cx - w / 2, cy - h / 2, w, h, 2, kDim);
  text(cx - textWidth(name, 1) / 2, cy - 8, name, 1, kText);
}

// ============================================================ the Amiga's picture
// 640 x 512 RGB565: each Amiga line written twice by AmigaCore
static uint16_t picture[640 * 512];
static const int kPicX = 320, kPicY = 28; // on the TV: 1280 x 1024

static void composePicture() {
  for (int r = 0; r < 512; r++) {
    const uint16_t *src = picture + r * 640;
    uint32_t *out = g_frame + (kPicY + r * 2) * kW + kPicX;
    for (int x = 0; x < 640; x++) {
      uint32_t c = lut565[src[x]];
      out[2 * x] = out[2 * x + 1] = c;
    }
    memcpy(out + kW, out, 1280 * 4);
  }
}

// With the on-screen keyboard: the picture 1.5 x 3 (960 x 768, the same
// shape), the keyboard under it
static const int kSmallX = 480, kSmallY = 8;
static void composePictureSmall() {
  static int srcX[960];
  if (!srcX[959])
    for (int x = 0; x < 960; x++) srcX[x] = x * 2 / 3;
  for (int r = 0; r < 256; r++) {
    const uint16_t *src = picture + r * 2 * 640;
    uint32_t *out = g_frame + (kSmallY + r * 3) * kW + kSmallX;
    for (int x = 0; x < 960; x++) out[x] = lut565[src[srcX[x]]];
    memcpy(out + kW, out, 960 * 4);
    memcpy(out + 2 * kW, out, 960 * 4);
  }
}

// ============================================================ the emulator
static uint8_t chipRam[0x80000], slowRam[0x80000], kickRom[0x40000];
static std::atomic<int> emuFps{0};
static std::atomic<bool> running{false};

static void emuThread(void *) {
  uint64_t next = ps5NowUs();
  uint64_t statStart = next;
  int frames = 0;
  while (true) {
    if (amigaPaused()) { // the MOD player is open
      sceKernelUsleep(20000);
      next = ps5NowUs();
      continue;
    }
    AmigaFrameTimes t;
    amigaRunFrame(&t);
    frames++;
    uint64_t now = ps5NowUs();
    if (now - statStart >= 1000000) {
      emuFps.store((int)(frames * 1000000ull / (now - statStart)));
      frames = 0;
      statStart = now;
    }
    // one frame every 20 ms, as a PAL Amiga (50 a second)
    next += 20000;
    if (next > now) sceKernelUsleep((uint32_t)(next - now));
    else if (now - next > 100000) next = now; // (far behind: no catching up)
  }
}

// ============================================================ sound
static const int kGrain = 256;
static int audioPort = -1;
static std::atomic<int> volumePct{80};

// The MOD player's engine; the sound thread renders it while the player is open
struct SpinLock {
  std::atomic_flag f = ATOMIC_FLAG_INIT;
  void lock() {
    while (f.test_and_set(std::memory_order_acquire)) {
    }
  }
  void unlock() { f.clear(std::memory_order_release); }
};
static ModEngine mod;
static SpinLock modLock;
static std::atomic<bool> modSound{false};
// the MOD player's sound as it goes out, for its oscilloscope
static int16_t modWave[2048];
static std::atomic<int> modWaveHead{0};

static void soundThread(void *) {
  static int16_t out[kGrain * 2];
  static int16_t mono[kGrain];
  while (true) {
    if (modSound.load(std::memory_order_acquire)) {
      modLock.lock();
      mod.render(mono, kGrain);
      modLock.unlock();
      int wh = modWaveHead.load(std::memory_order_relaxed);
      for (int i = 0; i < kGrain; i++) modWave[(wh + i) & 2047] = mono[i];
      modWaveHead.store((wh + kGrain) & 2047, std::memory_order_release);
      int vol = volumePct.load(std::memory_order_relaxed);
      for (int i = 0; i < kGrain; i++) out[2 * i] = out[2 * i + 1] = (int16_t)(mono[i] * vol / 100);
      if (audioPort < 0 || sceAudioOutOutput(audioPort, out) < 0) sceKernelUsleep(kGrain * 1000000 / 48000);
      continue;
    }
    // too far behind the emulator (its clock and the sound's drift apart a
    // little): skip ahead, so the delay stays short (about 50 ms)
    int avail = paula_ring_avail();
    if (avail > 2400 + 1200) {
      int16_t l, r;
      for (int i = avail - 2400; i > 0; i--) paula_ring_pop(&l, &r);
    }
    int vol = volumePct.load(std::memory_order_relaxed);
    for (int i = 0; i < kGrain; i++) {
      int16_t l, r;
      paula_ring_pop(&l, &r);
      // the Amiga's channels are hard left and right: mix a quarter across,
      // easier on headphones and a TV
      int ml = (l * 3 + r) / 4, mr = (r * 3 + l) / 4;
      out[2 * i] = (int16_t)(ml * vol / 100);
      out[2 * i + 1] = (int16_t)(mr * vol / 100);
    }
    if (audioPort < 0 || sceAudioOutOutput(audioPort, out) < 0) sceKernelUsleep(kGrain * 1000000 / 48000);
  }
}

static void soundStart() {
  int r = sceAudioOutInit();
  if (r != 0 && (unsigned)r != 0x8026000eu) printf("Amiga: AudioOut init failed %x\n", (unsigned)r);
  audioPort = sceAudioOutOpen(0xff, 0, 0, kGrain, 48000, 1); // the main port, s16 stereo
  if (audioPort < 0) ps5Notify("Amiga 500: no sound output");
  ps5StartThread(soundThread, nullptr, 64 * 1024);
}

// ============================================================ disks
static std::vector<std::string> disks;
static std::string diskInDrive;           // DF0:
static uint8_t *diskBuf = nullptr;        // a disk on its way in (AmigaCore copies it)
static char status[96] = "";

static void requestDisk(const std::string &name, bool boot) {
  if (amigaDiskBusy()) return;            // (the last one is still going in)
  std::string err = AmigaFiles::readDisk(name, diskBuf);
  if (!err.empty()) {
    snprintf(status, sizeof(status), "%s", err.c_str());
    return;
  }
  amigaRequestDisk(diskBuf, boot);
  diskInDrive = name;
}

// ============================================================ the controller
static int padHandle = -1;
static const int kSample = 120, kSamples = 16;
static uint8_t (*padSamples)[kSample] = nullptr;
static uint32_t padHeld = 0;
static uint8_t padLX = 128, padLY = 128, padRX = 128, padRY = 128;
static bool padTouch = false;
static float padTX = 0, padTY = 0, padTW = 1920, padTH = 943;

enum : uint32_t {
  RAW_R3 = 0x0004, RAW_OPTIONS = 0x0008, RAW_UP = 0x0010, RAW_RIGHT = 0x0020, RAW_DOWN = 0x0040, RAW_LEFT = 0x0080,
  RAW_L2 = 0x0100, RAW_R2 = 0x0200, RAW_L1 = 0x0400, RAW_R1 = 0x0800, RAW_TRIANGLE = 0x1000,
  RAW_CIRCLE = 0x2000, RAW_CROSS = 0x4000, RAW_SQUARE = 0x8000, RAW_TOUCHPAD = 0x00100000,
  RAW_INTERCEPTED = 0x80000000u,
  STICK_UP = 0x01000000, STICK_DOWN = 0x02000000, STICK_LEFT = 0x04000000, STICK_RIGHT = 0x08000000,
};

static void padInit() {
  sceUserServiceInitialize(nullptr);
  int32_t user = -1;
  if (sceUserServiceGetInitialUser(&user) < 0 || user < 0 || scePadInit() < 0) return;
  padSamples = (uint8_t(*)[kSample])malloc(64 * 1024);
  if (!padSamples) return;
  memset(padSamples, 0, 64 * 1024);
  padHandle = scePadOpen(user, 0, 0, nullptr);
  if (padHandle < 0) padHandle = scePadGetHandle(user, 0, 0);
}

static void padRead() {
  if (padHandle < 0 || !padSamples) return;
  int n = scePadRead(padHandle, padSamples, kSamples);
  if (n <= 0) return;
  uint32_t any = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t *s = padSamples[i];
    uint32_t raw = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
    if (raw & RAW_INTERCEPTED) {
      any = 0;
      padLX = padLY = padRX = padRY = 128;
      padTouch = false;
      continue;
    }
    any |= raw;
    padLX = s[4];
    padLY = s[5];
    padRX = s[6];
    padRY = s[7];
    padTouch = s[52] > 0;
    if (padTouch) {
      float x = (float)(s[60] | s[61] << 8), y = (float)(s[62] | s[63] << 8);
      if (x + 1 > padTW) padTW = x + 1;
      if (y + 1 > padTH) padTH = y + 1;
      padTX = x / (padTW - 1);
      padTY = y / (padTH - 1);
    }
  }
  any &= 0x00ffffffu;
  float lx = (padLX - 128) / 127.0f, ly = (padLY - 128) / 127.0f;
  if (lx < -0.45f) any |= STICK_LEFT;
  if (lx > 0.45f) any |= STICK_RIGHT;
  if (ly < -0.45f) any |= STICK_UP;
  if (ly > 0.45f) any |= STICK_DOWN;
  padHeld = any;
}

static uint32_t held = 0, pressed = 0, prevHeld = 0;
static uint64_t repeatAt[32];
static bool repeating(uint32_t bits) {
  uint64_t now = ps5NowUs();
  for (int b = 0; b < 32; b++) {
    uint32_t bit = 1u << b;
    if (!(bits & bit)) continue;
    if (pressed & bit) {
      repeatAt[b] = now + 380000;
      return true;
    }
    if ((held & bit) && now >= repeatAt[b]) {
      repeatAt[b] = now + 90000;
      return true;
    }
  }
  return false;
}

// ---- the mouse
static bool prevTouch = false;
static float prevTX = 0, prevTY = 0, mouseAccX = 0, mouseAccY = 0;

static bool touchIsMouse = true; // (false: the on-screen keyboard has the touchpad)
static void mouseFrame(bool active) {
  float dx = 0, dy = 0;
  // the touchpad, like a laptop's: its width is about 500 mouse steps (one
  // and a half times across a low-resolution screen); it is about half as
  // tall as it is wide, so up and down is half that
  if (active && touchIsMouse && padTouch && prevTouch) {
    dx += (padTX - prevTX) * 500.0f;
    dy += (padTY - prevTY) * 250.0f;
  }
  prevTouch = padTouch;
  prevTX = padTX;
  prevTY = padTY;
  // the left stick: faster the further it goes
  auto stick = [](uint8_t v) {
    float f = (v - 128) / 127.0f, a = f < 0 ? -f : f;
    if (a < 0.15f) return 0.0f;
    a = (a - 0.15f) / 0.85f;
    return (f < 0 ? -1.0f : 1.0f) * a * a * 9.0f;
  };
  if (active) {
    dx += stick(padLX);
    dy += stick(padLY);
  }
  mouseAccX += dx;
  mouseAccY += dy;
  int ix = (int)mouseAccX, iy = (int)mouseAccY;
  // (input.device reads the counters each frame as 8-bit steps: keep each
  // step well inside that)
  if (ix > 40) ix = 40;
  if (ix < -40) ix = -40;
  if (iy > 40) iy = 40;
  if (iy < -40) iy = -40;
  mouseAccX -= ix;
  mouseAccY -= iy;
  if (mouseAccX > 40 || mouseAccX < -40) mouseAccX = 0;
  if (mouseAccY > 40 || mouseAccY < -40) mouseAccY = 0;
  if (ix || iy) input_mouse_move(ix, iy);
}

static int fullCopies = 2; // the sides changed: both scanout buffers need them
static void drawSides();

// ============================================================ the MOD player
// A screen of its own (the menu's MOD player), as on the board: the Amiga
// pauses and the modules in the app's assets/amiga/mods folder play through
// the board's ProTracker replayer (ModEngine). They are listed by music.txt
// (tools/make_music_list.py: a PS5 app can't list a folder).
static bool modOpen = false;
static MusicList modList;
static bool modTried = false, modListLoaded = false;
static std::string modFolder;
static int modItems[65536];
static int modCount = 0, modSel = 0, modTop = 0;
static int modPlaying = -1;
static uint8_t *modData = nullptr; // the module playing (from the system)
static char modMsg[80] = "";
static const int kModRows = 20;
// the view (Triangle): 0 the list and channels, 1 the same with the channel
// bars over it, 2... the full-screen visuals
static int modView = 0;
static const int kModViews = 2 + Visuals::COUNT;
static Visuals modVisuals;
static uint64_t modVisLast = 0;
static int modPrevPeriod[ModEngine::MAX_CH], modPrevLevel[ModEngine::MAX_CH];

static std::string modsPath() { return std::string(ps5AppDir()) + "/assets/amiga/mods/"; }
static void *bigAlloc(size_t n) { return heap_caps_malloc(n, 0); }

static void modListFolder(const std::string &folder, int select) {
  modFolder = folder;
  int n = modListLoaded ? modList.children(folder.c_str(), modItems, 65536) : 0;
  modCount = 0;
  for (int i = 0; i < n; i++) // (folders and modules: SID tunes are the C64's)
    if (modList[modItems[i]].kind != 'S') modItems[modCount++] = modItems[i];
  modSel = 0;
  for (int i = 0; i < modCount; i++)
    if (modItems[i] == select) modSel = i;
  modTop = modSel < kModRows ? 0 : modSel - kModRows / 2;
}

static void modOpenNow() {
  if (!modTried) {
    modTried = true;
    modListLoaded = modList.load((modsPath() + "music.txt").c_str(), bigAlloc, heap_caps_free);
  }
  if (modCount == 0) modListFolder("", -1);
  amigaPause(true);
  modSound.store(true, std::memory_order_release);
  modOpen = true;
  fullCopies = 2;
}

static void modStop() {
  modLock.lock();
  mod.unload();
  modLock.unlock();
  if (modData) heap_caps_free(modData);
  modData = nullptr;
  modPlaying = -1;
}

static void modClose() {
  modStop();
  modSound.store(false, std::memory_order_release);
  amigaPause(false);
  amigaMarkAllDirty();
  modOpen = false;
  drawSides();
}

static void modPlay(int index) {
  const MusicEntry &e = modList[index];
  FILE *f = fopen((modsPath() + e.path).c_str(), "rb");
  if (!f) {
    snprintf(modMsg, sizeof(modMsg), "Cannot open %.60s", e.name);
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > (8l << 20)) {
    fclose(f);
    snprintf(modMsg, sizeof(modMsg), "Too big (8 MB at most): %.50s", e.name);
    return;
  }
  uint8_t *data = (uint8_t *)heap_caps_malloc((size_t)size, 0);
  size_t got = data ? fread(data, 1, (size_t)size, f) : 0;
  fclose(f);
  if (!data || got != (size_t)size) {
    if (data) heap_caps_free(data);
    snprintf(modMsg, sizeof(modMsg), "Cannot read %.60s", e.name);
    return;
  }
  modLock.lock();
  const char *err = mod.load(data, got);
  modLock.unlock();
  if (modData) heap_caps_free(modData); // (the engine has let go of it)
  modData = data;
  if (err) {
    snprintf(modMsg, sizeof(modMsg), "%s", err);
    modLock.lock();
    mod.unload();
    modLock.unlock();
    heap_caps_free(modData);
    modData = nullptr;
    modPlaying = -1;
    return;
  }
  modMsg[0] = 0;
  modPlaying = index;
}

static int modNeighbour(int index, int dir) {
  const MusicEntry &e = modList[index];
  static int around[65536];
  std::string folder(e.parent, (size_t)e.parentLen);
  int n = modList.children(folder.c_str(), around, 65536);
  for (int i = 0; i < n; i++) {
    if (around[i] != index) continue;
    for (int j = i + dir; j >= 0 && j < n; j += dir)
      if (modList[around[j]].kind == 'M') return around[j];
  }
  return -1;
}

static void modNext(int dir) {
  if (modPlaying < 0) return;
  int n = modNeighbour(modPlaying, dir);
  if (n >= 0) modPlay(n);
  else modStop();
}

static void modInput() {
  const uint32_t up = RAW_UP | STICK_UP, down = RAW_DOWN | STICK_DOWN, left = RAW_LEFT | STICK_LEFT,
                 right = RAW_RIGHT | STICK_RIGHT;
  if (pressed & RAW_OPTIONS) {
    modClose();
    return;
  }
  if (pressed & RAW_TRIANGLE) modView = (modView + 1) % kModViews;
  if (modView > 1 && (pressed & RAW_CIRCLE)) {
    modView = 0; // (back to the list)
    pressed &= ~RAW_CIRCLE;
  }
  if (modCount > 0 && modView <= 1) {
    if (repeating(up)) modSel = (modSel - 1 + modCount) % modCount;
    if (repeating(down)) modSel = (modSel + 1) % modCount;
    if (repeating(left)) modSel = modSel - kModRows < 0 ? 0 : modSel - kModRows;
    if (repeating(right)) modSel = modSel + kModRows >= modCount ? modCount - 1 : modSel + kModRows;
    if (modSel < modTop) modTop = modSel;
    if (modSel >= modTop + kModRows) modTop = modSel - kModRows + 1;
  }
  if ((pressed & RAW_CROSS) && modCount > 0 && modView <= 1) {
    int i = modItems[modSel];
    if (modList[i].kind == 'D') modListFolder(modList[i].path, -1);
    else modPlay(i);
  }
  if (pressed & RAW_CIRCLE) {
    if (modFolder.empty()) {
      modClose();
      return;
    }
    size_t slash = modFolder.rfind('/');
    std::string upF = slash == std::string::npos ? "" : modFolder.substr(0, slash);
    int was = -1;
    for (int i = 0; i < modList.size(); i++)
      if (modList[i].kind == 'D' && modFolder == modList[i].path) was = i;
    modListFolder(upF, was);
  }
  if (modPlaying >= 0) {
    if (pressed & RAW_SQUARE) mod.paused = !mod.paused;
    if (pressed & RAW_TOUCHPAD) modStop();
    if (pressed & (RAW_R1 | RAW_L1)) {
      int to = mod.position + ((pressed & RAW_R1) ? 1 : -1);
      if (to >= 0 && to < mod.songLength) {
        modLock.lock();
        mod.setPosition(to);
        modLock.unlock();
      }
    }
    if (pressed & RAW_R2) modNext(1);
    if (pressed & RAW_L2) modNext(-1);
    // the end of the song: the next module in the folder
    if (modPlaying >= 0 && mod.ended) modNext(1);
  }
}

static void drawModPlayer() {
  char line[96];
  fillRect(0, 0, kW, kH, kWbBlue);
  // a Workbench window over the whole TV
  frameRect(24, 24, kW - 48, kH - 48, 4, kWbWhite);
  fillRect(24, 24, kW - 48, 44, kWbWhite);
  text(44, 30, "MOD Player", 2, kWbBlue);
  text(kW - 44 - textWidth(modelTitle(), 2), 30, modelTitle(), 2, kWbBlue);

  // ---- left: the folder and what is in it
  snprintf(line, sizeof(line), "/%.44s", modFolder.c_str());
  text(60, 96, line, 2, kWbOrange);
  int lx = 60, ly = 146, rowH = 40;
  if (!modListLoaded) {
    text(lx, ly, "No modules in the app.", 2, kWbWhite);
    text(lx, ly + 40, "Put them in amiga/assets/amiga/mods", 2, kWbWhite);
    text(lx, ly + 80, "and build again.", 2, kWbWhite);
  } else if (modCount == 0) {
    text(lx, ly, "(empty)", 2, kWbWhite);
  }
  for (int r = 0; r < kModRows && modTop + r < modCount; r++) {
    int i = modItems[modTop + r];
    const MusicEntry &e = modList[i];
    if (e.kind == 'D') snprintf(line, sizeof(line), "%.44s/", e.name);
    else snprintf(line, sizeof(line), "%.45s", e.title[0] ? e.title : e.name);
    int y = ly + r * rowH;
    uint32_t col = i == modPlaying ? kWbOrange : e.kind == 'D' ? kWbBlack : kWbWhite;
    if (modTop + r == modSel) {
      fillRect(lx - 10, y - 4, 860, rowH - 4, kWbOrange);
      col = kWbBlack;
    }
    text(lx, y, line, 2, col);
  }
  if (modCount > kModRows) {
    snprintf(line, sizeof(line), "%d-%d of %d", modTop + 1, modTop + kModRows < modCount ? modTop + kModRows : modCount,
             modCount);
    text(lx, ly + kModRows * rowH + 4, line, 1, kWbWhite);
  }

  // ---- right: what is playing
  int rx = 960, ry = 96;
  fillRect(rx - 30, 90, 4, 880, kWbWhite);
  if (modPlaying < 0 || !mod.loaded()) {
    text(rx, ry, "Nothing playing", 3, kWbWhite);
    text(rx, ry + 60, "Choose a module on the left", 2, kWbWhite);
  } else {
    const MusicEntry &e = modList[modPlaying];
    text(rx, ry, mod.title[0] ? mod.title : e.name, 3, kWbWhite);
    snprintf(line, sizeof(line), "%.40s", e.name);
    text(rx, ry + 54, line, 1, kWbBlack);
    snprintf(line, sizeof(line), "%s  %d channels  %d samples", mod.format, mod.channels, mod.numSamples());
    text(rx, ry + 80, line, 2, kWbWhite);
    uint32_t secs = mod.playedSamples / ModEngine::RATE;
    snprintf(line, sizeof(line), "Position %02d/%02d   Pattern %02d   Row %02d", mod.position, mod.songLength,
             mod.pattern, mod.row);
    text(rx, ry + 124, line, 2, kWbWhite);
    snprintf(line, sizeof(line), "Speed %d   Tempo %d   %u:%02u%s", mod.speed, mod.tempo, (unsigned)(secs / 60),
             (unsigned)(secs % 60), mod.paused ? "   PAUSED" : "");
    text(rx, ry + 164, line, 2, mod.paused ? kWbOrange : kWbWhite);
    // the song's progress (by position)
    int bw = 880;
    frameRect(rx, ry + 210, bw, 18, 2, kWbWhite);
    if (mod.songLength > 0)
      fillRect(rx + 4, ry + 214, (bw - 8) * (mod.position * 64 + mod.row) / (mod.songLength * 64), 10, kWbOrange);

    // ---- the channels: a scope, the level, the note and sample
    int nch = mod.channels < ModEngine::MAX_CH ? mod.channels : ModEngine::MAX_CH;
    int cw = 210, chh = nch > 4 ? 220 : 300;
    for (int c = 0; c < nch; c++) {
      int cx = rx + (c % 4) * (cw + 14), cy = ry + 260 + (c / 4) * (chh + 40);
      const ModEngine::ChannelView &v = mod.view[c];
      fillRect(cx, cy, cw, chh, kWbBlack);
      frameRect(cx, cy, cw, chh, 2, kWbWhite);
      // the scope: the last samples played, oldest first
      int sh = chh - 70, mid = cy + 8 + sh / 2, prevY = mid;
      for (int k = 0; k < ModEngine::SCOPE_N && k < cw - 10; k++) {
        int sv = v.scope[(v.scopePos + k) % ModEngine::SCOPE_N];
        int y = mid - sv * (sh / 2 - 2) / 128;
        int y0 = prevY < y ? prevY : y, y1 = prevY < y ? y : prevY;
        fillRect(cx + 5 + k * (cw - 10) / ModEngine::SCOPE_N, y0, 2, y1 - y0 + 2, kWbOrange);
        prevY = y;
      }
      // the level
      fillRect(cx + 6, cy + chh - 54, (cw - 12) * v.level / 127, 10, kWbWhite);
      char note[4];
      ModEngine::noteName(v.period, note);
      snprintf(line, sizeof(line), "%d %s %02d", c + 1, note, v.sample);
      text(cx + 8, cy + chh - 36, line, 2, kWbWhite);
    }
  }
  if (modMsg[0]) text(rx, 900, modMsg, 2, kWbOrange);

  // ---- the controls, along the bottom
  int by = 1000, x = 60;
  auto ctl = [&](Icon ic, const char *btn, const char *what) {
    int bw;
    if (btn) {
      bw = textWidth(btn, 1) + 10;
      buttonLabel(btn, x + bw / 2, by + 16);
    } else {
      bw = 28;
      icon(ic, x + 14, by + 16, 26);
    }
    text(x + bw + 10, by, what, 2, kWbWhite);
    x += bw + 10 + textWidth(what, 2) + 44;
  };
  ctl(ICON_CROSS, nullptr, "play/open");
  ctl(ICON_CIRCLE, nullptr, "back");
  ctl(ICON_SQUARE, nullptr, "pause");
  ctl(ICON_TRIANGLE, nullptr, "visuals");
  ctl(ICON_CROSS, "PAD", "stop");
  ctl(ICON_CROSS, "L1 R1", "position");
  ctl(ICON_CROSS, "L2 R2", "module");
  ctl(ICON_CROSS, "OPT", "Amiga");
}

// The MOD player's visuals: the channels' levels, notes and new notes, and
// the sound itself, given to the shared Visuals (common/Visuals.h)
static void modGlyph(uint32_t *, int x, int y, char c, int scale, uint32_t col) {
  char s1[2] = {c, 0};
  text(x, y, s1, scale, col);
}

// view: 1 the bars over the player's screen, 2... a full-screen visual
static void drawModVisuals(int view) {
  uint64_t now = ps5NowUs();
  float dt = modVisLast ? (now - modVisLast) / 1e6f : 1 / 60.0f;
  modVisLast = now;
  VisInput in = {};
  bool on = modPlaying >= 0 && mod.loaded();
  in.channels = on ? (mod.channels < 8 ? mod.channels : 8) : 4;
  for (int c = 0; c < in.channels; c++) {
    const ModEngine::ChannelView &v = mod.view[c];
    in.level[c] = on ? v.level / 127.0f : 0;
    if (on && v.period > 0) {
      float midi = 48 + 12 * log2f(428.0f / v.period); // (C-2, period 428, as note 48)
      float p = (midi - 24) / 72.0f;
      in.pitch[c] = p < 0.01f ? 0.01f : p > 1 ? 1 : p;
    }
    // a new note: the period changed, or the level jumped
    in.hit[c] = on && ((v.period > 0 && v.period != modPrevPeriod[c]) || v.level > modPrevLevel[c] + 30);
    modPrevPeriod[c] = v.period;
    modPrevLevel[c] = v.level;
  }
  static int16_t wave[1024];
  int wh = modWaveHead.load(std::memory_order_acquire);
  for (int i = 0; i < 1024; i++) wave[i] = on ? modWave[(wh - 1024 + i) & 2047] : 0;
  in.wave = wave;
  in.waveN = 1024;
  static char info[128], title[64];
  if (on) {
    const MusicEntry &e = modList[modPlaying];
    snprintf(title, sizeof(title), "%s", mod.title[0] ? mod.title : e.name);
    uint32_t secs = mod.playedSamples / ModEngine::RATE;
    snprintf(info, sizeof(info), "%s  %d channels  -  position %02d/%02d  pattern %02d  -  %u:%02u", mod.format,
             mod.channels, mod.position, mod.songLength, mod.pattern, (unsigned)(secs / 60), (unsigned)(secs % 60));
    in.progress = mod.songLength ? (mod.position * 64 + mod.row) / (float)(mod.songLength * 64) : -1;
  } else {
    snprintf(title, sizeof(title), "Nothing playing");
    snprintf(info, sizeof(info), "Circle: the list, to choose a module");
    in.progress = -1;
  }
  in.title = title;
  in.author = on ? mod.format : "";
  in.info = info;
  in.paused = mod.paused;
  in.caption = "MOD PLAYER";
  static const VisFont font = {modGlyph, 8, 16};
  if (view == 1) modVisuals.drawBars(g_frame, in, dt);
  else modVisuals.draw(g_frame, view - 2, in, font, dt);
}

// ============================================================ a USB keyboard (and mouse)
// Read straight from USB (common/UsbKeyboard.h): taken from the PS5 while the
// app runs, looked for again every 2 seconds when there is none. The keys go
// to the Amiga by their place (an Amiga keyboard laid out as a US PC one):
//   Windows / Menu the left / right Amiga keys, Alt Alt, F11 HELP, F12 the menu,
//   Ctrl + both Windows keys (or Windows + Menu) the Amiga's reset.
// A wireless set's mouse on the same receiver is the Amiga's mouse too. In the
// menu and the MOD player: the arrows, Return (Cross), Esc (Circle), Space
// (Square), Tab (Triangle), F12 (Options).
static UsbKeyboard usbKb;
static bool usbKbWanted = true; // the menu's USB keyboard: on
static int usbKbScanIn = 0;     // frames to the next look for one
static bool usbToAmiga = false; // false while the menu or the MOD player has the keys
static uint8_t usbPrev[8];
static uint32_t usbNav = 0;     // controller buttons from keys (menus)
static bool amigaKeyDown[0x68]; // what the Amiga has been told is down
static bool capsOn = false;
static int usbMouseButtons = 0; // 1 left, 2 right

// HID usage -> Amiga raw key code (0xff: none)
static uint8_t hidToAmiga(uint8_t h) {
  static const uint8_t letters[26] = {0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,
                                      0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31};
  if (h >= 0x04 && h <= 0x1d) return letters[h - 0x04];
  if (h >= 0x1e && h <= 0x27) return (uint8_t)(0x01 + h - 0x1e); // 1-9, 0
  if (h >= 0x3a && h <= 0x43) return (uint8_t)(0x50 + h - 0x3a); // F1-F10
  static const uint8_t keypad19[9] = {0x1d, 0x1e, 0x1f, 0x2d, 0x2e, 0x2f, 0x3d, 0x3e, 0x3f};
  if (h >= 0x59 && h <= 0x61) return keypad19[h - 0x59];
  switch (h) {
  case 0x28: return 0x44; // Return
  case 0x29: return 0x45; // Esc
  case 0x2a: return 0x41; // Backspace
  case 0x2b: return 0x42; // Tab
  case 0x2c: return 0x40; // Space
  case 0x2d: return 0x0b; // -
  case 0x2e: return 0x0c; // =
  case 0x2f: return 0x1a; // [
  case 0x30: return 0x1b; // ]
  case 0x31: return 0x0d; // backslash
  case 0x32: return 0x2b; // (international, by Return)
  case 0x33: return 0x29; // ;
  case 0x34: return 0x2a; // '
  case 0x35: return 0x00; // `
  case 0x36: return 0x38; // ,
  case 0x37: return 0x39; // .
  case 0x38: return 0x3a; // /
  case 0x44: return 0x5f; // F11: HELP
  case 0x4c: return 0x46; // Delete
  case 0x4f: return 0x4e; // right
  case 0x50: return 0x4f; // left
  case 0x51: return 0x4d; // down
  case 0x52: return 0x4c; // up
  case 0x54: return 0x5c; // keypad /
  case 0x55: return 0x5d; // keypad *
  case 0x56: return 0x4a; // keypad -
  case 0x57: return 0x5e; // keypad +
  case 0x58: return 0x43; // keypad Enter
  case 0x62: return 0x0f; // keypad 0
  case 0x63: return 0x3c; // keypad .
  case 0x64: return 0x30; // (international, by left Shift)
  case 0x65: return 0x67; // Menu: right Amiga
  }
  return 0xff;
}

static void amigaKey(uint8_t code, bool down) {
  if (code >= 0x68 || amigaKeyDown[code] == down) return;
  amigaKeyDown[code] = down;
  kbd_key(code, down ? 0 : 1);
}

static void amigaAllKeysUp() {
  for (int c = 0; c < 0x68; c++)
    if (amigaKeyDown[c] && c != 0x62) amigaKey((uint8_t)c, false);
}

static void usbReport(const uint8_t *r) {
  bool held[256] = {};
  for (int k = 2; k < 8; k++)
    if (r[k] >= 4) held[r[k]] = true;
  uint32_t nav = 0;
  if (held[0x52]) nav |= RAW_UP;
  if (held[0x51]) nav |= RAW_DOWN;
  if (held[0x50]) nav |= RAW_LEFT;
  if (held[0x4f]) nav |= RAW_RIGHT;
  if (held[0x28]) nav |= RAW_CROSS;
  if (held[0x29]) nav |= RAW_CIRCLE;
  if (held[0x2c]) nav |= RAW_SQUARE;
  if (held[0x2b]) nav |= RAW_TRIANGLE;
  if (held[0x45]) nav |= RAW_OPTIONS;
  usbNav = nav;
  if (usbToAmiga) {
    // Ctrl + Amiga + Amiga: the reset
    bool ctrl = r[0] & 0x11, lA = r[0] & 0x08, rA = (r[0] & 0x80) || held[0x65];
    bool wasReset = (usbPrev[0] & 0x11) && (usbPrev[0] & 0x08) && ((usbPrev[0] & 0x80) != 0);
    if (ctrl && lA && rA && !wasReset) {
      amigaAllKeysUp();
      amigaRequestReset();
      snprintf(status, sizeof(status), "RESET");
    }
    // the modifiers: Ctrl, Shift, Alt, Windows (Amiga) - left and right
    static const uint8_t mods[8] = {0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67};
    bool modDown[0x68] = {};
    for (int b = 0; b < 8; b++)
      if (r[0] >> b & 1) modDown[mods[b]] = true;
    for (int b = 0; b < 8; b++) amigaKey(mods[b], modDown[mods[b]] || (mods[b] == 0x67 && held[0x65]));
    // Caps Lock: the Amiga's locks (down while on)
    bool capsNow = held[0x39], capsBefore = false;
    for (int k = 2; k < 8; k++) capsBefore |= usbPrev[k] == 0x39;
    if (capsNow && !capsBefore) {
      capsOn = !capsOn;
      amigaKey(0x62, capsOn);
    }
    // the rest: up for those let go, down for new ones
    for (int k = 2; k < 8; k++) {
      uint8_t c = usbPrev[k];
      if (c >= 4 && !held[c] && c != 0x39 && c != 0x65) amigaKey(hidToAmiga(c), false);
    }
    for (int k = 2; k < 8; k++) {
      uint8_t c = r[k];
      if (c >= 4 && c != 0x39 && c != 0x45 && c != 0x65) amigaKey(hidToAmiga(c), true);
    }
  }
  memcpy(usbPrev, r, 8);
}

static void usbKeyboardFrame() {
  if (!usbToAmiga) amigaAllKeysUp();
  usbMouseButtons = 0;
  if (!usbKbWanted) {
    usbNav = 0;
    return;
  }
  if (!usbKb.isOpen()) {
    if (usbKb.lost) usbKb.close(false); // (unplugged)
    usbNav = 0;
    if (--usbKbScanIn > 0) return;
    usbKbScanIn = 120;
    UsbKeyboard::Info i;
    if (UsbKeyboard::find(i) && usbKb.open(i, true) == 0) {
      memset(usbPrev, 0, sizeof(usbPrev));
      snprintf(status, sizeof(status), "%s", usbKb.hasMouse() ? "USB KEYBOARD AND MOUSE" : "USB KEYBOARD");
    }
    return;
  }
  uint8_t r[8];
  if (usbKb.poll(r)) usbReport(r);
  int dx, dy;
  usbKb.takeMouse(dx, dy);
  if (usbToAmiga) {
    // (input.device reads the counters each frame as 8-bit steps: keep each step well inside that)
    while (dx || dy) {
      int sx = dx > 40 ? 40 : dx < -40 ? -40 : dx, sy = dy > 40 ? 40 : dy < -40 ? -40 : dy;
      input_mouse_move(sx, sy);
      dx -= sx;
      dy -= sy;
      if (dx > 200 || dx < -200 || dy > 200 || dy < -200) break;
    }
    usbMouseButtons = usbKb.mouseButtons;
  }
}

// ============================================================ the on-screen keyboard
// An Amiga 500 keyboard (US), drawn: R3 (click the right stick) shows it under
// a smaller picture. The D-pad chooses a key (red frame), Cross presses it (held
// as long as Cross is), the touchpad points at keys and its click presses one.
// Square is SPACE, Circle RETURN. Shift, Ctrl, Alt and the Amiga keys chosen
// with Cross hold for the next key (blue); Caps Lock locks. The left stick and
// R1 R2 / L1 L2 stay the mouse.
struct OsKey {
  const char *label;
  uint8_t code;
  float x, y, w, h; // in key units
};
static const OsKey osKeys[] = {
    {"Esc", 0x45, 0, 0, 1.25f, 0.8f},
    {"F1", 0x50, 1.75f, 0, 1.25f, 0.8f}, {"F2", 0x51, 3.0f, 0, 1.25f, 0.8f}, {"F3", 0x52, 4.25f, 0, 1.25f, 0.8f},
    {"F4", 0x53, 5.5f, 0, 1.25f, 0.8f}, {"F5", 0x54, 6.75f, 0, 1.25f, 0.8f}, {"F6", 0x55, 8.25f, 0, 1.25f, 0.8f},
    {"F7", 0x56, 9.5f, 0, 1.25f, 0.8f}, {"F8", 0x57, 10.75f, 0, 1.25f, 0.8f}, {"F9", 0x58, 12.0f, 0, 1.25f, 0.8f},
    {"F10", 0x59, 13.25f, 0, 1.25f, 0.8f},
    {"Del", 0x46, 15.5f, 0, 1.5f, 0.8f}, {"Help", 0x5f, 17.0f, 0, 1.5f, 0.8f},
    // row 1
    {"`", 0x00, 0, 1, 1, 1}, {"1", 0x01, 1, 1, 1, 1}, {"2", 0x02, 2, 1, 1, 1}, {"3", 0x03, 3, 1, 1, 1},
    {"4", 0x04, 4, 1, 1, 1}, {"5", 0x05, 5, 1, 1, 1}, {"6", 0x06, 6, 1, 1, 1}, {"7", 0x07, 7, 1, 1, 1},
    {"8", 0x08, 8, 1, 1, 1}, {"9", 0x09, 9, 1, 1, 1}, {"0", 0x0a, 10, 1, 1, 1}, {"-", 0x0b, 11, 1, 1, 1},
    {"=", 0x0c, 12, 1, 1, 1}, {"\\", 0x0d, 13, 1, 1, 1}, {"Bksp", 0x41, 14, 1, 1, 1},
    // row 2
    {"Tab", 0x42, 0, 2, 1.5f, 1}, {"Q", 0x10, 1.5f, 2, 1, 1}, {"W", 0x11, 2.5f, 2, 1, 1}, {"E", 0x12, 3.5f, 2, 1, 1},
    {"R", 0x13, 4.5f, 2, 1, 1}, {"T", 0x14, 5.5f, 2, 1, 1}, {"Y", 0x15, 6.5f, 2, 1, 1}, {"U", 0x16, 7.5f, 2, 1, 1},
    {"I", 0x17, 8.5f, 2, 1, 1}, {"O", 0x18, 9.5f, 2, 1, 1}, {"P", 0x19, 10.5f, 2, 1, 1}, {"[", 0x1a, 11.5f, 2, 1, 1},
    {"]", 0x1b, 12.5f, 2, 1, 1}, {"Return", 0x44, 13.5f, 2, 1.5f, 2},
    // row 3
    {"Ctrl", 0x63, 0, 3, 1.25f, 1}, {"Caps", 0x62, 1.25f, 3, 1.25f, 1}, {"A", 0x20, 2.5f, 3, 1, 1},
    {"S", 0x21, 3.5f, 3, 1, 1}, {"D", 0x22, 4.5f, 3, 1, 1}, {"F", 0x23, 5.5f, 3, 1, 1}, {"G", 0x24, 6.5f, 3, 1, 1},
    {"H", 0x25, 7.5f, 3, 1, 1}, {"J", 0x26, 8.5f, 3, 1, 1}, {"K", 0x27, 9.5f, 3, 1, 1}, {"L", 0x28, 10.5f, 3, 1, 1},
    {";", 0x29, 11.5f, 3, 1, 1}, {"'", 0x2a, 12.5f, 3, 1, 1},
    // row 4
    {"Shift", 0x60, 0, 4, 2.25f, 1}, {"Z", 0x31, 2.25f, 4, 1, 1}, {"X", 0x32, 3.25f, 4, 1, 1},
    {"C", 0x33, 4.25f, 4, 1, 1}, {"V", 0x34, 5.25f, 4, 1, 1}, {"B", 0x35, 6.25f, 4, 1, 1}, {"N", 0x36, 7.25f, 4, 1, 1},
    {"M", 0x37, 8.25f, 4, 1, 1}, {",", 0x38, 9.25f, 4, 1, 1}, {".", 0x39, 10.25f, 4, 1, 1}, {"/", 0x3a, 11.25f, 4, 1, 1},
    {"Shift", 0x61, 12.25f, 4, 2.75f, 1},
    // row 5
    {"Alt", 0x64, 0.75f, 5, 1.5f, 1}, {"A", 0x66, 2.25f, 5, 1.25f, 1}, {"", 0x40, 3.5f, 5, 8, 1},
    {"A", 0x67, 11.5f, 5, 1.25f, 1}, {"Alt", 0x65, 12.75f, 5, 1.5f, 1},
    // the cursor keys
    {"", 0x4c, 16.25f, 4, 1, 1}, {"", 0x4f, 15.25f, 5, 1, 1}, {"", 0x4d, 16.25f, 5, 1, 1},
    {"", 0x4e, 17.25f, 5, 1, 1},
    // the keypad
    {"(", 0x5a, 19, 1, 1, 1}, {")", 0x5b, 20, 1, 1, 1}, {"/", 0x5c, 21, 1, 1, 1}, {"*", 0x5d, 22, 1, 1, 1},
    {"7", 0x3d, 19, 2, 1, 1}, {"8", 0x3e, 20, 2, 1, 1}, {"9", 0x3f, 21, 2, 1, 1}, {"-", 0x4a, 22, 2, 1, 1},
    {"4", 0x2d, 19, 3, 1, 1}, {"5", 0x2e, 20, 3, 1, 1}, {"6", 0x2f, 21, 3, 1, 1}, {"+", 0x5e, 22, 3, 1, 1},
    {"1", 0x1d, 19, 4, 1, 1}, {"2", 0x1e, 20, 4, 1, 1}, {"3", 0x1f, 21, 4, 1, 1}, {"Ent", 0x43, 22, 4, 1, 2},
    {"0", 0x0f, 19, 5, 2, 1}, {".", 0x3c, 21, 5, 1, 1},
};
static const int kOsKeys = (int)(sizeof(osKeys) / sizeof(osKeys[0]));
static const int kOsUnit = 54, kOsRowH = 46;
static const int kOsX = kPicX + (1280 - 23 * kOsUnit) / 2, kOsY = 790;

static bool oskOpen = false;
static int oskSel = 29;           // the chosen key (Q to start with)
static int oskDown = -1;          // the key Cross (or a touchpad click) holds down
static uint8_t oskSticky = 0;     // modifiers held for the next key: 1 shift, 2 ctrl, 4 alt, 8 amiga (left), 16 amiga (right)
static bool oskCaps = false;
static int oskTouch = -1;         // the key under the finger

static void oskRect(const OsKey &k, int &x, int &y, int &w, int &h) {
  x = kOsX + (int)(k.x * kOsUnit) + 2;
  y = kOsY + (int)(k.y * kOsRowH) + 2;
  w = (int)(k.w * kOsUnit) - 4;
  h = (int)(k.h * kOsRowH) - 4;
}

static int oskModBit(uint8_t code) {
  switch (code) {
  case 0x60: case 0x61: return 1;
  case 0x63: return 2;
  case 0x64: case 0x65: return 4;
  case 0x66: return 8;
  case 0x67: return 16;
  }
  return 0;
}
static uint8_t oskModCode(int bit) { return bit == 1 ? 0x60 : bit == 2 ? 0x63 : bit == 4 ? 0x64 : bit == 8 ? 0x66 : 0x67; }

static void oskSetSticky(uint8_t now) {
  for (int b = 1; b <= 16; b <<= 1)
    if ((now ^ oskSticky) & b) amigaKey(oskModCode(b), (now & b) != 0);
  oskSticky = now;
}

static void oskPress(int i) {
  const OsKey &k = osKeys[i];
  if (k.code == 0x62) { // Caps Lock: locks
    oskCaps = !oskCaps;
    amigaKey(0x62, oskCaps);
    return;
  }
  if (int b = oskModBit(k.code)) {
    oskSetSticky(oskSticky ^ (uint8_t)b);
    return;
  }
  amigaKey(k.code, true);
  oskDown = i;
}
static void oskRelease() {
  if (oskDown < 0) return;
  amigaKey(osKeys[oskDown].code, false);
  oskDown = -1;
  oskSetSticky(0); // (the modifiers were for this key)
}

// the nearest key in a direction from the chosen one
static int oskMove(int dx, int dy) {
  const OsKey &c = osKeys[oskSel];
  float cx = c.x + c.w / 2, cy = c.y + (dy ? c.h / 2 : 0.5f);
  int best = oskSel;
  float bestCost = 1e9f;
  for (int i = 0; i < kOsKeys; i++) {
    if (i == oskSel) continue;
    const OsKey &k = osKeys[i];
    float kx = k.x + k.w / 2, ky = k.y + k.h / 2;
    float ax = (kx - cx) * dx + (ky - cy) * dy; // along the direction
    if (ax <= 0.1f) continue;
    float side = dx ? (cy >= k.y && cy < k.y + k.h ? 0 : (ky - cy < 0 ? cy - ky : ky - cy))
                    : (cx >= k.x && cx < k.x + k.w ? 0 : (kx - cx < 0 ? cx - kx : kx - cx));
    float cost = ax + side * 4;
    if (cost < bestCost) bestCost = cost, best = i;
  }
  return best;
}

static int oskKeyAt(float ux, float uy) {
  for (int i = 0; i < kOsKeys; i++) {
    const OsKey &k = osKeys[i];
    if (ux >= k.x && ux < k.x + k.w && uy >= k.y && uy < k.y + k.h) return i;
  }
  return -1;
}

static void oskOpenNow(bool open) {
  if (!open) {
    oskRelease();
    oskSetSticky(0);
  }
  oskOpen = open;
  drawSides();
}

// once a frame while it shows (the Amiga screen, no menu)
static void oskInput() {
  if (repeating(RAW_LEFT)) oskSel = oskMove(-1, 0);
  if (repeating(RAW_RIGHT)) oskSel = oskMove(1, 0);
  if (repeating(RAW_UP)) oskSel = oskMove(0, -1);
  if (repeating(RAW_DOWN)) oskSel = oskMove(0, 1);
  // the touchpad: the keyboard, the whole pad
  oskTouch = -1;
  if (padTouch) oskTouch = oskKeyAt(padTX * 23.0f, padTY * 6.0f);
  if (oskDown < 0) {
    if (pressed & RAW_CROSS) oskPress(oskSel);
    else if ((pressed & RAW_TOUCHPAD) && oskTouch >= 0) oskPress(oskSel = oskTouch);
    else if (pressed & RAW_SQUARE) oskPress(oskSel = oskKeyAt(7.5f, 5.5f));
    else if (pressed & RAW_CIRCLE) oskPress(oskSel = oskKeyAt(14.0f, 2.5f));
  } else if (!(held & (RAW_CROSS | RAW_TOUCHPAD | RAW_SQUARE | RAW_CIRCLE))) {
    oskRelease();
  }
}

static void drawOsk() {
  const uint32_t body = rgb(0xc8, 0xc0, 0xa8), key = rgb(0xf0, 0xea, 0xd8), dark = rgb(0x9c, 0x96, 0x88),
                 label = rgb(0x30, 0x30, 0x38), blue = rgb(0x40, 0x80, 0xe0), red = rgb(0xff, 0x10, 0x10),
                 orange = rgb(0xff, 0x88, 0x00);
  fillRect(kOsX - 12, kOsY - 10, 23 * kOsUnit + 24, 6 * kOsRowH + 18, body);
  for (int i = 0; i < kOsKeys; i++) {
    const OsKey &k = osKeys[i];
    int x, y, w, h;
    oskRect(k, x, y, w, h);
    bool fn = k.y == 0 || k.code >= 0x60 || k.code == 0x41 || k.code == 0x42 || k.code == 0x44 || k.code == 0x43;
    uint32_t c = fn ? dark : key;
    int mb = oskModBit(k.code);
    if ((mb && (oskSticky & mb)) || (k.code == 0x62 && oskCaps) || i == oskDown) c = blue;
    fillRect(x, y, w, h, c);
    fillRect(x, y + h - 4, w, 4, rgb(0x70, 0x6c, 0x60)); // (a shadow)
    if (k.code == 0x66 || k.code == 0x67) { // the Amiga keys: an "A", filled for the right one
      int s = 2;
      uint32_t ac = k.code == 0x67 ? label : rgb(0xff, 0xff, 0xff);
      if (k.code == 0x66) fillRect(x + w / 2 - 14, y + 6, 28, h - 14, label);
      text(x + w / 2 - 8, y + (h - 32) / 2, "A", s, ac);
    } else if (k.code >= 0x4c && k.code <= 0x4f) { // the cursor keys: an arrow
      int cx = x + w / 2, cy = y + (h - 4) / 2;
      for (int t = 0; t < 10; t++) {
        if (k.code == 0x4c) fillRect(cx - t, cy - 6 + t, 2 * t + 1, 1, label);  // up
        if (k.code == 0x4d) fillRect(cx - t, cy + 6 - t, 2 * t + 1, 1, label);  // down
        if (k.code == 0x4f) fillRect(cx - 6 + t, cy - t, 1, 2 * t + 1, label);  // left
        if (k.code == 0x4e) fillRect(cx + 6 - t, cy - t, 1, 2 * t + 1, label);  // right
      }
    } else if (k.label[0]) {
      int s = strlen(k.label) == 1 ? 2 : 1;
      int tw = textWidth(k.label, s), th = 16 * s;
      text(x + (w - tw) / 2, y + (h - 4 - th) / 2, k.label, s, label);
    }
    if (i == oskTouch) frameRect(x - 2, y - 2, w + 4, h + 4, 3, orange);
    if (i == oskSel) frameRect(x - 3, y - 3, w + 6, h + 6, 4, red);
  }
}

// ============================================================ the menu
enum ItemKind { IT_RESUME, IT_MOD, IT_OSK, IT_RESET, IT_MODEL, IT_VOLUME, IT_USBKBD, IT_QUIT, IT_HEADER, IT_DISK, IT_NONE };
struct Item {
  ItemKind kind;
  std::string name;
};
static std::vector<Item> items;
static bool menuOpen = false;
static int menuSel = 0, menuTop = 0;
static const int kMenuRows = 18;
static bool quitArmed = false; // Quit chosen once: Cross again closes the app
static int modelPick = 0;      // (the model shown in the menu: Cross switches to it)

static void menuOpenNow() {
  items.clear();
  items.push_back({IT_RESUME, ""});
  items.push_back({IT_MOD, ""});
  items.push_back({IT_OSK, ""});
  items.push_back({IT_RESET, ""});
#ifdef AMIBERRY
  items.push_back({IT_MODEL, ""});
  modelPick = amigaModel();
#endif
  items.push_back({IT_VOLUME, ""});
  items.push_back({IT_USBKBD, ""});
  items.push_back({IT_QUIT, ""});
  items.push_back({IT_HEADER, ""});
  for (auto &d : disks) items.push_back({IT_DISK, d});
  if (disks.empty()) items.push_back({IT_NONE, "(no .adf disks in the app)"});
  menuSel = 0;
  for (int i = 0; i < (int)items.size(); i++)
    if (items[i].kind == IT_DISK && (menuSel == 0 || items[i].name == diskInDrive)) {
      menuSel = i;
      if (items[i].name == diskInDrive) break;
    }
  menuTop = menuSel < kMenuRows ? 0 : menuSel - kMenuRows / 2;
  menuOpen = true;
}

static bool selectable(int i) { return items[i].kind != IT_HEADER && items[i].kind != IT_NONE; }

static void menuInput() {
  const uint32_t up = RAW_UP | STICK_UP, down = RAW_DOWN | STICK_DOWN, left = RAW_LEFT | STICK_LEFT,
                 right = RAW_RIGHT | STICK_RIGHT;
  int n = (int)items.size();
  if (repeating(up)) {
    int i = menuSel;
    do i = (i - 1 + n) % n;
    while (!selectable(i));
    menuSel = i;
  }
  if (repeating(down)) {
    int i = menuSel;
    do i = (i + 1) % n;
    while (!selectable(i));
    menuSel = i;
  }
  if (menuSel < menuTop) menuTop = menuSel;
  if (menuSel >= menuTop + kMenuRows) menuTop = menuSel - kMenuRows + 1;
  if (menuSel <= 7) menuTop = 0;
  if (items[menuSel].kind != IT_QUIT) quitArmed = false;
  Item &it = items[menuSel];
#ifdef AMIBERRY
  if (it.kind == IT_MODEL && (repeating(left) || repeating(right))) {
    // (the next model with its Kickstart in the app)
    int step = (held & right) ? 1 : -1, n = amigaModelCount();
    for (int k = 1; k <= n; k++) {
      int m = ((modelPick + step * k) % n + n) % n;
      if (amigaModelAvailable(m)) {
        modelPick = m;
        break;
      }
    }
  }
  if (it.kind == IT_MODEL && (pressed & RAW_CROSS) && modelPick != amigaModel()) {
    amigaSetModel(modelPick);
    snprintf(status, sizeof(status), "AMIGA %s", amigaModelName(modelPick) + 1);
    menuOpen = false;
    return;
  }
#endif
  if (it.kind == IT_VOLUME && (repeating(left) || repeating(right))) {
    int v = volumePct.load() + ((held & right) ? 10 : -10);
    volumePct.store(v < 0 ? 0 : v > 100 ? 100 : v);
  }
  if (pressed & (RAW_CIRCLE | RAW_OPTIONS)) {
    menuOpen = false;
    quitArmed = false;
    return;
  }
  if (it.kind == IT_QUIT && (pressed & RAW_CROSS)) {
    if (quitArmed) {
      volumePct.store(0);
      usbKb.close(true); // (the keyboard back to the PS5)
      sceSystemServiceLoadExec("exit", nullptr); // (the app closes)
    }
    quitArmed = true;
    return;
  }
  if (it.kind == IT_DISK && (pressed & (RAW_CROSS | RAW_SQUARE))) {
    bool boot = (pressed & RAW_CROSS) != 0;
    requestDisk(it.name, boot);
    snprintf(status, sizeof(status), "%s %s", boot ? "BOOTING" : "DF0:", it.name.c_str());
    menuOpen = false;
    return;
  }
  if (pressed & RAW_CROSS) {
    if (it.kind == IT_USBKBD) {
      usbKbWanted = !usbKbWanted;
      if (!usbKbWanted) usbKb.close(true); // the PS5 has it again
      usbKbScanIn = 0;
    }
    if (it.kind == IT_RESUME) menuOpen = false;
    if (it.kind == IT_OSK) {
      menuOpen = false;
      oskOpenNow(!oskOpen);
    }
    if (it.kind == IT_MOD) {
      menuOpen = false;
      modOpenNow();
    }
    if (it.kind == IT_RESET) {
      if (!diskInDrive.empty()) requestDisk(diskInDrive, true); // (a restart with the disk in)
      else amigaRequestReset();
      snprintf(status, sizeof(status), "RESET");
      menuOpen = false;
    }
  }
}

static void drawMenu() {
  int rowH = 40, w = 1040, h = (kMenuRows + 4) * rowH + 40;
  int x = kPicX + (1280 - w) / 2, y = kPicY + (1024 - h) / 2;
  // a Workbench window: white frame, blue inside, title bar
  fillRect(x, y, w, h, kWbBlue);
  frameRect(x, y, w, h, 4, kWbWhite);
  fillRect(x, y, w, 40, kWbWhite);
  text(x + 16, y + 4, modelTitle(), 2, kWbBlue);
  int tx = x + 32, ty = y + 56;
  int n = (int)items.size();
  for (int r = 0; r < kMenuRows && menuTop + r < n; r++) {
    int i = menuTop + r;
    const Item &it = items[i];
    char line[96];
    uint32_t col = kWbWhite;
    switch (it.kind) {
    case IT_RESUME: snprintf(line, sizeof(line), "Back to the Amiga"); break;
    case IT_MOD: snprintf(line, sizeof(line), "MOD player"); break;
    case IT_OSK: snprintf(line, sizeof(line), oskOpen ? "Hide the keyboard (R3)" : "On-screen keyboard (R3)"); break;
    case IT_QUIT: snprintf(line, sizeof(line), quitArmed ? "Quit? Press X again" : "Quit to the PS5 home screen"); break;
    case IT_RESET: snprintf(line, sizeof(line), "Reset (Ctrl-Amiga-Amiga)"); break;
    case IT_VOLUME: snprintf(line, sizeof(line), "Volume               %3d%%", volumePct.load()); break;
#ifdef AMIBERRY
    case IT_MODEL:
      if (modelPick == amigaModel())
        snprintf(line, sizeof(line), "Model   < %s, Kickstart %s >", amigaModelName(modelPick), amigaModelKickstart(modelPick));
      else
        snprintf(line, sizeof(line), "Model   < %s >   X: switch", amigaModelName(modelPick));
      break;
#else
    case IT_MODEL: break;
#endif
    case IT_USBKBD:
      snprintf(line, sizeof(line), "USB keyboard  %s", !usbKbWanted ? "off (the PS5 has it)" : !usbKb.isOpen() ? "  none found"
                                                        : usbKb.hasMouse() ? "+ mouse in use" : "      in use");
      break;
    case IT_HEADER:
      snprintf(line, sizeof(line), "Disks:   X boot from it    [] swap into DF0:");
      col = kWbBlack;
      break;
    case IT_DISK:
      snprintf(line, sizeof(line), "%s%.48s", it.name == diskInDrive ? "DF0: " : "     ", it.name.c_str());
      if (it.name == diskInDrive) col = kWbOrange;
      break;
    case IT_NONE: snprintf(line, sizeof(line), "%s", it.name.c_str()); col = kWbBlack; break;
    }
    int ly = ty + r * rowH;
    if (i == menuSel) {
      fillRect(tx - 12, ly - 2, w - 40, rowH - 4, kWbOrange);
      col = kWbBlack;
    }
    text(tx, ly + 2, line, 2, col);
  }
  int by = y + h - 52;
  icon(ICON_CROSS, tx + 12, by + 16, 26);
  text(tx + 32, by, "choose", 2, kWbWhite);
  icon(ICON_CIRCLE, tx + 232, by + 16, 26);
  text(tx + 252, by, "back", 2, kWbWhite);
  text(tx + 420, by, "<> volume", 2, kWbWhite);
}

// ============================================================ the sides
static void sideRow(int x, int y, Icon ic, const char *btn, const char *what) {
  if (btn) buttonLabel(btn, x + 40, y + 16);
  else icon(ic, x + 40, y + 16, 26);
  text(x + 84, y, what, 2, kText);
}

static void drawSides() {
  fillRect(0, 0, kW, kH, kSideBg);
  if (oskOpen) {
    // the keyboard's help: left the keys, right the mouse and the rest
    text(28, 40, "AMIGA", 4, kWbOrange);
    text(28, 110, modelLabel(), 4, kWbWhite);
    int x = 8, y = 300, step = 50;
    text(x + 20, y, "KEYBOARD", 2, kWbOrange);
    sideRow(x, y += step, ICON_CROSS, "D-PAD", "CHOOSE");
    sideRow(x, y += step, ICON_CROSS, nullptr, "PRESS");
    sideRow(x, y += step, ICON_SQUARE, nullptr, "SPACE");
    sideRow(x, y += step, ICON_CIRCLE, nullptr, "RETURN");
    sideRow(x, y += step, ICON_CROSS, "PAD", "POINT,");
    text(x + 84, y += 34, "CLICK", 2, kText);
    int rx = 1600 + 8;
    y = 300;
    text(rx + 20, y, "MOUSE", 2, kWbOrange);
    y += step;
    text(rx + 20, y, "LEFT STICK", 2, kText);
    sideRow(rx, y += step, ICON_CROSS, "R1 R2", "LEFT");
    sideRow(rx, y += step, ICON_CROSS, "L1 L2", "RIGHT");
    y += step;
    sideRow(rx, y += step, ICON_CROSS, "R3", "HIDE KEYS");
    sideRow(rx, y += step, ICON_CROSS, "OPT", "MENU");
    fullCopies = 2;
    return;
  }
  // left: the name and the mouse; right: the joystick and the rest
  text(28, 40, "AMIGA", 4, kWbOrange);
  text(28, 110, modelLabel(), 4, kWbWhite);
  text(32, 180, "ON PS5", 2, kDim);
  int x = 8, y = 300, step = 50;
  text(x + 20, y, "MOUSE", 2, kWbOrange);
  y += step;
  text(x + 20, y, "LEFT STICK", 2, kText);
  y += 34;
  text(x + 20, y, "OR TOUCHPAD", 2, kText);
  y += step;
  sideRow(x, y, ICON_SQUARE, nullptr, "LEFT");
  sideRow(x, y += step, ICON_CROSS, "R1 R2", "LEFT");
  sideRow(x, y += step, ICON_CROSS, "PAD", "LEFT");
  sideRow(x, y += step, ICON_CIRCLE, nullptr, "RIGHT");
  sideRow(x, y += step, ICON_CROSS, "L1 L2", "RIGHT");

  int rx = 1600 + 8;
  y = 300;
  text(rx + 20, y, "JOYSTICK", 2, kWbOrange);
  y += step;
  text(rx + 20, y, "PORT 2", 2, kDim);
  y += step;
  sideRow(rx, y, ICON_CROSS, "D-PAD", "MOVE");
  sideRow(rx, y += step, ICON_CROSS, nullptr, "FIRE");
  y += step * 2;
  sideRow(rx, y, ICON_CROSS, "OPT", "MENU");
  y += 34;
  text(rx + 84, y += 20, "DISKS", 2, kDim);
  text(rx + 84, y += 30, "RESET, QUIT", 2, kDim);
  sideRow(rx, y += step, ICON_TRIANGLE, nullptr, "MOD PLAYER");
  sideRow(rx, y += step, ICON_CROSS, "R3", "KEYBOARD");
  fullCopies = 2;
}

// the disk in the drive and the speed, under the help on the left
static void drawStatus() {
  static char shown[200] = "";
  char now[200];
  bool led = running && drive_led();
  snprintf(now, sizeof(now), "%s|%d|%s|%d", diskInDrive.c_str(), emuFps.load(), status, led);
  if (strcmp(now, shown) == 0) return;
  snprintf(shown, sizeof(shown), "%s", now);
  fillRect(0, 900, 320, 180, kSideBg);
  text(28, 910, "DF0:", 2, kDim);
  // the drive light, as on the A500's case: green while the motor turns
  int lx = 28 + textWidth("DF0:", 2) + 16;
  fillRect(lx, 914, 44, 20, led ? rgb(0x10, 0x40, 0x10) : rgb(0x0c, 0x18, 0x0c));
  fillRect(lx + 3, 917, 38, 14, led ? rgb(0x30, 0xff, 0x40) : rgb(0x1c, 0x34, 0x1c));
  if (led) fillRect(lx + 6, 919, 14, 4, rgb(0xc0, 0xff, 0xc8)); // (a shine)
  std::string d = diskInDrive.empty() ? "(empty)" : diskInDrive;
  if (d.size() > 34) d = d.substr(0, 34);
  // the name in two lines of 17
  text(28, 944, d.substr(0, 17).c_str(), 1, kText);
  if (d.size() > 17) text(28, 964, d.substr(17).c_str(), 1, kText);
  char f[32];
  snprintf(f, sizeof(f), "%d FPS", emuFps.load());
  text(28, 1000, f, 2, kDim);
  if (status[0]) text(28, 1040, status, 1, kDim);
  fullCopies = 2;
}

// ============================================================ a message instead of the Amiga
static void showMessage(const char *title, const std::string &why, const char *hint1, const char *hint2) {
  fillRect(kPicX, kPicY, 1280, 1024, kWbBlue);
  text(kPicX + (1280 - textWidth(title, 4)) / 2, kPicY + 300, title, 4, kWbWhite);
  text(kPicX + (1280 - textWidth(why.c_str(), 2)) / 2, kPicY + 420, why.c_str(), 2, kWbOrange);
  text(kPicX + (1280 - textWidth(hint1, 2)) / 2, kPicY + 520, hint1, 2, kWbWhite);
  text(kPicX + (1280 - textWidth(hint2, 2)) / 2, kPicY + 560, hint2, 2, kWbWhite);
}

// ============================================================ the program
int main() {
  padInit();
  videoInit();
  drawSides();
  for (int i = 0; i < 640 * 512; i++) picture[i] = 0x02b5; // Workbench blue, until the Amiga draws
  composePicture();
  copyBlocks(scanout[0], 0, kBlocksX - 1);
  flushRange(scanout[0], kUsedBytes);
  sceVideoOutSubmitFlip(video, 0, 1, 1);
  sceVideoOutWaitVblank(video);
  sceSystemServiceHideSplashScreen();

  diskBuf = (uint8_t *)heap_caps_malloc(901120, 0);
  soundStart(); // (Paula's ring stays empty without a Kickstart: silence; the MOD player still plays)
  std::string err = AmigaFiles::loadKickstart(kickRom);
  bool haveA500Kick = err.empty();
#ifdef AMIBERRY
  // every Kickstart in the app, for the models (Amiberry: 2.04, 2.05, 3.1 too)
  AmigaFiles::forEachKickstart(amigaAddKickstart);
  for (int m = 0; m < amigaModelCount(); m++)
    if (amigaModelAvailable(m)) err.clear();
#endif
  if (err.empty()) {
    // the disk at start-up: default.adf, else the first one
    disks = AmigaFiles::listDisks();
    std::string boot;
    for (auto &d : disks)
      if (strcasecmp(d.c_str(), "default.adf") == 0) boot = d;
    if (boot.empty() && !disks.empty()) boot = disks[0];
    if (!boot.empty()) {
      uint8_t *adf = drive_alloc_adf();
      std::string e = adf ? AmigaFiles::readDisk(boot, adf) : "NO MEMORY FOR THE DISK";
      if (e.empty()) {
        drive_mount_ready();
        diskInDrive = boot;
      } else {
        snprintf(status, sizeof(status), "%s", e.c_str());
      }
    }
    AmigaOutput out = {picture, 640, 0, 0, nullptr};
    amigaMicros = [] { return (int64_t)ps5NowUs(); };
    if (amigaCoreInit(chipRam, slowRam, haveA500Kick ? kickRom : nullptr, out)) {
      ps5StartThread(emuThread, nullptr, 1024 * 1024);
      running = true;
    } else {
      err = "THE EMULATOR DID NOT START";
    }
  }

  int shown = 0;
  for (int64_t flip = 2;; ++flip) {
    sceVideoOutWaitVblank(video);
    padRead();
#ifdef AMIBERRY
    { // (another model running: its number at the side)
      static std::string shownModel;
      if (shownModel != modelLabel() && !modOpen) {
        shownModel = modelLabel();
        drawSides();
      }
    }
#endif
    usbToAmiga = running && !menuOpen && !modOpen;
    usbKeyboardFrame();
    held = padHeld | (usbToAmiga ? (usbNav & RAW_OPTIONS) : usbNav);
    pressed = held & ~prevHeld;
    prevHeld = held;

    if (modOpen) {
      // the MOD player has the TV (and the sound); the Amiga is paused
      modInput();
      if (modOpen) {
        if (modView <= 1) drawModPlayer();
        if (modView >= 1) drawModVisuals(modView);
        fullCopies = 1;
      }
    } else if (running) {
      if (menuOpen) {
        menuInput();
        mouseFrame(false);
        input_set_joy(0, 0, 0, 0, 0);
        input_set_lmb(0);
        input_set_rmb(0);
      } else {
        if (pressed & RAW_OPTIONS) menuOpenNow();
        if (pressed & RAW_TRIANGLE) modOpenNow(); // straight to the MOD player
        if (pressed & RAW_R3) oskOpenNow(!oskOpen);
        touchIsMouse = !oskOpen;
        mouseFrame(true);
        if (oskOpen) {
          // the keyboard has the D-pad, Cross, Square, Circle and the touchpad
          oskInput();
          input_set_lmb(((held & (RAW_R1 | RAW_R2)) || (usbMouseButtons & 1)) ? 1 : 0);
          input_set_rmb(((held & (RAW_L1 | RAW_L2)) || (usbMouseButtons & 2)) ? 1 : 0);
          input_set_joy(0, 0, 0, 0, 0);
        } else {
          input_set_lmb(((held & (RAW_R1 | RAW_R2 | RAW_SQUARE | RAW_TOUCHPAD)) || (usbMouseButtons & 1)) ? 1 : 0);
          input_set_rmb(((held & (RAW_L1 | RAW_L2 | RAW_CIRCLE)) || (usbMouseButtons & 2)) ? 1 : 0);
          // the joystick: the D-pad (the left stick is the mouse)
          input_set_joy((held & RAW_UP) != 0, (held & RAW_DOWN) != 0, (held & RAW_LEFT) != 0, (held & RAW_RIGHT) != 0,
                        (held & RAW_CROSS) != 0);
        }
      }
      if (!usbToAmiga || menuOpen) {
        oskDown = -1; // (every key went up for the menu or the MOD player)
        oskSticky = 0;
      }
      if (oskOpen) {
        composePictureSmall();
        drawOsk();
      } else {
        composePicture();
      }
      if (menuOpen) drawMenu();
      drawStatus();
    } else {
      showMessage("NO KICKSTART", err, "Put kick.rom (Kickstart 1.3, 256K) in amiga/assets/amiga",
                  "and build the app again - see amiga/README.md");
      text(kPicX + (1280 - textWidth("Options: the MOD player", 2)) / 2, kPicY + 680, "Options: the MOD player", 2,
           kWbOrange);
      if (pressed & RAW_OPTIONS) modOpenNow(); // (it needs no Kickstart)
    }

    int back = shown ^ 1;
    if (fullCopies > 0) {
      fullCopies--;
      copyBlocks(scanout[back], 0, kBlocksX - 1);
      flushRange(scanout[back], kUsedBytes);
    } else {
      int bx0 = kPicX / kBlock, bx1 = (kPicX + 1280 - 1) / kBlock;
      copyBlocks(scanout[back], bx0, bx1);
      flushBlocks(scanout[back], bx0, bx1);
    }
    if (sceVideoOutSubmitFlip(video, back, 1, flip) >= 0) shown = back;
  }
}
