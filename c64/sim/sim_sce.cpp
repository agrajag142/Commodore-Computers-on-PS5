// sim_sce.cpp
// The PC simulator: stand-ins for the PS5 system functions in ps5_sce.h, so
// the app can be built and run on Linux without the console (sim/build.sh).
//
//   video         the two scanout buffers in ordinary memory; a flip can be
//                 saved as a picture (SIM_SHOTS="300,900": those frames, as
//                 shot-<frame>.ppm in the current folder)
//   vblank        60 a second, from the clock; SIM_FRAMES=n stops after n
//   controller    a script: SIM_PAD="120-125:4000;300-302:1000" holds those
//                 buttons (the PS5's bits, hex) for those frames;
//                 SIM_TOUCH="200-260:0.5,0.4" puts a finger on the touchpad
//   sound         AudioOut takes its time like the real one; SIM_WAV=file
//                 also writes what was played
//   notifications printed

#include "../src/ps5/ps5_sce.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

static uint64_t nowUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static std::atomic<long> frameNo{0};

// ---------------------------------------------------------------- memory, system
extern "C" size_t sceKernelGetDirectMemorySize() { return 0x10000000; }
extern "C" int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *phys) {
  *phys = 0;
  return 0;
}
extern "C" int sceKernelMapDirectMemory(void **address, size_t length, int, int, int64_t, size_t) {
  *address = aligned_alloc(0x200000, length);
  memset(*address, 0, length);
  return *address ? 0 : -1;
}
extern "C" int sceKernelSendNotificationRequest(uint32_t, void *request, size_t, int) {
  fprintf(stderr, "NOTIFICATION: %s\n", (const char *)request + 45);
  return 0;
}
extern "C" int sceKernelUsleep(uint32_t us) { return usleep(us); }
extern "C" int sceSystemServiceHideSplashScreen() { return 0; }
extern "C" int sceUserServiceInitialize(const void *) { return 0; }
extern "C" int sceUserServiceGetInitialUser(int32_t *user) {
  *user = 1;
  return 0;
}

// ---------------------------------------------------------------- video
static void *buffers[2];
static std::vector<long> shots;
static long maxFrames = 0;

extern "C" int sceVideoOutOpen(int32_t, int32_t, int32_t, const void *) {
  if (const char *s = getenv("SIM_SHOTS")) {
    std::string all = s;
    size_t at = 0;
    while (at < all.size()) {
      shots.push_back(atol(all.c_str() + at));
      at = all.find(',', at);
      if (at == std::string::npos) break;
      at++;
    }
  }
  if (const char *f = getenv("SIM_FRAMES")) maxFrames = atol(f);
  return 1;
}
extern "C" int sceVideoOutSetFlipRate(int32_t, int32_t) { return 0; }
extern "C" void sceVideoOutSetBufferAttribute2(void *, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t) {}
extern "C" int sceVideoOutRegisterBuffers2(int32_t, int32_t, int32_t, void *bufs, int32_t count, void *, int32_t, void *) {
  void **b = (void **)bufs;
  for (int i = 0; i < count && i < 2; i++) buffers[i] = b[i * 4];
  return 0;
}

// the same layout as ps5_main.cpp's tiledOffset
static size_t tiledOffset(unsigned x, unsigned y) {
  const uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                          ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                          ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
  const uint32_t blockIndex = (y >> 7) * 15 + (x >> 7);
  return ((size_t)blockIndex << 16) + offset;
}

static void saveShot(int buffer, long frame) {
  char name[64];
  snprintf(name, sizeof(name), "shot-%05ld.ppm", frame);
  FILE *f = fopen(name, "wb");
  if (!f) return;
  fprintf(f, "P6\n1920 1080\n255\n");
  static uint8_t row[1920 * 3];
  const uint8_t *b = (const uint8_t *)buffers[buffer];
  for (unsigned y = 0; y < 1080; y++) {
    for (unsigned x = 0; x < 1920; x++) {
      uint32_t p = *(const uint32_t *)(b + tiledOffset(x, y));
      row[x * 3] = p & 0xff;
      row[x * 3 + 1] = (p >> 8) & 0xff;
      row[x * 3 + 2] = (p >> 16) & 0xff;
    }
    fwrite(row, 1, sizeof(row), f);
  }
  fclose(f);
  fprintf(stderr, "SIM: saved %s\n", name);
}

extern "C" int sceVideoOutSubmitFlip(int32_t, int32_t buffer_index, uint32_t, int64_t) {
  long f = frameNo.load();
  for (long s : shots)
    if (s == f) saveShot(buffer_index, f);
  return 0;
}

extern "C" int sceVideoOutWaitVblank(int32_t) {
  static uint64_t next = 0;
  uint64_t now = nowUs();
  if (next == 0) next = now;
  next += 16667;
  if (next > now) usleep((useconds_t)(next - now));
  long f = ++frameNo;
  if (maxFrames && f > maxFrames) {
    fprintf(stderr, "SIM: %ld frames, done\n", maxFrames);
    fflush(stderr);
    _exit(0);
  }
  return 0;
}

// ---------------------------------------------------------------- controller
struct PadStep {
  long from, to;
  uint32_t buttons;
};
struct TouchStep {
  long from, to;
  float x, y;
};
static std::vector<PadStep> padScript;
static std::vector<TouchStep> touchScript;

extern "C" int scePadInit() {
  if (const char *s = getenv("SIM_PAD")) {
    std::string all = s;
    size_t at = 0;
    while (at < all.size()) {
      PadStep p{};
      sscanf(all.c_str() + at, "%ld-%ld:%x", &p.from, &p.to, &p.buttons);
      padScript.push_back(p);
      at = all.find(';', at);
      if (at == std::string::npos) break;
      at++;
    }
  }
  if (const char *s = getenv("SIM_TOUCH")) {
    std::string all = s;
    size_t at = 0;
    while (at < all.size()) {
      TouchStep t{};
      sscanf(all.c_str() + at, "%ld-%ld:%f,%f", &t.from, &t.to, &t.x, &t.y);
      touchScript.push_back(t);
      at = all.find(';', at);
      if (at == std::string::npos) break;
      at++;
    }
  }
  return 0;
}
extern "C" int scePadOpen(int32_t, int32_t, int32_t, const void *) { return 1; }
extern "C" int scePadGetHandle(int32_t, int32_t, int32_t) { return 1; }
extern "C" int scePadRead(int32_t, void *data, int32_t) {
  uint8_t *s = (uint8_t *)data;
  memset(s, 0, 120);
  long f = frameNo.load();
  uint32_t b = 0;
  for (auto &p : padScript)
    if (f >= p.from && f <= p.to) b |= p.buttons;
  s[0] = b & 0xff;
  s[1] = (b >> 8) & 0xff;
  s[2] = (b >> 16) & 0xff;
  s[3] = (b >> 24) & 0xff;
  s[4] = s[5] = s[6] = s[7] = 128;
  for (auto &t : touchScript)
    if (f >= t.from && f <= t.to) {
      s[52] = 1;
      uint16_t x = (uint16_t)(t.x * 1919), y = (uint16_t)(t.y * 942);
      s[60] = x & 0xff;
      s[61] = x >> 8;
      s[62] = y & 0xff;
      s[63] = y >> 8;
    }
  return 1;
}

// ---------------------------------------------------------------- sound
static FILE *wav = nullptr;
static uint32_t wavBytes = 0;
static double sumSquares = 0;
static long sampleCount = 0;

static void wavHeader() {
  uint32_t h[11] = {0x46464952, 36 + wavBytes, 0x45564157, 0x20746d66, 16, 0x00020001, 48000, 48000 * 4, 0x00100004,
                    0x61746164, wavBytes};
  fseek(wav, 0, SEEK_SET);
  fwrite(h, 4, 11, wav);
  fseek(wav, 0, SEEK_END);
  fflush(wav);
}

extern "C" int sceAudioOutInit() { return 0; }
extern "C" int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned) {
  if (const char *f = getenv("SIM_WAV")) {
    wav = fopen(f, "wb");
    if (wav) wavHeader();
  }
  return 1;
}
extern "C" int sceAudioOutOutput(int, const void *samples) {
  static uint64_t next = 0;
  uint64_t now = nowUs();
  if (next == 0) next = now;
  next += 256 * 1000000ull / 48000;
  if (next > now) usleep((useconds_t)(next - now));
  const int16_t *s = (const int16_t *)samples;
  for (int i = 0; i < 512; i++) sumSquares += (double)s[i] * s[i];
  sampleCount += 512;
  if (wav) {
    fwrite(samples, 4, 256, wav);
    wavBytes += 1024;
    if ((wavBytes & 0xffff) == 0) wavHeader();
  }
  return 0;
}

__attribute__((destructor)) static void soundReport() {
  if (wav) wavHeader();
}
