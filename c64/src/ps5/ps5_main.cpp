// ps5_main.cpp
// Commodore 64 for the PS5 - the program.
//
// The emulator (src/core: the T-HMI-C64 emulator by retroelec, with the
// Waveshare ESP32-P4 port's changes) runs on its own thread at a real C64's
// speed. This file:
//
//   the TV picture   the C64 screen, 4x (1280 x 800 plus its border, the
//                    full height of the TV); with the on-screen keyboard
//                    up, 3x above the keyboard. Help at the sides.
//   the DualSense    the joystick (D-pad / left stick, Cross fires) and a few
//                    keys; Triangle brings up the on-screen keyboard;
//                    Options opens the menu
//   the keyboard     the Waveshare port's keyboard picture, 1.5x; the D-pad
//                    or the touchpad picks a key, Cross presses it
//   the menu         the programs and disks to load, RESET, the joystick
//                    port, the volume
//
// The video output follows the PS5 starter's screen code
// (ps5-native-app-boilerplate's demo renderer, Copyright (C) 2026
// BlackBearReloaded; changes for Breeze, MiladRTX), as NUCLEAR MISSION's does.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../core/C64Sys.h"
#include "../core/ExtCmdQueue.h"
#include "../core/platform/PlatformFactory.h"
#include "../core/platform/PlatformManager.h"
#include "../core/roms/charset.h"
#include "MusicList.h"
#include "P4KeyboardData.h"
#include "P4SidEngine.h"
#include "Visuals.h"
#include "ps5_app.h"
#include "ps5_sce.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================ colours
// RGBA as the TV buffer takes it: 0xAABBGGRR
static constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | b << 16 | g << 8 | r; }

// The C64's colours (Pepto's PAL palette)
static const uint32_t kC64Pal[16] = {
    rgb(0x00, 0x00, 0x00), rgb(0xff, 0xff, 0xff), rgb(0x68, 0x37, 0x2b), rgb(0x70, 0xa4, 0xb2),
    rgb(0x6f, 0x3d, 0x86), rgb(0x58, 0x8d, 0x43), rgb(0x35, 0x28, 0x79), rgb(0xb8, 0xc7, 0x6f),
    rgb(0x6f, 0x4f, 0x25), rgb(0x43, 0x39, 0x00), rgb(0x9a, 0x67, 0x59), rgb(0x44, 0x44, 0x44),
    rgb(0x6c, 0x6c, 0x6c), rgb(0x9a, 0xd2, 0x84), rgb(0x6c, 0x5e, 0xb5), rgb(0x95, 0x95, 0x95)};

static const uint32_t kSideBg = rgb(0x14, 0x12, 0x1c);
static const uint32_t kText = rgb(0xe0, 0xe0, 0xe8);
static const uint32_t kDim = rgb(0x8a, 0x88, 0x98);
static const uint32_t kTitle = rgb(0x8c, 0x7e, 0xe6);   // C64 light blue, brighter
static const uint32_t kSelect = rgb(0xff, 0xe0, 0x40);  // the keyboard's chosen key
static const uint32_t kIconCross = rgb(0x7c, 0xb2, 0xe8), kIconCircle = rgb(0xff, 0x66, 0x6e),
                      kIconSquare = rgb(0xe0, 0x8c, 0xd2), kIconTriangle = rgb(0x40, 0xe0, 0xb0);

// ============================================================ the TV picture
static const int kW = 1920, kH = 1080;
static const int kBlock = 128, kBlocksX = kW / kBlock, kBlocksY = (kH + kBlock - 1) / kBlock;
static const size_t kFrameBytes = 0x1000000, kMemBytes = kFrameBytes * 2, kMemAlign = 0x200000;
static const size_t kUsedBytes = (size_t)kBlocksX * kBlocksY << 16;
uint32_t g_frame[kW * kH]; // the TV picture, row by row
static void *scanout[2];
static int video = -1;

static inline size_t tiledOffset(unsigned x, unsigned y) {
  const uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                          ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                          ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
  const uint32_t blockIndex = (y >> 7) * kBlocksX + (x >> 7);
  return ((size_t)blockIndex << 16) + offset;
}
// Columns of 128x128 blocks bx0..bx1 into the tiled scanout buffer, in 4x8
// groups so the writes to write-combined memory run in address order
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
  if (video < 0) halt("Commodore 64: sceVideoOutOpen failed");
  size_t pool = sceKernelGetDirectMemorySize();
  if (pool < kMemBytes) halt("Commodore 64: not enough direct memory");
  int64_t phys = 0;
  if (sceKernelAllocateDirectMemory(0, (int64_t)pool, kMemBytes, kMemAlign, 3 /* wc garlic */, &phys) < 0)
    halt("Commodore 64: direct-memory allocation failed");
  void *mapped = nullptr;
  if (sceKernelMapDirectMemory(&mapped, kMemBytes, 0x33, 0, phys, kMemAlign) < 0)
    halt("Commodore 64: direct-memory mapping failed");
  scanout[0] = mapped;
  scanout[1] = (uint8_t *)mapped + kFrameBytes;
  struct {
    void *data, *metadata, *r0, *r1;
  } bufs[2] = {{scanout[0], nullptr, nullptr, nullptr}, {scanout[1], nullptr, nullptr, nullptr}};
  alignas(16) uint8_t attribute[80] = {};
  sceVideoOutSetFlipRate(video, 0);
  sceVideoOutSetBufferAttribute2(attribute, UINT64_C(0x8000000022000000) /* rgba8 srgb */, 0, kW, kH, 0, 0, 0);
  if (sceVideoOutRegisterBuffers2(video, 0, 0, bufs, 2, attribute, 0, nullptr) < 0)
    halt("Commodore 64: buffer registration failed");
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

// The C64's own letters (its character ROM, upper case / graphics set)
static uint8_t screenCode(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c >= '@' && c <= '_') return (uint8_t)(c - '@');
  if (c >= ' ' && c <= '?') return (uint8_t)c;
  return '?';
}
// Text at x, y, each C64 pixel scale x scale; reverse = the C64's inverse letters
static void text(int x, int y, const char *s, int scale, uint32_t col, bool reverse = false, uint32_t bg = 0) {
  for (; *s; s++, x += 8 * scale) {
    const uint8_t *g = charset_rom + screenCode(*s) * 8;
    for (int gy = 0; gy < 8; gy++)
      for (int gx = 0; gx < 8; gx++) {
        bool on = (g[gy] >> (7 - gx)) & 1;
        if (reverse) on = !on;
        if (on) fillRect(x + gx * scale, y + gy * scale, scale, scale, col);
        else if (reverse) fillRect(x + gx * scale, y + gy * scale, scale, scale, bg);
      }
  }
}
static int textWidth(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

// The DualSense's button symbols, size x size, centred on cx, cy
enum Icon { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE };
static void icon(Icon kind, int cx, int cy, int size) {
  int r = size / 2, t = size >= 24 ? 3 : 2;
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      bool on = false;
      uint32_t col = 0;
      switch (kind) {
      case ICON_CROSS: {
        int k = r * 7 / 10;
        on = (abs(dx - dy) <= t / 2 + 1 || abs(dx + dy) <= t / 2 + 1) && abs(dx) <= k && abs(dy) <= k;
        col = kIconCross;
        break;
      }
      case ICON_CIRCLE: {
        int d2 = dx * dx + dy * dy, ro = r * 8 / 10, ri = ro - t - 1;
        on = d2 <= ro * ro && d2 >= ri * ri;
        col = kIconCircle;
        break;
      }
      case ICON_SQUARE: {
        int k = r * 7 / 10;
        on = (abs(dx) <= k && abs(dy) <= k) && (abs(dx) > k - t - 1 || abs(dy) > k - t - 1);
        col = kIconSquare;
        break;
      }
      case ICON_TRIANGLE: {
        // apex at the top, base at the bottom
        int top = -r * 7 / 10, bot = r * 6 / 10;
        if (dy < top || dy > bot) break;
        float f = (float)(dy - top) / (bot - top);  // 0 at the apex, 1 at the base
        float half = f * r * 0.8f;
        float edge = half - fabsf((float)dx);
        on = edge >= 0 && (edge <= t + 0.5f || dy >= bot - t);
        col = kIconTriangle;
        break;
      }
      }
      if (on) g_frame[(cy + dy) * kW + cx + dx] = col;
    }
}

// A small boxed label for a button without a symbol (L1, R1, OPTIONS...)
static void buttonLabel(const char *name, int cx, int cy, int scale) {
  int w = textWidth(name, scale) + 6 * scale, h = 10 * scale;
  frameRect(cx - w / 2, cy - h / 2, w, h, scale > 1 ? 2 : 1, kDim);
  text(cx - textWidth(name, scale) / 2, cy - 4 * scale, name, scale, kText);
}

// ============================================================ the layout
enum Mode { MODE_PLAY, MODE_KEYS };
static Mode mode = MODE_PLAY;
static bool menuOpen = false;

struct Layout {
  int scale;          // C64 pixels to TV pixels
  int ax, ay, aw, ah; // the C64 area (border included)
  int ix, iy;         // the 320 x 200 picture inside it
};
// play: 4x, 32 C64 pixels of border at the sides and 35 above and below,
// about what a real PAL C64 shows - the full height of the TV
static const Layout kPlay = {4, 192, 0, 1536, 1080, 320, 140};
// keyboard up: 3x, a thinner border, the keyboard below
static const Layout kKeys = {3, 384, 4, 1152, 648, 480, 28};
static const int kKbX = 420, kKbY = 656, kKbW = 1080, kKbH = 420; // the keyboard, 1.5x
static const Layout &layout() { return mode == MODE_PLAY ? kPlay : kKeys; }

static void composeC64() {
  const Layout &L = layout();
  const uint32_t border = kC64Pal[g_c64Border & 15];
  static uint32_t row[kW];
  for (int y = L.ay; y < L.iy; y++) {
    uint32_t *out = g_frame + y * kW + L.ax;
    for (int x = 0; x < L.aw; x++) out[x] = border;
  }
  const int left = L.ix - L.ax, right = L.ax + L.aw - (L.ix + 320 * L.scale);
  for (int r = 0; r < 200; r++) {
    uint32_t *p = row;
    for (int x = 0; x < left; x++) *p++ = border;
    const uint8_t *src = g_c64Pixels + r * 320;
    for (int x = 0; x < 320; x++) {
      uint32_t c = kC64Pal[src[x] & 15];
      for (int k = 0; k < L.scale; k++) *p++ = c;
    }
    for (int x = 0; x < right; x++) *p++ = border;
    for (int k = 0; k < L.scale; k++) memcpy(g_frame + (L.iy + r * L.scale + k) * kW + L.ax, row, (size_t)L.aw * 4);
  }
  for (int y = L.iy + 200 * L.scale; y < L.ay + L.ah; y++) {
    uint32_t *out = g_frame + y * kW + L.ax;
    for (int x = 0; x < L.aw; x++) out[x] = border;
  }
}

// ============================================================ the on-screen keyboard
// The picture (720 x 280) at 1.5x in its three looks: normal, pressed, latched
static uint32_t kbImage[3][kKbW * kKbH];

static void keyboardInit() {
  static uint8_t idx[P4KB_W * P4KB_H];
  int at = 0;
  for (int i = 0; i + 1 < P4KB_RLE_SIZE && at < P4KB_W * P4KB_H; i += 2) {
    int run = p4kbRle[i] + 1;
    uint8_t c = p4kbRle[i + 1];
    while (run-- > 0 && at < P4KB_W * P4KB_H) idx[at++] = c;
  }
  for (int p = 0; p < 3; p++) {
    float pal[P4KB_COLORS][3];
    for (int c = 0; c < P4KB_COLORS; c++) {
      uint16_t v = p4kbPalette[p][c];
      pal[c][0] = (float)(((v >> 11) & 31) * 255 / 31);
      pal[c][1] = (float)(((v >> 5) & 63) * 255 / 63);
      pal[c][2] = (float)((v & 31) * 255 / 31);
    }
    // smooth 1.5x: each TV pixel blends the four keyboard pixels around it
    for (int y = 0; y < kKbH; y++) {
      float sy = (y + 0.5f) / 1.5f - 0.5f;
      int y0 = (int)sy;
      if (sy < 0) sy = 0, y0 = 0;
      int y1 = y0 + 1 < P4KB_H ? y0 + 1 : y0;
      float fy = sy - y0;
      for (int x = 0; x < kKbW; x++) {
        float sx = (x + 0.5f) / 1.5f - 0.5f;
        int x0 = (int)sx;
        if (sx < 0) sx = 0, x0 = 0;
        int x1 = x0 + 1 < P4KB_W ? x0 + 1 : x0;
        float fx = sx - x0;
        const float *a = pal[idx[y0 * P4KB_W + x0]], *b = pal[idx[y0 * P4KB_W + x1]];
        const float *c = pal[idx[y1 * P4KB_W + x0]], *d = pal[idx[y1 * P4KB_W + x1]];
        float ch[3];
        for (int k = 0; k < 3; k++)
          ch[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
        kbImage[p][y * kKbW + x] = rgb((uint32_t)(ch[0] + 0.5f), (uint32_t)(ch[1] + 0.5f), (uint32_t)(ch[2] + 0.5f));
      }
    }
  }
}

static int keyByName(const char *name) {
  for (int i = 0; i < P4KB_KEY_COUNT; i++)
    if (strcmp(p4kbKeys[i].name, name) == 0) return i;
  return -1;
}
static int kSpace, kReturn, kDel, kF1;

// A key's place on the TV
struct Rect {
  int x, y, w, h;
};
static Rect keyRect(int k) {
  const P4Key &K = p4kbKeys[k];
  int x0 = K.x * 3 / 2, y0 = K.y * 3 / 2, x1 = (K.x + K.w) * 3 / 2, y1 = (K.y + K.h) * 3 / 2;
  return {kKbX + x0, kKbY + y0, x1 - x0, y1 - y0};
}

// ============================================================ what the C64 is sent
static int selKey = 0;              // the keyboard's chosen key
static bool latch[3] = {};          // one-shot SHIFT, CTRL, C= (chosen with Cross)
static bool shiftLock = false;
static int downKey = -1;            // the key the C64 sees now, or -1
static uint8_t downMods = 0;        // and its modifier keys
static int downFrames = 0;          // frames it has been down
static uint32_t downSource = 0;     // the controller button holding it
static int modsOnlyFrames = 0;      // C= + SHIFT (switch upper / lower case)
static int joyPort = 2;

static bool isMod(int k, int &m) {
  switch (p4kbKeys[k].kind) {
  case P4KB_KIND_SHIFT: m = 0; return true;
  case P4KB_KIND_CTRL: m = 1; return true;
  case P4KB_KIND_CBM: m = 2; return true;
  default: return false;
  }
}

static uint8_t latchedMods() {
  return (uint8_t)((latch[0] || shiftLock ? 1 : 0) | (latch[1] ? 2 : 0) | (latch[2] ? 4 : 0));
}

static bool l1Shift = false; // L1 held in the keyboard: SHIFT

// A key goes down for the C64 (held by the controller button `source`)
static void pressKey(int k, uint32_t source) {
  if (k < 0) return;
  const P4Key &K = p4kbKeys[k];
  int m;
  if (isMod(k, m)) {
    latch[m] = !latch[m];
    // C= and SHIFT together: the C64 switches between upper and lower case
    if (latch[0] && latch[2]) {
      modsOnlyFrames = 4;
      latch[0] = latch[2] = false;
    }
    return;
  }
  if (K.kind == P4KB_KIND_SHIFTLOCK) {
    shiftLock = !shiftLock;
    return;
  }
  if (K.kind == P4KB_KIND_RESTORE) {
    ExtCmdQueue::ExternalCmd c{};
    c.cmd = ExtCmd::RESTORE;
    c.param[0] = 0;
    ExtCmdQueue::getInstance().push(c);
    return;
  }
  downKey = k;
  downMods = (uint8_t)(K.mods | latchedMods() | (l1Shift ? 1 : 0));
  downFrames = 0;
  downSource = source;
  latch[0] = latch[1] = latch[2] = false;
}

// Once a frame: what the C64 sees. A key stays down at least 4 frames, so
// the C64's keyboard scan (60 times a second) can't miss a quick press.
static void sendKey(uint32_t held) {
  if (downKey >= 0) {
    downFrames++;
    if (!(held & downSource) && downFrames >= 4) downKey = -1;
  }
  uint32_t v = 0x00ffff;
  if (downKey >= 0) {
    const P4Key &K = p4kbKeys[downKey];
    v = (uint32_t)K.dc00 | (uint32_t)K.dc01 << 8 | (uint32_t)downMods << 16;
  } else if (modsOnlyFrames > 0) {
    modsOnlyFrames--;
    v = 0x00ffff | 5u << 16;
  } else if (mode == MODE_KEYS && l1Shift) {
    v = 0x00ffff | 1u << 16;
  }
  g_c64Key.store(v, std::memory_order_release);
}

static void drawKeyboard() {
  for (int y = 0; y < kKbH; y++) memcpy(g_frame + (kKbY + y) * kW + kKbX, kbImage[0] + y * kKbW, kKbW * 4);
  for (int k = 0; k < P4KB_KEY_COUNT; k++) {
    int look = 0, m;
    if (k == downKey) look = 1;
    if (isMod(k, m)) {
      if (latch[m]) look = 2;
      if (m == 0 && l1Shift) look = 1;
      if (m == 0 && shiftLock) look = 2;
      if (downKey >= 0 && (downMods >> m & 1)) look = 1;
    }
    if (p4kbKeys[k].kind == P4KB_KIND_SHIFTLOCK && shiftLock) look = 2;
    if (!look) continue;
    Rect r = keyRect(k);
    for (int y = r.y; y < r.y + r.h; y++)
      memcpy(g_frame + y * kW + r.x, kbImage[look] + (y - kKbY) * kKbW + (r.x - kKbX), (size_t)r.w * 4);
  }
  Rect s = keyRect(selKey);
  frameRect(s.x - 2, s.y - 2, s.w + 4, s.h + 4, 4, kSelect);
}

// The next key from `from` in a direction (dx, dy: one of them +1 or -1)
static int keyToward(int from, int dx, int dy) {
  const P4Key &F = p4kbKeys[from];
  int fx = F.x + F.w / 2, fy = F.y + F.h / 2;
  int best = -1;
  long bestScore = 0;
  for (int pass = 0; pass < 2 && best < 0; pass++) {
    for (int k = 0; k < P4KB_KEY_COUNT; k++) {
      if (k == from) continue;
      const P4Key &K = p4kbKeys[k];
      int kx = K.x + K.w / 2, ky = K.y + K.h / 2;
      long score;
      if (dx) {
        if (abs(ky - fy) > F.h / 2) continue;          // the same row
        int d = (kx - fx) * dx;
        if (pass == 1) d = d <= 0 ? d + 2000 : -1;     // round to the other end
        if (d <= 0) continue;
        score = d;
      } else {
        int d = (ky - fy) * dy;
        if (pass == 1) d = d <= 0 ? d + 2000 : -1;
        if (d <= 0) continue;
        // the nearest row first, then the key most under / over this one
        score = (long)d * 1000 + abs(kx - fx);
      }
      if (best < 0 || score < bestScore) best = k, bestScore = score;
    }
  }
  return best < 0 ? from : best;
}

// ============================================================ the controller
static int padHandle = -1;
static const int kSample = 120, kSamples = 16;
static uint8_t (*padSamples)[kSample] = nullptr;
static uint32_t padHeld = 0;
static uint8_t padLX = 128, padLY = 128;
static bool padTouch = false;
static float padTX = 0, padTY = 0, padTW = 1920, padTH = 943;

enum : uint32_t {
  RAW_OPTIONS = 0x0008, RAW_UP = 0x0010, RAW_RIGHT = 0x0020, RAW_DOWN = 0x0040, RAW_LEFT = 0x0080,
  RAW_L2 = 0x0100, RAW_R2 = 0x0200, RAW_L1 = 0x0400, RAW_R1 = 0x0800, RAW_TRIANGLE = 0x1000,
  RAW_CIRCLE = 0x2000, RAW_CROSS = 0x4000, RAW_SQUARE = 0x8000, RAW_TOUCHPAD = 0x00100000,
  RAW_INTERCEPTED = 0x80000000u,
  // made from the left stick
  STICK_UP = 0x01000000, STICK_DOWN = 0x02000000, STICK_LEFT = 0x04000000, STICK_RIGHT = 0x08000000,
};

static void padInit() {
  sceUserServiceInitialize(nullptr);
  int32_t user = -1;
  if (sceUserServiceGetInitialUser(&user) < 0 || user < 0 || scePadInit() < 0) return;
  // (far bigger than the samples need, on its own, in case this firmware's
  // samples are bigger than 120 bytes)
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
    if (raw & RAW_INTERCEPTED) { // the system has the controller (the PS menu)
      any = 0;
      padLX = padLY = 128;
      padTouch = false;
      continue;
    }
    any |= raw;
    padLX = s[4];
    padLY = s[5];
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
  // the left stick as four more buttons (8 ways: two can be on)
  float lx = (padLX - 128) / 127.0f, ly = (padLY - 128) / 127.0f;
  if (lx < -0.45f) any |= STICK_LEFT;
  if (lx > 0.45f) any |= STICK_RIGHT;
  if (ly < -0.45f) any |= STICK_UP;
  if (ly > 0.45f) any |= STICK_DOWN;
  padHeld = any;
}

static uint32_t held = 0, pressed = 0, prevHeld = 0;
static uint64_t repeatAt[32];

// Pressed now, or held long enough to repeat (menus, the keyboard's D-pad)
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

// ============================================================ the emulator
alignas(C64Sys) static unsigned char cpuStore[sizeof(C64Sys)];
static C64Sys *cpu = nullptr;

static void cpuThread(void *) { cpu->run(); }

static void emulatorStart() {
  PlatformManager::initialize(PlatformNS::create());
  cpu = new (cpuStore) C64Sys();
  static uint8_t ram[1 << 16];
  cpu->init(ram, charset_rom);
  // the SID player's engine: it takes its copy of the C64 just after start-up
  P4SidEngine::get().attach(cpu);
  ps5StartThread(cpuThread, nullptr, 1024 * 1024);
  // the keyboard scan every 8 ms and the CIAs' clocks every 100 ms, as on the board
  PlatformManager::getInstance().startIntervalTimer([] { cpu->scanKeyboard(); }, 8000);
  PlatformManager::getInstance().startIntervalTimer(
      [] {
        cpu->cia1.updateTOD();
        cpu->cia2.updateTOD();
      },
      100000);
}

static void pushCmd(ExtCmd cmd, const char *textParam = nullptr, int p0 = 0) {
  ExtCmdQueue::ExternalCmd c{};
  c.cmd = cmd;
  c.param[0] = (uint8_t)p0;
  if (textParam) snprintf((char *)&c.param[2], sizeof(c.param) - 2, "%s", textParam);
  ExtCmdQueue::getInstance().push(c);
}

// ============================================================ messages
static char toastText[64] = "";
static uint64_t toastUntil = 0;
static void toast(const char *msg) {
  snprintf(toastText, sizeof(toastText), "%s", msg);
  toastUntil = ps5NowUs() + 2500000;
}
static void drawToast() {
  if (!toastText[0] || ps5NowUs() > toastUntil) return;
  const Layout &L = layout();
  int s = 3, w = textWidth(toastText, s) + 48, h = 8 * s + 32;
  int x = L.ax + (L.aw - w) / 2, y = L.ay + L.ah - h - 40;
  fillRect(x, y, w, h, kC64Pal[6]);
  frameRect(x, y, w, h, 4, kC64Pal[14]);
  text(x + 24, y + 16, toastText, s, kC64Pal[1]);
}

// ============================================================ the menu
enum ItemKind { IT_RESUME, IT_SID, IT_RESET, IT_PORT, IT_VOLUME, IT_QUIT, IT_HEADER, IT_FILE, IT_NONE };
struct Item {
  ItemKind kind;
  std::string name;
};
static std::vector<Item> items;
static int menuSel = 0, menuTop = 0;
static int volume = 80;
static const int kMenuRows = 16;
static std::string lastFile; // the program or disk chosen last
static bool sidOpenRequested = false; // (the SID player is further down)
static bool quitArmed = false;        // QUIT chosen once: Cross again closes the app

static void menuOpenNow() {
  items.clear();
  items.push_back({IT_RESUME, ""});
  items.push_back({IT_SID, ""});
  items.push_back({IT_RESET, ""});
  items.push_back({IT_PORT, ""});
  items.push_back({IT_VOLUME, ""});
  items.push_back({IT_QUIT, ""});
  items.push_back({IT_HEADER, "PROGRAMS AND DISKS"});
  std::vector<std::string> files = ps5ListC64Files();
  for (auto &f : files) items.push_back({IT_FILE, f});
  if (files.empty()) items.push_back({IT_NONE, "(NONE IN THE APP'S C64 FOLDER)"});
  menuOpen = true;
  // on the program chosen last time, or the first one
  menuSel = 0;
  for (int i = 0; i < (int)items.size(); i++) {
    if (items[i].kind != IT_FILE) continue;
    if (menuSel == 0 || items[i].name == lastFile) menuSel = i;
    if (items[i].name == lastFile) break;
  }
  menuTop = menuSel < kMenuRows ? 0 : menuSel - kMenuRows / 2;
}

static bool selectable(int i) { return items[i].kind != IT_HEADER && items[i].kind != IT_NONE; }

static std::string upperName(const std::string &s) {
  std::string u = s;
  for (auto &c : u) c = (char)toupper((unsigned char)c);
  return u;
}

static void loadFile(const std::string &file) {
  std::string base = file.substr(0, file.size() - 4);
  bool disk = upperName(file.substr(file.size() - 4)) == ".D64";
  // a fresh C64 (as if switched off and on), BASIC's start-up (about 3 s:
  // the emulator waits for READY), then the program into memory and RUN -
  // or the disk in the drive and LOAD"*",8,1 and RUN typed
  pushCmd(ExtCmd::DETACHD64);
  pushCmd(ExtCmd::RESET);
  pushCmd(ExtCmd::WAITBASIC);
  if (!disk) {
    pushCmd(ExtCmd::AUTOSTART, base.c_str());
  } else {
    pushCmd(ExtCmd::ATTACHD64, base.c_str());
    // "LO" with a shifted O is BASIC's short form of LOAD: the whole line fits
    // the C64's 10-key keyboard buffer
    pushCmd(ExtCmd::TYPETEXT, "L\xcf\"*\",8,1\r");
    pushCmd(ExtCmd::TYPETEXT, "RUN\r");
  }
  char msg[64];
  snprintf(msg, sizeof(msg), "LOADING %s", upperName(base).c_str());
  toast(msg);
}

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
  if (menuSel <= 6) menuTop = 0;
  if (items[menuSel].kind != IT_QUIT) quitArmed = false;
  if (menuTop > (int)items.size() - kMenuRows) menuTop = (int)items.size() > kMenuRows ? (int)items.size() - kMenuRows : 0;
  Item &it = items[menuSel];
  if (it.kind == IT_VOLUME && (repeating(left) || repeating(right))) {
    volume += (held & right) ? 10 : -10;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    ps5SoundSetVolume(volume);
  }
  if (it.kind == IT_PORT && (pressed & (left | right))) joyPort = 3 - joyPort;
  if (pressed & (RAW_CIRCLE | RAW_OPTIONS)) {
    menuOpen = false;
    quitArmed = false;
    return;
  }
  if (pressed & RAW_CROSS) {
    switch (it.kind) {
    case IT_QUIT:
      if (quitArmed) {
        ps5SoundSetVolume(0);
        sceSystemServiceLoadExec("exit", nullptr); // (the app closes)
      }
      quitArmed = true;
      break;
    case IT_RESUME: menuOpen = false; break;
    case IT_SID: sidOpenRequested = true; break;
    case IT_RESET:
      pushCmd(ExtCmd::DETACHD64);
      pushCmd(ExtCmd::RESET);
      menuOpen = false;
      toast("RESET");
      break;
    case IT_PORT: joyPort = 3 - joyPort; break;
    case IT_VOLUME: break;
    case IT_FILE:
      lastFile = it.name;
      loadFile(it.name);
      menuOpen = false;
      break;
    default: break;
    }
  }
}

static void drawMenu() {
  const Layout &L = layout();
  int s = L.scale == 4 ? 3 : 2; // letters: 24 or 16 pixels
  int rowH = 12 * s;
  int w = 44 * 8 * s + 64, h = (kMenuRows + 5) * rowH + 40;
  if (h > L.ah - 40) h = L.ah - 40;
  int x = L.ax + (L.aw - w) / 2, y = L.ay + (L.ah - h) / 2;
  fillRect(x, y, w, h, kC64Pal[6]);
  frameRect(x, y, w, h, 6, kC64Pal[14]);
  int tx = x + 32, ty = y + 24;
  text(tx, ty, "**** COMMODORE 64 ON PS5 ****", s, kC64Pal[14]);
  ty += rowH * 2;
  int n = (int)items.size();
  for (int r = 0; r < kMenuRows && menuTop + r < n; r++) {
    int i = menuTop + r;
    const Item &it = items[i];
    char line[64];
    switch (it.kind) {
    case IT_RESUME: snprintf(line, sizeof(line), "BACK TO THE C64"); break;
    case IT_SID: snprintf(line, sizeof(line), "SID MUSIC PLAYER"); break;
    case IT_QUIT: snprintf(line, sizeof(line), quitArmed ? "QUIT? PRESS X AGAIN" : "QUIT TO THE PS5 HOME SCREEN"); break;
    case IT_RESET: snprintf(line, sizeof(line), "RESET C64"); break;
    case IT_PORT: snprintf(line, sizeof(line), "JOYSTICK PORT        %d", joyPort); break;
    case IT_VOLUME: snprintf(line, sizeof(line), "VOLUME            %3d%%", volume); break;
    case IT_HEADER: snprintf(line, sizeof(line), "%s", it.name.c_str()); break;
    case IT_FILE: snprintf(line, sizeof(line), "%.40s", upperName(it.name).c_str()); break;
    case IT_NONE: snprintf(line, sizeof(line), "%s", it.name.c_str()); break;
    }
    int ly = ty + r * rowH;
    if (it.kind == IT_HEADER) {
      text(tx, ly + 2 * s, line, s, kC64Pal[7]);
    } else if (i == menuSel) {
      // the C64's own inverse letters, the whole width
      char pad[48];
      snprintf(pad, sizeof(pad), "%-42s", line);
      text(tx, ly + 2 * s, pad, s, kC64Pal[14], true, kC64Pal[6]);
    } else {
      text(tx, ly + 2 * s, line, s, it.kind == IT_NONE ? kC64Pal[15] : kC64Pal[14]);
    }
  }
  // the controls, at the bottom
  int by = y + h - rowH - 12;
  int ix = tx;
  icon(ICON_CROSS, ix + 4 * s, by + 4 * s, 10 * s);
  text(ix + 12 * s, by, "CHOOSE", s, kC64Pal[1]);
  ix += 22 * 8 * s / 2;
  icon(ICON_CIRCLE, ix + 4 * s, by + 4 * s, 10 * s);
  text(ix + 12 * s, by, "BACK", s, kC64Pal[1]);
  ix += 16 * 8 * s / 2 + 8 * s;
  text(ix, by, "<> CHANGE", s, kC64Pal[1]);
}

// ============================================================ the sides
static int fullCopies = 2; // the sides changed: both scanout buffers need them

// One line of help: a button (its symbol, or its name in a box) and what it does.
// labelCx: the button's centre from x; textDx: where the words start
static void sideRow(int x, int y, int s, Icon ic, const char *btn, const char *what, int labelCx, int textDx) {
  if (btn) buttonLabel(btn, x + labelCx, y + 4 * s, s);
  else icon(ic, x + labelCx, y + 4 * s, 11 * s);
  text(x + textDx, y, what, s, kText);
}

static void drawSides() {
  fillRect(0, 0, kW, kH, kSideBg);
  const int s = 2;
  if (mode == MODE_PLAY) {
    // left: the name and the joystick port; right: the controls
    text(24, 60, "C64", 6, kTitle);
    text(28, 120, "ON PS5", 2, kDim);
    text(28, 860, "JOYSTICK", 2, kDim);
    text(28, 890, "PORT", 2, kDim);
    char p[4];
    snprintf(p, sizeof(p), "%d", joyPort);
    text(120, 880, p, 4, kText);
    int x = 1728 + 8, y = 300, step = 52, lc = 28, td = 64;
    sideRow(x, y, s, ICON_CROSS, nullptr, "FIRE", lc, td);
    sideRow(x, y += step, s, ICON_SQUARE, nullptr, "SPACE", lc, td);
    sideRow(x, y += step, s, ICON_CIRCLE, nullptr, "RETURN", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "R1", "F1", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "L1", "PORT", lc, td);
    sideRow(x, y += step * 3 / 2, s, ICON_TRIANGLE, nullptr, "KEYS", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "OPT", "MENU", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "PAD", "SID", lc, td);
  } else {
    int x = 32, y = 130, step = 56, lc = 50, td = 110;
    text(x, 44, "KEYBOARD", 3, kTitle);
    sideRow(x, y, s, ICON_CROSS, "D-PAD", "CHOOSE KEY", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, nullptr, "PRESS KEY", lc, td);
    sideRow(x, y += step, s, ICON_SQUARE, nullptr, "SPACE", lc, td);
    sideRow(x, y += step, s, ICON_CIRCLE, nullptr, "DELETE", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "R1", "RETURN", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "L1", "SHIFT", lc, td);
    text(x, y += step * 3 / 2, "TOUCHPAD: POINT", s, kText);
    text(x, y += 28, "AT A KEY, CLICK", s, kText);
    text(x, y += 28, "TO PRESS IT", s, kText);
    sideRow(x, y += step * 3 / 2, s, ICON_TRIANGLE, nullptr, "JOYSTICK", lc, td);
    sideRow(x, y += step, s, ICON_CROSS, "OPT", "MENU", lc, td);
    int rx = 1552;
    text(rx, 44, "SHIFT CTRL C=", 2, kDim);
    text(rx, 74, "PRESSED ONCE,", 2, kDim);
    text(rx, 104, "HELD FOR THE", 2, kDim);
    text(rx, 134, "NEXT KEY", 2, kDim);
    text(rx, 194, "C= THEN SHIFT:", 2, kDim);
    text(rx, 224, "UPPER/LOWER", 2, kDim);
    text(rx, 254, "CASE", 2, kDim);
  }
  fullCopies = 2;
}

// ============================================================ the SID music player
// A screen of its own (the menu's SID MUSIC PLAYER): the tunes in the app's
// assets/c64/music folder, listed by music.txt (tools/make_music_list.py: a
// PS5 app can't list a folder), played on the emulated C64 by the board's
// P4SidEngine. Opening it saves the C64 as it is; closing it puts it back,
// so a game carries on where it was.
static bool sidOpen = false;
static MusicList music;
static bool musicTried = false, musicLoaded = false;
static std::string sidFolder;              // the folder shown ("" = the top)
static int sidItems[65536];                // what is in it: indices into music
static int sidCount = 0, sidSel = 0, sidTop = 0;
static int sidPlaying = -1;                // the tune playing (index into music)
static int sidWaiting = -1;                // chosen while the C64 was still starting: plays when it can
static char sidMsg[64] = "";
static uint8_t sidFile[P4SidEngine::MAX_FILE];
static const int kSidRows = 19;
// the view (Triangle): 0 the list and voices, 1 the same with the channel bars
// over it, 2... the full-screen visuals
static int sidView = 0;
static const int kSidViews = 2 + Visuals::COUNT;
static Visuals sidVisuals;
static uint64_t sidVisLast = 0;
static uint8_t sidPrevGate[3];

static std::string musicFolder() { return std::string(ps5AppDir()) + "/assets/c64/music/"; }

static void sidList(const std::string &folder, int select) {
  sidFolder = folder;
  int n = musicLoaded ? music.children(folder.c_str(), sidItems, 65536) : 0;
  sidCount = 0;
  for (int i = 0; i < n; i++) // (folders and SID tunes: modules are the Amiga's)
    if (music[sidItems[i]].kind != 'M') sidItems[sidCount++] = sidItems[i];
  sidSel = 0;
  for (int i = 0; i < sidCount; i++)
    if (sidItems[i] == select) sidSel = i;
  sidTop = sidSel < kSidRows ? 0 : sidSel - kSidRows / 2;
}

static void sidOpenNow() {
  if (!musicTried) {
    musicTried = true;
    musicLoaded = music.load((musicFolder() + "music.txt").c_str(), ps5BigAlloc, ps5BigFree);
  }
  if (sidCount == 0) sidList("", -1);
  P4SidEngine::get().enter();
  sidOpen = true;
  menuOpen = false;
  downKey = -1;
  fullCopies = 2;
}

static void sidClose() {
  P4SidEngine::get().stop();
  P4SidEngine::get().leave();
  sidOpen = false;
  sidPlaying = -1;
  drawSides();
}

static void sidPlay(int index, int song) {
  const MusicEntry &e = music[index];
  FILE *f = fopen((musicFolder() + e.path).c_str(), "rb");
  if (!f) {
    snprintf(sidMsg, sizeof(sidMsg), "CANNOT OPEN %.40s", e.name);
    return;
  }
  size_t len = fread(sidFile, 1, sizeof(sidFile), f);
  fclose(f);
  const char *err = P4SidEngine::get().load(sidFile, len, song);
  if (err) {
    snprintf(sidMsg, sizeof(sidMsg), "%s", err);
    return;
  }
  sidMsg[0] = 0;
  sidPlaying = index;
}

// The next (or previous) tune in the playing tune's folder; -1 = none
static int sidNeighbour(int index, int dir) {
  const MusicEntry &e = music[index];
  static int around[65536];
  std::string folder(e.parent, (size_t)e.parentLen);
  int n = music.children(folder.c_str(), around, 65536);
  for (int i = 0; i < n; i++) {
    if (around[i] != index) continue;
    for (int j = i + dir; j >= 0 && j < n; j += dir)
      if (music[around[j]].kind == 'S') return around[j];
  }
  return -1;
}

static void sidNextTune(int dir) {
  if (sidPlaying < 0) return;
  int n = sidNeighbour(sidPlaying, dir);
  if (n >= 0) sidPlay(n, music[n].start);
  else P4SidEngine::get().stop(), sidPlaying = -1;
}

static void sidInput() {
  P4SidEngine &eng = P4SidEngine::get();
  const uint32_t up = RAW_UP | STICK_UP, down = RAW_DOWN | STICK_DOWN, left = RAW_LEFT | STICK_LEFT,
                 right = RAW_RIGHT | STICK_RIGHT;
  if (pressed & RAW_OPTIONS) {
    sidClose();
    return;
  }
  if (pressed & RAW_TRIANGLE) sidView = (sidView + 1) % kSidViews;
  if (sidView > 1 && (pressed & RAW_CIRCLE)) {
    sidView = 0; // (back to the list)
    pressed &= ~RAW_CIRCLE;
  }
  if (sidCount > 0 && sidView <= 1) {
    if (repeating(up)) sidSel = (sidSel - 1 + sidCount) % sidCount;
    if (repeating(down)) sidSel = (sidSel + 1) % sidCount;
    if (repeating(left)) sidSel = sidSel - kSidRows < 0 ? 0 : sidSel - kSidRows;
    if (repeating(right)) sidSel = sidSel + kSidRows >= sidCount ? sidCount - 1 : sidSel + kSidRows;
    if (sidSel < sidTop) sidTop = sidSel;
    if (sidSel >= sidTop + kSidRows) sidTop = sidSel - kSidRows + 1;
  }
  if ((pressed & RAW_CROSS) && sidView <= 1) {
    if (sidCount > 0) {
      int i = sidItems[sidSel];
      if (music[i].kind == 'D') sidList(music[i].path, -1);
      else if (music[i].kind == 'S') {
        // (the engine must have taken the C64 over first: its switch-over
        // clears anything asked of it before)
        if (eng.ready() && eng.isActive()) sidPlay(i, music[i].start);
        else sidWaiting = i; // (still starting: in a moment)
      }
    }
  }
  if (pressed & RAW_CIRCLE) {
    if (sidFolder.empty()) {
      sidClose();
      return;
    }
    // up a folder, on the one just left
    size_t slash = sidFolder.rfind('/');
    std::string up = slash == std::string::npos ? "" : sidFolder.substr(0, slash);
    int was = -1;
    for (int i = 0; i < music.size(); i++)
      if (music[i].kind == 'D' && sidFolder == music[i].path) was = i;
    sidList(up, was);
  }
  if (sidPlaying >= 0) {
    if (pressed & RAW_SQUARE) eng.setPaused(!eng.isPaused());
    if (pressed & RAW_TOUCHPAD) {
      eng.stop();
      sidPlaying = -1;
    }
    if (pressed & RAW_R1) eng.selectSong(eng.currentSong() + 1);
    if (pressed & RAW_L1) eng.selectSong(eng.currentSong() - 1);
    if (pressed & RAW_R2) sidNextTune(1);
    if (pressed & RAW_L2) sidNextTune(-1);
  }
  if (sidWaiting >= 0 && eng.ready() && eng.isActive()) {
    sidPlay(sidWaiting, music[sidWaiting].start);
    sidWaiting = -1;
  }
  // the end of a song: the next song of the tune, then the next tune
  if (sidPlaying >= 0 && eng.isPlaying() && !eng.isPaused()) {
    const MusicEntry &e = music[sidPlaying];
    uint32_t ms = eng.framesPlayed() * 20, len = MusicList::songLength(e, eng.currentSong());
    bool over = len ? ms >= len : (eng.silentFrames() >= 250 && ms > 3000); // (unknown: 5 s of silence)
    if (over) {
      if (eng.currentSong() < e.songs) eng.selectSong(eng.currentSong() + 1);
      else sidNextTune(1);
    }
  }
}

static void sidNoteName(uint16_t freq, char out[5]) {
  if (freq < 100) {
    strcpy(out, "---");
    return;
  }
  static const char *names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  double hz = freq * (985248.0 / 16777216.0);
  int midi = (int)lround(69 + 12 * log2(hz / 440.0));
  if (midi < 0) midi = 0;
  if (midi > 127) midi = 127;
  snprintf(out, 5, "%s%d", names[midi % 12], midi / 12 - 1);
}

static void timeText(uint32_t ms, char *out, size_t n) {
  snprintf(out, n, "%u:%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
}

static void drawSidPlayer() {
  P4SidEngine &eng = P4SidEngine::get();
  const uint32_t blue = kC64Pal[6], light = kC64Pal[14], white = kC64Pal[1], yellow = kC64Pal[7];
  fillRect(0, 0, kW, kH, light);
  fillRect(48, 48, kW - 96, kH - 96, blue);
  text(96, 80, "**** SID MUSIC PLAYER ****", 4, light);

  // ---- left: the folder and what is in it
  char line[80];
  snprintf(line, sizeof(line), "/%.40s", sidFolder.c_str());
  text(96, 150, line, 2, white);
  int lx = 96, ly = 190, rowH = 38;
  if (!musicLoaded) {
    text(lx, ly, "NO MUSIC IN THE APP.", 3, light);
    text(lx, ly + 50, "PUT .SID FILES IN", 2, light);
    text(lx, ly + 80, "C64/ASSETS/C64/MUSIC", 2, light);
    text(lx, ly + 110, "AND BUILD AGAIN", 2, light);
  } else if (sidCount == 0) {
    text(lx, ly, "(EMPTY)", 3, light);
  }
  for (int r = 0; r < kSidRows && sidTop + r < sidCount; r++) {
    int i = sidItems[sidTop + r];
    const MusicEntry &e = music[i];
    if (e.kind == 'D') snprintf(line, sizeof(line), "%.30s/", e.name);
    else snprintf(line, sizeof(line), "%.31s", e.title[0] ? e.title : e.name);
    bool sel = sidTop + r == sidSel;
    uint32_t col = i == sidPlaying ? yellow : light;
    if (sel) {
      char pad[40];
      snprintf(pad, sizeof(pad), "%-32s", line);
      text(lx, ly + r * rowH, pad, 3, col, true, blue);
    } else {
      text(lx, ly + r * rowH, line, 3, e.kind == 'D' ? white : col);
    }
  }
  if (sidCount > kSidRows) {
    snprintf(line, sizeof(line), "%d-%d OF %d", sidTop + 1, sidTop + kSidRows < sidCount ? sidTop + kSidRows : sidCount,
             sidCount);
    text(lx, ly + kSidRows * rowH + 6, line, 2, light);
  }

  // ---- right: what is playing
  int rx = 1000, ry = 150;
  fillRect(rx - 30, 140, 4, 820, light);
  if (!eng.ready()) {
    text(rx, ry, "STARTING THE C64...", 3, light);
  } else if (sidPlaying < 0 || !eng.isPlaying()) {
    text(rx, ry, "NOTHING PLAYING", 3, light);
    text(rx, ry + 50, "CHOOSE A TUNE ON THE LEFT", 2, light);
  } else {
    const MusicEntry &e = music[sidPlaying];
    const SidTuneInfo &t = eng.tuneInfo();
    text(rx, ry, t.name[0] ? t.name : e.name, 3, white);
    text(rx, ry + 44, t.author, 2, light);
    text(rx, ry + 74, t.released, 2, light);
    snprintf(line, sizeof(line), "SONG %d OF %d", eng.currentSong(), t.songs);
    text(rx, ry + 130, line, 3, white);
    uint32_t ms = eng.framesPlayed() * 20, len = MusicList::songLength(e, eng.currentSong());
    char a[16], b[16];
    timeText(ms, a, sizeof(a));
    if (len) {
      timeText(len, b, sizeof(b));
      snprintf(line, sizeof(line), "%s / %s", a, b);
    } else {
      snprintf(line, sizeof(line), "%s", a);
    }
    text(rx, ry + 180, line, 3, white);
    // the progress bar
    int bw = 780;
    frameRect(rx, ry + 222, bw, 20, 3, light);
    if (len) fillRect(rx + 4, ry + 226, (int)((uint64_t)(bw - 8) * (ms < len ? ms : len) / len), 12, white);
    snprintf(line, sizeof(line), "SID %d%s%s", eng.modelInUse(), t.rsid ? "  RSID" : "",
             eng.isPaused() ? "   PAUSED" : "");
    text(rx, ry + 262, line, 2, eng.isPaused() ? yellow : light);

    // the three voices: envelope level, waveform, note
    const SID &sid = cpu->sid;
    for (int v = 0; v < 3; v++) {
      int vx = rx + v * 260, vy = ry + 330, bh = 300;
      snprintf(line, sizeof(line), "VOICE %d", v + 1);
      text(vx, vy, line, 2, white);
      uint8_t ctrl = sid.sidVoice[v].control;
      const char *wave = (ctrl & 0x80) ? "NOISE" : (ctrl & 0x40) ? "PULSE" : (ctrl & 0x20) ? "SAW" : (ctrl & 0x10) ? "TRI" : "-";
      text(vx, vy + 30, wave, 2, light);
      char note[5];
      sidNoteName((uint16_t)(sid.sidreg[v * 7] | sid.sidreg[v * 7 + 1] << 8), note);
      if (sid.sidVoice[v].env == 0) strcpy(note, "---");
      text(vx + 120, vy + 30, note, 2, light);
      frameRect(vx, vy + 64, 200, bh, 3, light);
      int h = sid.sidVoice[v].env * (bh - 8) / 255;
      if (h > 0) fillRect(vx + 4, vy + 64 + bh - 4 - h, 192, h, v == 0 ? kC64Pal[13] : v == 1 ? kC64Pal[7] : kC64Pal[10]);
    }
  }
  if (sidMsg[0]) text(rx, 900, sidMsg, 2, yellow);

  // ---- the controls, along the bottom
  int by = 990, x = 96;
  auto ctl = [&](Icon ic, const char *btn, const char *what) {
    if (btn) buttonLabel(btn, x + 24, by + 8, 2);
    else icon(ic, x + 16, by + 8, 26);
    int bw = btn ? textWidth(btn, 2) + 24 : 32;
    text(x + bw + 10, by, what, 2, white);
    x += bw + 10 + textWidth(what, 2) + 40;
  };
  ctl(ICON_CROSS, nullptr, "PLAY/OPEN");
  ctl(ICON_CIRCLE, nullptr, "BACK");
  ctl(ICON_SQUARE, nullptr, "PAUSE");
  ctl(ICON_TRIANGLE, nullptr, "VISUALS");
  ctl(ICON_CROSS, "PAD", "STOP");
  ctl(ICON_CROSS, "L1 R1", "SONG");
  ctl(ICON_CROSS, "L2 R2", "TUNE");
  ctl(ICON_CROSS, "OPT", "C64");
}

// The SID player's visuals: the voices' envelopes, notes and new notes, and
// the sound itself, given to the shared Visuals (common/Visuals.h)
static void sidGlyph(uint32_t *, int x, int y, char c, int scale, uint32_t col) {
  char s1[2] = {c, 0};
  text(x, y, s1, scale, col);
}

// view: 1 the bars over the player's screen, 2... a full-screen visual
static void drawSidVisuals(int view) {
  P4SidEngine &eng = P4SidEngine::get();
  uint64_t now = ps5NowUs();
  float dt = sidVisLast ? (now - sidVisLast) / 1e6f : 1 / 60.0f;
  sidVisLast = now;
  VisInput in = {};
  in.channels = 3;
  bool on = sidPlaying >= 0 && eng.isPlaying();
  const SID &sid = cpu->sid;
  for (int v = 0; v < 3; v++) {
    uint8_t ctrl = sid.sidVoice[v].control;
    in.level[v] = on ? sid.sidVoice[v].env / 255.0f : 0;
    uint16_t freq = (uint16_t)(sid.sidreg[v * 7] | sid.sidreg[v * 7 + 1] << 8);
    if (on && freq >= 100 && sid.sidVoice[v].env > 0) {
      float midi = 69 + 12 * log2f(freq * (985248.0f / 16777216.0f) / 440.0f);
      float p = (midi - 24) / 72.0f;
      in.pitch[v] = p < 0.01f ? 0.01f : p > 1 ? 1 : p;
    }
    in.hit[v] = on && (ctrl & 1) && !(sidPrevGate[v] & 1); // the gate opened: a new note
    sidPrevGate[v] = ctrl;
  }
  static int16_t wave[1024];
  in.wave = wave;
  in.waveN = ps5SoundRecent(wave, 1024);
  static char info[96], title[64], author[64];
  if (on) {
    const SidTuneInfo &t = eng.tuneInfo();
    const MusicEntry &e = music[sidPlaying];
    snprintf(title, sizeof(title), "%s", t.name[0] ? t.name : e.name);
    snprintf(author, sizeof(author), "%s", t.author);
    uint32_t ms = eng.framesPlayed() * 20, len = MusicList::songLength(e, eng.currentSong());
    char a[16], b[16];
    timeText(ms, a, sizeof(a));
    if (len) timeText(len, b, sizeof(b));
    snprintf(info, sizeof(info), "%s  -  SONG %d OF %d  -  %s%s%s", author, eng.currentSong(), t.songs, a, len ? " / " : "",
             len ? b : "");
    in.progress = len ? (float)ms / len : -1;
  } else {
    snprintf(title, sizeof(title), "NOTHING PLAYING");
    author[0] = 0;
    snprintf(info, sizeof(info), "CIRCLE: THE LIST, TO CHOOSE A TUNE");
    in.progress = -1;
  }
  in.title = title;
  in.author = author;
  in.info = info;
  in.paused = eng.isPaused();
  in.caption = "SID MUSIC";
  static const VisFont font = {sidGlyph, 8, 8};
  if (view == 1) sidVisuals.drawBars(g_frame, in, dt);
  else sidVisuals.draw(g_frame, view - 2, in, font, dt);
}

// ============================================================ once a frame
static void frameInput() {
  padRead();
  held = padHeld;
  pressed = held & ~prevHeld;
  prevHeld = held;

  if (sidOpen) {
    g_c64Joy[0].store(0xff);
    g_c64Joy[1].store(0xff);
    sidInput();
    sendKey(held);
    return;
  }
  if (menuOpen) {
    g_c64Joy[0].store(0xff);
    g_c64Joy[1].store(0xff);
    menuInput();
    sendKey(held);
    if (sidOpenRequested) {
      sidOpenRequested = false;
      sidOpenNow();
    } else if (!menuOpen) {
      drawSides(); // (the port may have changed)
    }
    return;
  }
  if (pressed & RAW_OPTIONS) {
    menuOpenNow();
    downKey = -1;
    sendKey(held);
    return;
  }
  if (pressed & RAW_TRIANGLE) {
    mode = mode == MODE_PLAY ? MODE_KEYS : MODE_PLAY;
    downKey = -1;
    latch[0] = latch[1] = latch[2] = false;
    drawSides();
  }

  uint8_t joy = 0xff;
  if (mode == MODE_PLAY) {
    if (held & (RAW_UP | STICK_UP)) joy &= ~1;
    if (held & (RAW_DOWN | STICK_DOWN)) joy &= ~2;
    if (held & (RAW_LEFT | STICK_LEFT)) joy &= ~4;
    if (held & (RAW_RIGHT | STICK_RIGHT)) joy &= ~8;
    if (held & (RAW_CROSS | RAW_R2)) joy &= ~16;
    if (pressed & RAW_SQUARE) pressKey(kSpace, RAW_SQUARE);
    if (pressed & RAW_CIRCLE) pressKey(kReturn, RAW_CIRCLE);
    if (pressed & RAW_R1) pressKey(kF1, RAW_R1);
    if (pressed & RAW_TOUCHPAD) { // straight to the SID music player
      sidOpenNow();
      return;
    }
    if (pressed & RAW_L1) {
      joyPort = 3 - joyPort;
      char m[32];
      snprintf(m, sizeof(m), "JOYSTICK IN PORT %d", joyPort);
      toast(m);
      drawSides();
    }
  } else {
    l1Shift = (held & RAW_L1) != 0;
    if (repeating(RAW_LEFT | STICK_LEFT)) selKey = keyToward(selKey, -1, 0);
    if (repeating(RAW_RIGHT | STICK_RIGHT)) selKey = keyToward(selKey, 1, 0);
    if (repeating(RAW_UP | STICK_UP)) selKey = keyToward(selKey, 0, -1);
    if (repeating(RAW_DOWN | STICK_DOWN)) selKey = keyToward(selKey, 0, 1);
    // the touchpad is the keyboard: where the finger is picks the key
    if (padTouch) {
      int tx = (int)(padTX * P4KB_W), ty = (int)(padTY * P4KB_H);
      for (int k = 0; k < P4KB_KEY_COUNT; k++) {
        const P4Key &K = p4kbKeys[k];
        if (tx >= K.x && tx < K.x + K.w && ty >= K.y && ty < K.y + K.h) selKey = k;
      }
    }
    if (pressed & RAW_CROSS) pressKey(selKey, RAW_CROSS);
    if (pressed & RAW_TOUCHPAD) pressKey(selKey, RAW_TOUCHPAD);
    if (pressed & RAW_SQUARE) pressKey(kSpace, RAW_SQUARE);
    if (pressed & RAW_CIRCLE) pressKey(kDel, RAW_CIRCLE);
    if (pressed & RAW_R1) pressKey(kReturn, RAW_R1);
  }
  g_c64Joy[joyPort - 1].store(joy, std::memory_order_release);
  g_c64Joy[2 - joyPort].store(0xff, std::memory_order_release);
  sendKey(held);
}

// ============================================================ the program
int main() {
  mkdir(ps5DataDir(), 0777);
  padInit();
  videoInit();
  keyboardInit();
  kSpace = keyByName("SPACE");
  kReturn = keyByName("RETURN");
  kDel = keyByName("INST/DEL");
  kF1 = keyByName("F1");
  selKey = kReturn;
  drawSides();
  copyBlocks(scanout[0], 0, kBlocksX - 1);
  flushRange(scanout[0], kUsedBytes);
  sceVideoOutSubmitFlip(video, 0, 1, 1);
  sceVideoOutWaitVblank(video);
  sceSystemServiceHideSplashScreen();

  emulatorStart();
  ps5SoundSetVolume(volume);

  int shown = 0;
  Mode drawnMode = mode;
  for (int64_t flip = 2;; ++flip) {
    sceVideoOutWaitVblank(video);
    frameInput();
    if (mode != drawnMode) drawnMode = mode;
#ifdef PS5_SIM
    if (getenv("SIM_SPEED")) { // while a SID tune plays: the C64's frames per real second (50 = right)
      static uint64_t t0 = 0;
      static uint32_t f0 = 0;
      uint32_t f = P4SidEngine::get().framesPlayed();
      if (flip % 600 == 0) {
        uint64_t t = ps5NowUs();
        if (t0) fprintf(stderr, "SPEED: %.3f frames/s\n", (f - f0) * 1e6 / (double)(t - t0));
        t0 = t;
        f0 = f;
      }
    }
#endif
    cpu->vic.refresh(); // the newest finished C64 picture into g_c64Pixels
    if (sidOpen) {
      if (sidView <= 1) drawSidPlayer();
      if (sidView >= 1) drawSidVisuals(sidView);
      fullCopies = 1; // (the player has the whole TV)
    } else {
      composeC64();
      if (mode == MODE_KEYS) drawKeyboard();
      if (menuOpen) drawMenu();
      drawToast();
    }
    int back = shown ^ 1;
    if (fullCopies > 0) {
      fullCopies--;
      copyBlocks(scanout[back], 0, kBlocksX - 1);
      flushRange(scanout[back], kUsedBytes);
    } else {
      const Layout &L = layout();
      int bx0 = L.ax / kBlock, bx1 = (L.ax + L.aw - 1) / kBlock;
      copyBlocks(scanout[back], bx0, bx1);
      flushBlocks(scanout[back], bx0, bx1);
    }
    if (sceVideoOutSubmitFlip(video, back, 1, flip) >= 0) shown = back;
  }
}
