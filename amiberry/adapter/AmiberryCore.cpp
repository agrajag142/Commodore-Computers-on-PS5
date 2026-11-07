// AmiberryCore.cpp
// The Amiga app's emulator interface (AmigaCore.h, and the few chip functions
// the front end calls from a500.h) on top of Amiberry, built as its libretro
// core (a static library, libamiberry.a). The front end stays as it is: the
// menu, the controller, the keyboards, the MOD player.
//
//   picture   Amiberry hands over each finished frame (XRGB8888); it is
//             converted into the front end's 640 x 512 RGB565 picture
//   sound     48 kHz stereo into a ring the front end's sound thread empties
//   input     the mouse (port 1) and joystick (port 2) as libretro devices;
//             the keyboard through Amiberry's keyboard callback
//   disks     the front end reads an .adf into memory; it is written to the
//             app's storage and put in DF0: through Amiberry's disk control
//   Kickstart the front end loads (and decrypts) it; it is written to the
//             system folder, where Amiberry looks for kick.rom
//
// Everything that touches Amiberry runs on the emulator thread (amigaRunFrame):
// requests from the screen thread (keys, disks, reset) wait in queues.

#include "AmigaCore.h"
#include "chips/a500.h"
#include "libretro.h"

#include <atomic>
#include <ctype.h>
#include <utility>
#include <vector>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>

int64_t (*amigaMicros)() = nullptr;
extern "C" int ps5portDiagnostics(void (*log)(const char *)); // (ps5port/diagnostics.cpp)
extern "C" void ps5portWatch(void);
extern "C" void ps5portSelfTest(void);
extern "C" void ps5portPoke(pthread_t t);
extern "C" void ps5portTrace(int on); // (functions entered, into the log: frame 2, where the PS5 build dies)

// ------------------------------------------------------------ where files go
static const char *kSystemDir = "/download0/amiberry"; // the app's own storage (writable)

static void mkdirs() {
  mkdir("/download0", 0777);
  mkdir(kSystemDir, 0777);
}

static bool writeFile(const std::string &path, const uint8_t *data, size_t n) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  fclose(f);
  return ok;
}

// ------------------------------------------------------------ the log
// (written with write(), no stdio lock: a thread stuck anywhere can't stop
//  another from logging)
static int logFd = -2;
static void logWrite(const char *s, size_t n) {
  if (logFd == -2) {
    // beside the app, where FTP reaches it; else the app's own storage
    logFd = open("/data/homebrew/PPSA99906/amiberry.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (logFd < 0) {
      mkdirs();
      logFd = open("/download0/amiberry/amiberry.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
  }
  if (logFd >= 0 && n) write(logFd, s, n);
}
static void logV(const char *fmt, va_list a) {
  char line[2048];
  int n = vsnprintf(line, sizeof line, fmt, a);
  if (n < 0) return;
  logWrite(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}
static void logLine(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  logV(fmt, a);
  va_end(a);
}
static void RETRO_CALLCONV coreLog(enum retro_log_level level, const char *fmt, ...) {
  if (level < RETRO_LOG_INFO) return;
  va_list a;
  va_start(a, fmt);
  logV(fmt, a);
  va_end(a);
}

static int traceFrames = 0; // (the first frames: every call from the core, into the log)

// ------------------------------------------------------------ the picture
static AmigaOutput out{};
static std::atomic<bool> showPicture{true};

static void RETRO_CALLCONV videoRefresh(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (traceFrames < 4) logLine("PS5: video %p %ux%u\n", data, w, h);
  if (!data || !out.fb || !showPicture.load(std::memory_order_relaxed)) return;
  // Into 640 x 512: a low-resolution picture (320 wide) is doubled across,
  // a single-line one (up to 300 high) down; anything bigger is cropped round
  // the middle.
  int sx = w <= 400 ? 2 : 1, sy = h <= 300 ? 2 : 1;
  int fw = (int)w * sx, fh = (int)h * sy;
  // (the Amiga's standard screen sits right of the middle of Amiberry's
  // overscan picture: Workbench 1.3, 640 wide, was 20 pixels right on the PS5)
  const int kShiftX = 20;
  int cropX = fw > 640 ? (fw - 640) / 2 + kShiftX : 0, cropY = fh > 512 ? (fh - 512) / 2 : 0;
  if (cropX > fw - 640) cropX = fw > 640 ? fw - 640 : 0;
  int padX = fw < 640 ? (640 - fw) / 2 : 0, padY = fh < 512 ? (512 - fh) / 2 : 0;
  for (int y = 0; y < 512; y++) {
    uint16_t *dst = out.fb + (out.y + y) * out.stride + out.x;
    int fy = y - padY + cropY;
    if (fy < 0 || fy >= fh) {
      memset(dst, 0, 640 * 2);
      continue;
    }
    const uint32_t *src = (const uint32_t *)((const uint8_t *)data + (size_t)(fy / sy) * pitch);
    for (int x = 0; x < 640; x++) {
      int fx = x - padX + cropX;
      if (fx < 0 || fx >= fw) {
        dst[x] = 0;
        continue;
      }
      uint32_t p = src[fx / sx];
      dst[x] = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
    }
  }
  static unsigned lastW, lastH;
  if (w != lastW || h != lastH) logLine("PS5: picture %ux%u (pitch %zu)\n", w, h, pitch), lastW = w, lastH = h;
}

// ------------------------------------------------------------ the sound
static const int kRing = 16384; // stereo frames
static int16_t ring[kRing * 2];
static std::atomic<unsigned> ringHead{0}, ringTail{0};

static size_t RETRO_CALLCONV audioBatch(const int16_t *data, size_t frames) {
  if (traceFrames < 4) logLine("PS5: audio %zu\n", frames);
  unsigned h = ringHead.load(std::memory_order_relaxed);
  unsigned t = ringTail.load(std::memory_order_acquire);
  for (size_t i = 0; i < frames; i++) {
    if (h - t >= (unsigned)kRing) break; // full: dropped
    ring[(h % kRing) * 2] = data[i * 2];
    ring[(h % kRing) * 2 + 1] = data[i * 2 + 1];
    h++;
  }
  ringHead.store(h, std::memory_order_release);
  return frames;
}
static void RETRO_CALLCONV audioSample(int16_t l, int16_t r) {
  int16_t s[2] = {l, r};
  audioBatch(s, 1);
}

int paula_ring_avail(void) {
  return (int)(ringHead.load(std::memory_order_acquire) - ringTail.load(std::memory_order_relaxed));
}
int paula_ring_pop(int16_t *l, int16_t *r) {
  unsigned t = ringTail.load(std::memory_order_relaxed);
  if (t == ringHead.load(std::memory_order_acquire)) {
    *l = *r = 0;
    return 0;
  }
  *l = ring[(t % kRing) * 2];
  *r = ring[(t % kRing) * 2 + 1];
  ringTail.store(t + 1, std::memory_order_release);
  return 1;
}

// ------------------------------------------------------------ input
static std::atomic<int> mouseDX{0}, mouseDY{0};
static std::atomic<int> lmb{0}, rmb{0};
static std::atomic<unsigned> joyBits{0}; // libretro joypad bits

void input_mouse_move(int dx, int dy) {
  mouseDX.fetch_add(dx, std::memory_order_relaxed);
  mouseDY.fetch_add(dy, std::memory_order_relaxed);
}
void input_set_lmb(int v) { lmb.store(v ? 1 : 0, std::memory_order_relaxed); }
void input_set_rmb(int v) { rmb.store(v ? 1 : 0, std::memory_order_relaxed); }
int input_lmb(void) { return lmb.load(std::memory_order_relaxed); }
int input_rmb(void) { return rmb.load(std::memory_order_relaxed); }
void input_set_joy(int up, int down, int left, int right, int fire) {
  unsigned b = 0;
  if (up) b |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
  if (down) b |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (left) b |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (right) b |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  if (fire) b |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
  joyBits.store(b, std::memory_order_relaxed);
}
int input_joy_fire(void) { return (joyBits.load() >> RETRO_DEVICE_ID_JOYPAD_B) & 1; }

// this frame's mouse movement (taken once per frame, before retro_run)
static int frameDX, frameDY;

static void RETRO_CALLCONV inputPoll(void) {}
static int16_t RETRO_CALLCONV inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (traceFrames < 4) logLine("PS5: input %u %u %u\n", port, device, id);
  if (device == RETRO_DEVICE_MOUSE && port == 0) {
    switch (id) {
    case RETRO_DEVICE_ID_MOUSE_X: return (int16_t)frameDX;
    case RETRO_DEVICE_ID_MOUSE_Y: return (int16_t)frameDY;
    case RETRO_DEVICE_ID_MOUSE_LEFT: return (int16_t)lmb.load(std::memory_order_relaxed);
    case RETRO_DEVICE_ID_MOUSE_RIGHT: return (int16_t)rmb.load(std::memory_order_relaxed);
    }
    return 0;
  }
  // (Amiberry reads the joystick in the Amiga's port 2 from libretro pad 0,
  // "Libretro Joystick 1"; the mouse from port 0's mouse)
  if (device == RETRO_DEVICE_JOYPAD && port == 0) {
    unsigned b = joyBits.load(std::memory_order_relaxed);
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)b;
    return (int16_t)((b >> id) & 1);
  }
  return 0;
}

// ---- the keyboard: Amiga raw key codes (from the front end) -> RETROK
static retro_keyboard_event_t coreKeyboard = nullptr;
static unsigned rawToRetrok(uint8_t raw) {
  switch (raw) {
  case 0x00: return RETROK_BACKQUOTE;
  case 0x01: return RETROK_1; case 0x02: return RETROK_2; case 0x03: return RETROK_3; case 0x04: return RETROK_4;
  case 0x05: return RETROK_5; case 0x06: return RETROK_6; case 0x07: return RETROK_7; case 0x08: return RETROK_8;
  case 0x09: return RETROK_9; case 0x0a: return RETROK_0;
  case 0x0b: return RETROK_MINUS; case 0x0c: return RETROK_EQUALS; case 0x0d: return RETROK_BACKSLASH;
  case 0x0f: return RETROK_KP0;
  case 0x10: return RETROK_q; case 0x11: return RETROK_w; case 0x12: return RETROK_e; case 0x13: return RETROK_r;
  case 0x14: return RETROK_t; case 0x15: return RETROK_y; case 0x16: return RETROK_u; case 0x17: return RETROK_i;
  case 0x18: return RETROK_o; case 0x19: return RETROK_p;
  case 0x1a: return RETROK_LEFTBRACKET; case 0x1b: return RETROK_RIGHTBRACKET;
  case 0x1d: return RETROK_KP1; case 0x1e: return RETROK_KP2; case 0x1f: return RETROK_KP3;
  case 0x20: return RETROK_a; case 0x21: return RETROK_s; case 0x22: return RETROK_d; case 0x23: return RETROK_f;
  case 0x24: return RETROK_g; case 0x25: return RETROK_h; case 0x26: return RETROK_j; case 0x27: return RETROK_k;
  case 0x28: return RETROK_l; case 0x29: return RETROK_SEMICOLON; case 0x2a: return RETROK_QUOTE;
  case 0x2d: return RETROK_KP4; case 0x2e: return RETROK_KP5; case 0x2f: return RETROK_KP6;
  case 0x30: return RETROK_LESS;
  case 0x31: return RETROK_z; case 0x32: return RETROK_x; case 0x33: return RETROK_c; case 0x34: return RETROK_v;
  case 0x35: return RETROK_b; case 0x36: return RETROK_n; case 0x37: return RETROK_m;
  case 0x38: return RETROK_COMMA; case 0x39: return RETROK_PERIOD; case 0x3a: return RETROK_SLASH;
  case 0x3c: return RETROK_KP_PERIOD; case 0x3d: return RETROK_KP7; case 0x3e: return RETROK_KP8; case 0x3f: return RETROK_KP9;
  case 0x40: return RETROK_SPACE; case 0x41: return RETROK_BACKSPACE; case 0x42: return RETROK_TAB;
  case 0x43: return RETROK_KP_ENTER; case 0x44: return RETROK_RETURN; case 0x45: return RETROK_ESCAPE;
  case 0x46: return RETROK_DELETE; case 0x4a: return RETROK_KP_MINUS;
  case 0x4c: return RETROK_UP; case 0x4d: return RETROK_DOWN; case 0x4e: return RETROK_RIGHT; case 0x4f: return RETROK_LEFT;
  case 0x50: return RETROK_F1; case 0x51: return RETROK_F2; case 0x52: return RETROK_F3; case 0x53: return RETROK_F4;
  case 0x54: return RETROK_F5; case 0x55: return RETROK_F6; case 0x56: return RETROK_F7; case 0x57: return RETROK_F8;
  case 0x58: return RETROK_F9; case 0x59: return RETROK_F10;
  case 0x5a: return RETROK_KP_EQUALS; case 0x5b: return RETROK_KP_DIVIDE; // (the keypad's ( ) as Amiberry maps them)
  case 0x5c: return RETROK_KP_DIVIDE; case 0x5d: return RETROK_KP_MULTIPLY; case 0x5e: return RETROK_KP_PLUS;
  case 0x5f: return RETROK_PAGEDOWN; // HELP (Amiberry's: Page Down; Page Up is its freeze button)
  case 0x60: return RETROK_LSHIFT; case 0x61: return RETROK_RSHIFT; case 0x62: return RETROK_CAPSLOCK;
  case 0x63: return RETROK_LCTRL; case 0x64: return RETROK_LALT; case 0x65: return RETROK_RALT;
  case 0x66: return RETROK_LSUPER; case 0x67: return RETROK_RSUPER;
  }
  return RETROK_UNKNOWN;
}

static uint8_t keyQueue[256];
static std::atomic<unsigned> keyHead{0}, keyTail{0};
void kbd_key(uint8_t rawkey, int up) {
  unsigned h = keyHead.load(std::memory_order_relaxed);
  if (h - keyTail.load(std::memory_order_acquire) >= sizeof keyQueue) return;
  keyQueue[h % sizeof keyQueue] = (uint8_t)((rawkey & 0x7f) | (up ? 0x80 : 0));
  keyHead.store(h + 1, std::memory_order_release);
}
void cia_kbd_vsync(void) {}

// ------------------------------------------------------------ the drive light
static std::atomic<int> driveLight{0};
static std::atomic<int> hdLightOn{0}, hdLightUntil{0}, hdLightFrame{0};
static void RETRO_CALLCONV setLed(int led, int state) {
  // (Amiberry's: 0 power, 1 any drive, 2 hard drive/CD, 3 DF0:)
  static int logged;
  if (logged < 40) {
    logged++;
    logLine("PS5: LED %d %d\n", led, state);
  }
  if (led == 3) driveLight.store(state, std::memory_order_relaxed);
  if (led == 7) { // (the hard drive: on for a moment at a time - held a little, to be seen)
    if (state) hdLightUntil.store(hdLightFrame.load() + 6, std::memory_order_relaxed);
    hdLightOn.store(state, std::memory_order_relaxed);
  }
}
int drive_led(void) { return driveLight.load(std::memory_order_relaxed); }
int hd_led(void) {
  return hdLightOn.load(std::memory_order_relaxed) || hdLightFrame.load() < hdLightUntil.load(std::memory_order_relaxed);
}

// ------------------------------------------------------------ the environment
static retro_disk_control_ext_callback diskExt{};
static retro_disk_control_callback diskCb{};
static bool haveDiskExt = false, haveDiskCb = false;

// ------------------------------------------------------------ the models
// Each with the Kickstart it needs, written into Amiberry's folder under
// the name given here (from the front end's ROMs: amigaAddKickstart)
struct Model {
  const char *name, *kickName, *file;
  bool have;
  const char *option; // (Amiberry's name for it; null: the same)
};
static Model models[] = {
    {"A500", "1.3", "kick.rom", false, nullptr},
    {"A500+", "2.04", "kick204.rom", false, nullptr},
    {"A600", "2.05", "kick205.rom", false, nullptr},
    {"A1200", "3.1", "kick31.rom", false, nullptr},
    {"A4000", "3.1", "kick31a4000.rom", false, "A4040"}, // (the 68040 one)
};
static const int kModels = sizeof(models) / sizeof(models[0]);
static std::atomic<int> modelNow{0}, modelRequest{-1};

static uint32_t crc32of(const uint8_t *d, size_t n) {
  uint32_t c = 0xffffffff;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & (0 - (c & 1)));
  }
  return ~c;
}

void amigaAddKickstart(const char *name, const uint8_t *data, size_t len) {
  std::vector<uint8_t> rom(data, data + len);
  if (len == 524288 && !memcmp(rom.data(), rom.data() + 262144, 262144)) rom.resize(len = 262144);
  if (len != 262144 && len != 524288) {
    logLine("PS5: %s: not a Kickstart (%zu bytes)\n", name, len);
    return;
  }
  if (rom[0] != 0x11 && rom[1] == 0x11) // (a byte-swapped dump)
    for (size_t i = 0; i < len; i += 2) std::swap(rom[i], rom[i + 1]);
  // (a Kickstart starts 1111 4EF9 (512K) or 1114 4EF9 (256K))
  if (!(rom[0] == 0x11 && (rom[1] == 0x11 || rom[1] == 0x14) && rom[2] == 0x4e && rom[3] == 0xf9)) {
    logLine("PS5: %s: not a Kickstart\n", name);
    return;
  }
  int ver = rom[12] << 8 | rom[13], rev = rom[14] << 8 | rom[15];
  uint32_t crc = crc32of(rom.data(), len);
  std::string lower = name;
  for (auto &c : lower) c = (char)tolower((unsigned char)c);
  int m = -1;
  if (ver == 34 || ver == 33) m = 0;
  else if (len != 524288) m = -1; // (2.0 and later: 512K)
  else if (ver == 37) m = rev <= 210 ? 1 : 2; // (37.175 the A500+'s 2.04; 37.299-37.350 the A600's 2.05)
  else if (ver >= 39) {
    bool a4000 = crc == 0xD6BAE334 || crc == 0x9E6AC152 || lower.find("4000") != std::string::npos;
    bool a1200 = crc == 0x1483A091 || crc == 0x6C9B07D2 || lower.find("1200") != std::string::npos;
    if (a1200) m = 3;
    else if (a4000) m = 4;
    else if (!models[2].have) m = 2; // (3.x for the A500/A600/A2000: works in an A600)
  }
  if (m < 0) {
    logLine("PS5: %s: Kickstart %d.%d (CRC %08X), not used\n", name, ver, rev, (unsigned)crc);
    return;
  }
  mkdirs();
  if (!writeFile(std::string(kSystemDir) + "/" + models[m].file, rom.data(), len)) return;
  models[m].have = true;
  logLine("PS5: %s: Kickstart %d.%d (CRC %08X) for the %s\n", name, ver, rev, (unsigned)crc, models[m].name);
}

int amigaModelCount() { return kModels; }
const char *amigaModelName(int i) { return i >= 0 && i < kModels ? models[i].name : ""; }
const char *amigaModelKickstart(int i) { return i >= 0 && i < kModels ? models[i].kickName : ""; }
bool amigaModelAvailable(int i) { return i >= 0 && i < kModels && models[i].have; }
int amigaModel() { return modelNow.load(); }
void amigaSetModel(int i) {
  if (amigaModelAvailable(i)) modelRequest.store(i);
}
bool amigaModelChanging() { return modelRequest.load() >= 0; }

// ------------------------------------------------------------ the hard drive
// An .hdf Amiberry boots from as DH0: (its "game"); "" none. The app's
// assets as /app0 are read-only: the same file in the app's folder under
// /data/homebrew, where the Amiga can write to it, if it is there.
static std::string hdNow;
static std::string hdRequestPath;
static std::atomic<bool> hdRequest{false};
static std::string writablePath(const std::string &p) {
  static const char *app0 = "/app0/";
  if (p.compare(0, strlen(app0), app0)) return p;
  std::string alt = "/data/homebrew/PPSA99906/" + p.substr(strlen(app0));
  struct stat st;
  return stat(alt.c_str(), &st) == 0 ? alt : p;
}
void amigaSetHardDrive(const char *path) {
  if (hdRequest.load()) return;
  hdRequestPath = path ? writablePath(path) : "";
  hdRequest.store(true);
}
const char *amigaHardDrive() { return hdNow.c_str(); }

static void saveModel() {
  std::string m = models[modelNow.load()].name;
  writeFile(std::string(kSystemDir) + "/model.txt", (const uint8_t *)m.data(), m.size());
}
static void saveHardDrive() {
  writeFile(std::string(kSystemDir) + "/harddrive.txt", (const uint8_t *)hdNow.data(), hdNow.size());
}
static void loadHardDrive(const char *defaultPath) {
  FILE *f = fopen((std::string(kSystemDir) + "/harddrive.txt").c_str(), "r");
  if (!f) { // (none chosen yet: default.hdf, if the app has one)
    if (defaultPath && defaultPath[0]) hdNow = writablePath(defaultPath);
    f = nullptr;
  }
  if (!f) {
    struct stat st;
    std::string trying = std::string(kSystemDir) + "/harddrive.try";
    if (!hdNow.empty() && stat(trying.c_str(), &st) == 0) {
      logLine("PS5: %s took the app down last time: left out\n", hdNow.c_str());
      hdNow.clear();
      saveHardDrive();
      unlink(trying.c_str());
    }
    return;
  }
  char line[512] = {};
  fgets(line, sizeof line, f);
  fclose(f);
  struct stat st;
  if (line[0] && stat(line, &st) == 0) hdNow = line;
  // (the app went down last time with it in, before it had run 10 seconds:
  // started without it, so it doesn't go down again)
  std::string trying = std::string(kSystemDir) + "/harddrive.try";
  if (!hdNow.empty() && stat(trying.c_str(), &st) == 0) {
    logLine("PS5: %s took the app down last time: left out\n", hdNow.c_str());
    hdNow.clear();
    saveHardDrive();
    unlink(trying.c_str());
  }
}
static int hdFrames = -1; // (frames run with the hard drive in; -1 none)

static void loadModel() {
  FILE *f = fopen((std::string(kSystemDir) + "/model.txt").c_str(), "r");
  if (!f) return;
  char line[32] = {};
  fgets(line, sizeof line, f);
  fclose(f);
  for (int i = 0; i < kModels; i++)
    if (!strncmp(line, models[i].name, strlen(models[i].name)) && strlen(line) == strlen(models[i].name) && models[i].have)
      modelNow.store(i);
}

static const char *optionValue(const char *key) {
  if (!strcmp(key, "amiberry_model")) {
    const Model &md = models[modelNow.load()];
    return md.option ? md.option : md.name;
  }
  if (!strcmp(key, "amiberry_kickstart")) return models[modelNow.load()].file;
  if (!strcmp(key, "amiberry_audio_rate")) return "48000";
  if (!strcmp(key, "amiberry_port0_device")) return "mouse";
  if (!strcmp(key, "amiberry_port1_device")) return "joystick";
  if (!strcmp(key, "amiberry_statusline")) return "disabled";
  if (!strcmp(key, "amiberry_on_screen_keyboard")) return "disabled";
  if (!strcmp(key, "amiberry_crop_overscan")) return "disabled";
  if (!strcmp(key, "amiberry_stereo_separation")) return "7";
  return nullptr; // (Amiberry's default)
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data) {
  if (traceFrames < 4) logLine("PS5: env %u\n", cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL);
  switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
    *(const char **)data = kSystemDir;
    return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((retro_log_callback *)data)->log = coreLog;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    return *(const retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    retro_variable *v = (retro_variable *)data;
    v->value = v->key ? optionValue(v->key) : nullptr;
    return v->value != nullptr;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool *)data = false;
    return true;
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    *(unsigned *)data = 0; // (v1 variables: our answers above)
    return true;
  case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
    coreKeyboard = ((const retro_keyboard_callback *)data)->callback;
    return true;
  case RETRO_ENVIRONMENT_GET_LED_INTERFACE & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
    ((retro_led_interface *)data)->set_led_state = setLed;
    return true;
  case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    diskExt = *(const retro_disk_control_ext_callback *)data;
    haveDiskExt = true;
    return true;
  case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    diskCb = *(const retro_disk_control_callback *)data;
    haveDiskCb = true;
    return true;
  case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
    *(unsigned *)data = 1;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
    return true;
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
  case RETRO_ENVIRONMENT_SET_MESSAGE:
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
    return true;
  }
  return false;
}

// ------------------------------------------------------------ disks and the Kickstart
static std::string pendingDiskPath;   // a disk written out, waiting for the emulator thread
static std::atomic<int> diskRequest{0}; // 1 swap, 2 boot
static std::atomic<bool> resetRequest{false};
static uint8_t adfBuffer[901120];
static uint8_t *kick = nullptr;
static bool started = false, loaded = false;
static std::string firstDisk;
static std::atomic<bool> paused{false};

uint8_t *drive_alloc_adf(void) { return adfBuffer; }
void drive_mount_ready(void) {
  // the disk in DF0: at start-up
  mkdirs();
  firstDisk = std::string(kSystemDir) + "/df0.adf";
  if (!writeFile(firstDisk, adfBuffer, sizeof adfBuffer)) firstDisk.clear();
}

void amigaRequestDisk(const uint8_t *adf, bool boot) {
  // (a new file each time: the core keeps the old one open until it lets go)
  static int n = 0;
  char path[96];
  snprintf(path, sizeof(path), "%s/disk%d.adf", kSystemDir, n++ & 7);
  if (!writeFile(path, adf, 901120)) return;
  pendingDiskPath = path;
  diskRequest.store(boot ? 2 : 1, std::memory_order_release);
}
bool amigaDiskBusy() { return diskRequest.load(std::memory_order_acquire) != 0; }
void amigaRequestReset() { resetRequest.store(true, std::memory_order_release); }
void amigaReset() { amigaRequestReset(); }
void amigaMarkAllDirty() {}
void amigaShowPicture(bool on) { showPicture.store(on); }
void amigaPause(bool on) { paused.store(on); }
bool amigaPaused() { return paused.load(); }
void amigaUseDrawingCore(void (*)(), void (*)()) {}
void amigaUseBlitterCore(void (*)()) {}
void amigaRunBlitter() {}
int amigaDrawPending() { return 0; }

bool amigaCoreInit(uint8_t *chip, uint8_t *slow, uint8_t *rom, const AmigaOutput &o) {
  (void)chip;
  (void)slow;
  out = o;
  kick = rom;
  logLine("PS5: the front end is up (Kickstart %s), build " __DATE__ " " __TIME__ "\n", rom ? "loaded" : "missing");
  mkdirs();
  // the Kickstart as the front end loaded it (Amiga Forever's already decrypted)
  if (kick && !writeFile(std::string(kSystemDir) + "/kick.rom", kick, 262144)) {
    logLine("PS5: could not write kick.rom (%d)\n", errno);
    return false;
  }
  if (kick) models[0].have = true;
  return true;
}

static void insertDisk(const std::string &path) {
  retro_game_info info{};
  info.path = path.c_str();
  if (haveDiskExt) {
    bool e = diskExt.set_eject_state(true);
    unsigned n = diskExt.get_num_images();
    bool a = diskExt.add_image_index();
    bool r = diskExt.replace_image_index(n, &info);
    bool i = diskExt.set_image_index(n);
    bool c = diskExt.set_eject_state(false);
    logLine("PS5: disk %s in as image %u (eject %d add %d replace %d index %d close %d, now %u)\n", path.c_str(), n, e,
            a, r, i, c, diskExt.get_image_index());
  } else if (haveDiskCb) {
    diskCb.set_eject_state(true);
    unsigned n = diskCb.get_num_images();
    diskCb.add_image_index();
    diskCb.replace_image_index(n, &info);
    diskCb.set_image_index(n);
    diskCb.set_eject_state(false);
  }
}

// The watchdog: a frame that takes over 5 seconds - where the emulator
// thread is, every 5 seconds (up to 6 times)
static pthread_t emuThread;
static std::atomic<int> framesDone{0};
static void *watchdog(void *) {
  logLine("PS5: watchdog running\n");
  int last = -1, pokes = 0;
  while (pokes < 6) {
    sleep(5);
    int f = framesDone.load();
    logLine("PS5: watchdog: %d frames\n", f);
    if (f == last && !paused.load()) {
      logLine("PS5: no frame for 5 seconds (at frame %d)\n", f);
      ps5portPoke(emuThread);
      pokes++;
    }
    last = f;
  }
  return nullptr;
}

static std::string diskNow; // (the disk in DF0: now, for a restart)
static void coreStart() {
  logLine("PS5: retro_init (%s, Kickstart %s)...\n", models[modelNow.load()].name, models[modelNow.load()].file);
  retro_init();
  logLine("PS5: retro_init done\n");
  retro_set_controller_port_device(0, RETRO_DEVICE_MOUSE);
  retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
  if (diskNow.empty()) diskNow = firstDisk;
  if (!hdNow.empty()) {
    writeFile(std::string(kSystemDir) + "/harddrive.try", (const uint8_t *)"1", 1);
    hdFrames = 0;
  }
  // (a hard drive: Amiberry's "game", DH0:, booted from; else the floppy)
  const std::string &content = hdNow.empty() ? diskNow : hdNow;
  retro_game_info info{};
  info.path = content.empty() ? nullptr : content.c_str();
  logLine("PS5: retro_load_game...\n");
  loaded = retro_load_game(content.empty() ? nullptr : &info);
  logLine("PS5: retro_load_game(%s): %s\n", content.empty() ? "no disk" : content.c_str(), loaded ? "ok" : "FAILED");
}

// The emulator thread: one PAL frame
void amigaRunFrame(AmigaFrameTimes *times) {
  int64_t t0 = amigaMicros ? amigaMicros() : 0;
  if (!started) {
    started = true;
    retro_set_environment(environment);
    retro_set_video_refresh(videoRefresh);
    retro_set_audio_sample(audioSample);
    retro_set_audio_sample_batch(audioBatch);
    retro_set_input_poll(inputPoll);
    retro_set_input_state(inputState);
    ps5portDiagnostics([](const char *s) { logLine("%s", s); });
    // a crash or a hang in the emulator: where, into the log
    ps5portWatch();
    ps5portSelfTest();
    emuThread = pthread_self();
    pthread_t w;
    pthread_create(&w, nullptr, watchdog, nullptr);
    // Amiberry's data folder (it looks for one before anything else)
    setenv("AMIBERRY_DATA_DIR", kSystemDir, 1);
    loadModel();
    for (int i = 0; i < kModels && !models[modelNow.load()].have; i++)
      if (models[i].have) modelNow.store(i);
    coreStart();
  }
  // Another model or hard drive: Amiberry stopped and started again (its
  // settings for the machine are read at the start), with the disk that was in
  int m = modelRequest.exchange(-1);
  bool hd = hdRequest.load();
  if (m >= 0 || hd) {
    logLine("PS5: %s %s: Amiberry stops...\n", m >= 0 ? "model" : "hard drive",
            m >= 0 ? models[m].name : hdRequestPath.empty() ? "none" : hdRequestPath.c_str());
    if (loaded) {
      retro_unload_game();
      retro_deinit();
    }
    if (m >= 0) {
      modelNow.store(m);
      saveModel();
    }
    if (hd) {
      hdNow = hdRequestPath;
      saveHardDrive();
      hdRequest.store(false);
    }
    coreStart();
  }
  if (!loaded) return;
  // the screen thread's requests
  if (resetRequest.exchange(false)) {
    logLine("PS5: reset\n");
    retro_reset();
  }
  // A disk to boot: the reset first, the disk in a little later, as on a
  // real Amiga (in at the reset itself, Amiberry lost it: the hand)
  static int insertIn = 0;
  if (int d = diskRequest.load(std::memory_order_acquire)) {
    if (insertIn == 0) {
      if (d == 2) {
        logLine("PS5: reset for %s\n", pendingDiskPath.c_str());
        retro_reset();
        insertIn = 25;
      } else {
        insertIn = 1;
      }
    }
    if (--insertIn == 0) {
      insertDisk(pendingDiskPath);
      diskNow = pendingDiskPath;
      diskRequest.store(0, std::memory_order_release);
    }
  }
  for (unsigned t = keyTail.load(std::memory_order_relaxed); t != keyHead.load(std::memory_order_acquire); t++) {
    uint8_t k = keyQueue[t % sizeof keyQueue];
    unsigned code = rawToRetrok(k & 0x7f);
    if (coreKeyboard && code != RETROK_UNKNOWN) coreKeyboard(!(k & 0x80), code, 0, 0);
    keyTail.store(t + 1, std::memory_order_release);
  }
  hdLightFrame++;
  if (hdFrames >= 0 && ++hdFrames == 500) { // (10 seconds with it: it's fine)
    unlink((std::string(kSystemDir) + "/harddrive.try").c_str());
    hdFrames = -1;
  }
  static int frames;
  if (frames < 10 || frames % 500 == 0) logLine("PS5: frame %d\n", frames);
  frames++;
  frameDX = mouseDX.exchange(0, std::memory_order_relaxed);
  frameDY = mouseDY.exchange(0, std::memory_order_relaxed);
  if (traceFrames == 1) ps5portTrace(1);
  retro_run();
  if (traceFrames == 1) ps5portTrace(0);
  if (traceFrames < 4) logLine("PS5: retro_run returned\n");
  traceFrames++;
  framesDone++;
  if (times) {
    memset(times, 0, sizeof(*times));
    times->totalUs = times->cpuUs = (uint32_t)((amigaMicros ? amigaMicros() : 0) - t0);
  }
}

// At start-up, before the Amiga runs: the hard drive it boots from (the
// one chosen last time, else default.hdf); the front end then puts no
// floppy in if there is one
void amigaStartHardDrive(const char *defaultPath) {
  mkdirs();
  loadHardDrive(defaultPath);
  logLine("PS5: hard drive at start: %s\n", hdNow.empty() ? "none" : hdNow.c_str());
}
void amigaNoStartDisk() { firstDisk.clear(); }

