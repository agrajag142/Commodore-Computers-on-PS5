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
extern "C" void ps5portPoke(pthread_t t);

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

// ------------------------------------------------------------ the picture
static AmigaOutput out{};
static std::atomic<bool> showPicture{true};

static void RETRO_CALLCONV videoRefresh(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (!data || !out.fb || !showPicture.load(std::memory_order_relaxed)) return;
  // Into 640 x 512: a low-resolution picture (320 wide) is doubled across,
  // a single-line one (up to 300 high) down; anything bigger is cropped round
  // the middle.
  int sx = w <= 400 ? 2 : 1, sy = h <= 300 ? 2 : 1;
  int fw = (int)w * sx, fh = (int)h * sy;
  int cropX = fw > 640 ? (fw - 640) / 2 : 0, cropY = fh > 512 ? (fh - 512) / 2 : 0;
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
  if (device == RETRO_DEVICE_MOUSE && port == 0) {
    switch (id) {
    case RETRO_DEVICE_ID_MOUSE_X: return (int16_t)frameDX;
    case RETRO_DEVICE_ID_MOUSE_Y: return (int16_t)frameDY;
    case RETRO_DEVICE_ID_MOUSE_LEFT: return (int16_t)lmb.load(std::memory_order_relaxed);
    case RETRO_DEVICE_ID_MOUSE_RIGHT: return (int16_t)rmb.load(std::memory_order_relaxed);
    }
    return 0;
  }
  if (device == RETRO_DEVICE_JOYPAD && port == 1) {
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
  case 0x5f: return RETROK_PAGEUP; // HELP (Amiberry's default: Page Up / End)
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
static void RETRO_CALLCONV setLed(int led, int state) {
  if (led == 1) driveLight.store(state, std::memory_order_relaxed); // RETRO_LED_DRIVE0 (0 is power)
}
int drive_led(void) { return driveLight.load(std::memory_order_relaxed); }

// ------------------------------------------------------------ the environment
static retro_disk_control_ext_callback diskExt{};
static retro_disk_control_callback diskCb{};
static bool haveDiskExt = false, haveDiskCb = false;

static const char *optionValue(const char *key) {
  if (!strcmp(key, "amiberry_model")) return "A500";
  if (!strcmp(key, "amiberry_kickstart")) return "kick.rom";
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
  case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
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
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    return true;
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
  case RETRO_ENVIRONMENT_SET_MESSAGE:
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
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
  logLine("PS5: the front end is up (Kickstart %s)\n", rom ? "loaded" : "missing");
  mkdirs();
  // the Kickstart as the front end loaded it (Amiga Forever's already decrypted)
  if (kick && !writeFile(std::string(kSystemDir) + "/kick.rom", kick, 262144)) {
    logLine("PS5: could not write kick.rom (%d)\n", errno);
    return false;
  }
  return true;
}

static void insertDisk(const std::string &path) {
  retro_game_info info{};
  info.path = path.c_str();
  if (haveDiskExt) {
    diskExt.set_eject_state(true);
    unsigned n = diskExt.get_num_images();
    diskExt.add_image_index();
    diskExt.replace_image_index(n, &info);
    diskExt.set_image_index(n);
    diskExt.set_eject_state(false);
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
    emuThread = pthread_self();
    pthread_t w;
    pthread_create(&w, nullptr, watchdog, nullptr);
    // Amiberry's data folder (it looks for one before anything else)
    setenv("AMIBERRY_DATA_DIR", kSystemDir, 1);
    logLine("PS5: retro_init...\n");
    retro_init();
    logLine("PS5: retro_init done\n");
    retro_set_controller_port_device(0, RETRO_DEVICE_MOUSE);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    retro_game_info info{};
    info.path = firstDisk.empty() ? nullptr : firstDisk.c_str();
    logLine("PS5: retro_load_game...\n");
    loaded = retro_load_game(firstDisk.empty() ? nullptr : &info);
    logLine("PS5: retro_load_game(%s): %s\n", firstDisk.empty() ? "no disk" : firstDisk.c_str(), loaded ? "ok" : "FAILED");
  }
  if (!loaded) return;
  // the screen thread's requests
  if (resetRequest.exchange(false)) retro_reset();
  if (int d = diskRequest.load(std::memory_order_acquire)) {
    insertDisk(pendingDiskPath);
    if (d == 2) retro_reset();
    diskRequest.store(0, std::memory_order_release);
  }
  for (unsigned t = keyTail.load(std::memory_order_relaxed); t != keyHead.load(std::memory_order_acquire); t++) {
    uint8_t k = keyQueue[t % sizeof keyQueue];
    unsigned code = rawToRetrok(k & 0x7f);
    if (coreKeyboard && code != RETROK_UNKNOWN) coreKeyboard(!(k & 0x80), code, 0, 0);
    keyTail.store(t + 1, std::memory_order_release);
  }
  static int frames;
  if (frames < 10 || frames % 500 == 0) logLine("PS5: frame %d\n", frames);
  frames++;
  frameDX = mouseDX.exchange(0, std::memory_order_relaxed);
  frameDY = mouseDY.exchange(0, std::memory_order_relaxed);
  retro_run();
  framesDone++;
  if (times) {
    memset(times, 0, sizeof(*times));
    times->totalUs = times->cpuUs = (uint32_t)((amigaMicros ? amigaMicros() : 0) - t0);
  }
}
