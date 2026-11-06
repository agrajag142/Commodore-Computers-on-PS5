// KeyboardProbe.cpp - see KeyboardProbe.h

#include "KeyboardProbe.h"
#include "ps5_sce.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef PS5_SIM
#include <sys/ioctl.h>
#include <sys/types.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef KBD_DIRECT
// (the keyboard test build: imported from libSceKeyboard when the app starts)
extern "C" {
int sceKeyboardInit();
int sceKeyboardOpen(int32_t user, int32_t type, int32_t index, const void *param);
int sceKeyboardReadState(int32_t handle, void *data);
int sceKeyboardRead(int32_t handle, void *data, int32_t count);
}
#endif

namespace KeyboardProbe {

typedef int (*FnInit)();
typedef int (*FnOpen)(int32_t user, int32_t type, int32_t index, const void *param);
typedef int (*FnReadState)(int32_t handle, void *data);
typedef int (*FnRead)(int32_t handle, void *data, int32_t count);

static State S;
static bool started = false;
static FILE *logFile = nullptr;
static int logLines = 0;
static FnReadState readState = nullptr;
static FnRead readData = nullptr;
static int32_t handle = -1;
static const int kBuf = 16384; // far more than a read should write
static uint8_t buf[kBuf], buf2[kBuf];
static const uint8_t kFill = 0xa5;
static bool noisy[kStateBytes]; // changed while the keyboard was left alone (a clock, a counter)
static bool haveIdle = false;
static uint8_t prev[kStateBytes];

static void logLine(const char *fmt, ...) {
  if (!logFile || logLines > 4000) return;
  va_list a;
  va_start(a, fmt);
  vfprintf(logFile, fmt, a);
  va_end(a);
  fputc('\n', logFile);
  fflush(logFile);
  logLines++;
}

static void step(const char *fmt, ...) {
  char line[80];
  va_list a;
  va_start(a, fmt);
  vsnprintf(line, sizeof(line), fmt, a);
  va_end(a);
  if (S.stepCount < kLines) snprintf(S.steps[S.stepCount++], sizeof(S.steps[0]), "%s", line);
  logLine("%s", line);
}

static int written(const uint8_t *b) {
  int n = kBuf;
  while (n > 0 && b[n - 1] == kFill) n--;
  return n;
}

void start(int32_t initialUser) {
  if (started) return;
  started = true;
  memset(&S, 0, sizeof(S));
  memset(S.changedAt, 255, sizeof(S.changedAt));
  S.readStateRet = S.readRet = 0x7fffffff;

#ifdef KBD_DIRECT
#define KBD_TITLE "PPSA99906"
#else
#define KBD_TITLE "PPSA99904"
#endif
  static const char *logs[] = {"/data/homebrew/" KBD_TITLE "/kbdtest.txt", "/download0/kbdtest.txt"};
  for (const char *p : logs)
    if ((logFile = fopen(p, "w")) != nullptr) {
      snprintf(S.logPath, sizeof(S.logPath), "%s", p);
      break;
    }
  logLine("USB KEYBOARD TEST 2 (C64 for PS5, " KBD_TITLE "). Initial user %d (0x%x)", (int)initialUser,
          (unsigned)initialUser);

  // 0. Can the app see the USB devices themselves (/dev/ugenB.D, FreeBSD's
  // generic USB driver - how PS5-KBM-Bridge reads a keyboard)? Only looked at:
  // opened read-only, the device's IDs read, closed again.
  {
    int fd = open("/dev/null", O_RDONLY);
    step("/DEV/NULL: %s (%d)", fd >= 0 ? "OPENS" : "NOT VISIBLE", fd >= 0 ? 0 : errno);
    if (fd >= 0) close(fd);
    int opened = 0, tried = 0, shown = 0;
    int errs[8] = {}, errCount[8] = {}, kinds = 0;
    for (int bus = 0; bus <= 2; bus++)
      for (int dev = 1; dev <= 31; dev++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/ugen%d.%d", bus, dev);
        tried++;
        int f = open(path, O_RDONLY | O_NONBLOCK);
        if (f < 0) {
          int e = errno, k = 0;
          while (k < kinds && errs[k] != e) k++;
          if (k == kinds && kinds < 8) errs[kinds++] = e;
          if (k < 8) errCount[k]++;
          continue;
        }
        opened++;
        unsigned vid = 0, pid = 0;
#ifndef PS5_SIM
        struct usb_device_descriptor d;
        memset(&d, 0, sizeof(d));
        if (ioctl(f, USB_GET_DEVICE_DESC, &d) == 0) vid = UGETW(d.idVendor), pid = UGETW(d.idProduct);
#endif
        close(f);
        logLine("%s opened: %04x:%04x", path, vid, pid);
        if (shown++ < 4) step("%s: OPENS, DEVICE %04X:%04X", path, vid, pid);
      }
    char line[80];
    int n = snprintf(line, sizeof(line), "UGEN: %d OF %d OPEN.", opened, tried);
    for (int k = 0; k < kinds && n < 70; k++) n += snprintf(line + n, sizeof(line) - n, " ERR %d X%d", errs[k], errCount[k]);
    step("%s", line);
  }

  // 1. Can this app load a system library this way at all? Ones it already
  // uses (the controller, the sound) should load (or give their handle back).
  static const char *known[] = {"libScePad.sprx", "/system/common/lib/libScePad.sprx",
                                "/system/common/lib/libSceAudioOut.sprx"};
  for (const char *p : known) {
    logLine("about to load %s", p);
    int m = sceKernelLoadStartModule(p, 0, nullptr, 0, nullptr, nullptr);
    step("CHECK %s: %08X", p, (unsigned)m);
  }

  // 2. Is the keyboard library there (visible to the app), and does it load?
  static const char *paths[] = {"libSceKeyboard.sprx", "/system/common/lib/libSceKeyboard.sprx",
                                "/system/priv/lib/libSceKeyboard.sprx", "/system_ex/common_ex/lib/libSceKeyboard.sprx",
                                "/system/common/lib/libSceMouse.sprx"};
  int mod = -1;
  for (const char *p : paths) {
    if (p[0] == '/') {
      FILE *f = fopen(p, "rb");
      int e = errno;
      if (f) fclose(f);
      logLine("%s: %s (errno %d)", p, f ? "the file is there" : "cannot open the file", f ? 0 : e);
      if (!f) {
        step("%s: NO FILE (%d)", p, e);
        continue;
      }
    }
    logLine("about to load %s", p);
    int m = sceKernelLoadStartModule(p, 0, nullptr, 0, nullptr, nullptr);
    step("LOAD %s: %08X", p, (unsigned)m);
    if (m >= 0 && strstr(p, "Keyboard") && mod < 0) mod = m;
  }

  // 3. The system's own module loader, by name (the keyboard library, and the
  // IME library, which gave PS4 games the keyboard's keys)
  if (mod < 0) {
    static const char *names[] = {"libSceKeyboard", "libSceKeyboard.sprx", "libSceIme", "libSceIme.sprx"};
    int (*volatile byName)(const char *, uint32_t, uint64_t, uint64_t, int *) = sceSysmoduleLoadModuleByNameInternal;
    if (!byName) step("NO SYSMODULE LOADER (NOT IMPORTED)");
    for (const char *n : names) {
      if (!byName) break;
      int result = 0;
      logLine("about to call sceSysmoduleLoadModuleByNameInternal(\"%s\")", n);
      int r = byName(n, 0, 0, 0, &result);
      step("SYSMODULE %s: %08X", n, (unsigned)r);
    }
    static const char *loaded[] = {"libSceKeyboard.sprx", "libSceIme.sprx"};
    for (const char *n : loaded) {
      int m = sceKernelLoadStartModule(n, 0, nullptr, 0, nullptr, nullptr);
      step("NOW LOADED? %s: %08X", n, (unsigned)m);
      if (m >= 0 && strstr(n, "Keyboard")) mod = m;
      if (m >= 0 && strstr(n, "Ime")) {
        void *a = nullptr, *b = nullptr;
        sceKernelDlsym(m, "sceImeKeyboardOpen", &a);
        sceKernelDlsym(m, "sceImeUpdate", &b);
        step("IME: KEYBOARDOPEN %s UPDATE %s", a ? "Y" : "N", b ? "Y" : "N");
      }
    }
  }

  // (volatile: the imports' addresses are read at run time - the PS5 leaves
  // one it couldn't find at 0, which the compiler would otherwise assume can't happen)
  void *volatile fInit = nullptr, *volatile fOpen = nullptr, *volatile fState = nullptr, *volatile fRead = nullptr;
  if (mod < 0) {
#ifdef KBD_DIRECT
    // 3. This build also imports the keyboard functions the usual way: the PS5
    // found the library when it started the app, if this line shows at all.
    step("NOT LOADED, BUT THIS BUILD IMPORTS IT: USING THOSE");
    fInit = (void *)sceKeyboardInit;
    fOpen = (void *)sceKeyboardOpen;
    fState = (void *)sceKeyboardReadState;
    fRead = (void *)sceKeyboardRead;
#else
    step("THE KEYBOARD LIBRARY COULD NOT BE LOADED");
    return;
#endif
  } else {
    void *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
    sceKernelDlsym(mod, "sceKeyboardInit", &a);
    sceKernelDlsym(mod, "sceKeyboardOpen", &b);
    sceKernelDlsym(mod, "sceKeyboardReadState", &c);
    sceKernelDlsym(mod, "sceKeyboardRead", &d);
    fInit = a, fOpen = b, fState = c, fRead = d;
  }

  // 4. its functions
  step("FOUND: INIT %s OPEN %s READSTATE %s READ %s", fInit ? "Y" : "N", fOpen ? "Y" : "N", fState ? "Y" : "N",
       fRead ? "Y" : "N");
  readState = (FnReadState)fState;
  readData = (FnRead)fRead;

  // 5. init and open
  if (fInit) {
    logLine("about to call sceKeyboardInit()");
    step("INIT: %08X", (unsigned)((FnInit)fInit)());
  }
  if (!fOpen) {
    step("NO sceKeyboardOpen: STOPPED");
    return;
  }
  const int32_t users[] = {initialUser, 0xff, 0xfe, 0};
  for (int32_t u : users) {
    logLine("about to call sceKeyboardOpen(%d, 0, 0, NULL)", (int)u);
    int h = ((FnOpen)fOpen)(u, 0, 0, nullptr);
    step("OPEN USER %d (0x%X): %08X", (int)u, (unsigned)u, (unsigned)h);
    if (h >= 0) {
      handle = h;
      S.opened = true;
      break;
    }
  }
  if (!S.opened) step("NO KEYBOARD HANDLE: STOPPED");
  else step("READING. LEAVE THE KEYBOARD ALONE FOR A SECOND");
}

void frame() {
  if (!S.opened) return;
  if (S.reads == 0) logLine("about to call sceKeyboardReadState / sceKeyboardRead each frame");
  S.reads++;
  if (readState) {
    memset(buf, kFill, sizeof(buf));
    S.readStateRet = readState(handle, buf);
    S.writtenState = written(buf);
  }
  if (readData) {
    memset(buf2, kFill, sizeof(buf2));
    S.readRet = readData(handle, buf2, 1);
    S.writtenRead = written(buf2);
  }
  if (S.reads == 1) logLine("first reads: ReadState %08X (%d bytes), Read %08X (%d bytes)", (unsigned)S.readStateRet,
                            S.writtenState, (unsigned)S.readRet, S.writtenRead);
  // what is shown: ReadState's bytes, or Read's when ReadState gave nothing
  const uint8_t *src = (readState && S.readStateRet >= 0 && S.writtenState > 0) ? buf : buf2;
  memcpy(S.state, src, kStateBytes);
  for (int i = 0; i < kStateBytes; i++) {
    if (S.reads > 1 && S.state[i] != prev[i]) S.changedAt[i] = 0;
    else if (S.changedAt[i] < 255) S.changedAt[i]++;
  }

  // the first 60 reads (a second): the keyboard left alone. Bytes that change
  // then are a clock or a counter, and are left out of the changes below.
  if (S.reads <= 60) {
    if (S.reads > 1)
      for (int i = 0; i < kStateBytes; i++)
        if (S.state[i] != prev[i]) noisy[i] = true;
    memcpy(S.idle, S.state, kStateBytes);
    memcpy(prev, S.state, kStateBytes);
    if (S.reads == 60) {
      haveIdle = true;
      // a counter's higher bytes change too slowly to be seen in a second:
      // the rest of its 8 bytes (little-endian) go with its changing ones
      for (int i = 0; i < kStateBytes; i++)
        if (noisy[i])
          for (int j = i + 1; j < kStateBytes && (j & 7); j++) noisy[j] = true;
      char line[200];
      int n = snprintf(line, sizeof(line), "idle: ");
      for (int i = 0; i < 48 && n < 190; i++) n += snprintf(line + n, sizeof(line) - n, "%02X ", S.idle[i]);
      logLine("%s", line);
      n = snprintf(line, sizeof(line), "changing by themselves (left out):");
      for (int i = 0; i < kStateBytes && n < 190; i++)
        if (noisy[i]) n += snprintf(line + n, sizeof(line) - n, " %02X", i);
      logLine("%s", line);
    }
    return;
  }
  bool changed = false;
  for (int i = 0; i < kStateBytes; i++)
    if (!noisy[i] && S.state[i] != prev[i]) changed = true;
  memcpy(prev, S.state, kStateBytes);
  if (!changed || !haveIdle) return;

  char line[96];
  int n = snprintf(line, sizeof(line), "%5ld:", S.reads);
  int diffs = 0;
  for (int i = 0; i < kStateBytes; i++) {
    if (noisy[i] || S.state[i] == S.idle[i]) continue;
    diffs++;
    if (n < (int)sizeof(line) - 8) n += snprintf(line + n, sizeof(line) - n, " %02X=%02X", i, S.state[i]);
  }
  if (!diffs) snprintf(line + n, sizeof(line) - n, " (ALL KEYS UP)");
  memmove(S.changes[1], S.changes[0], sizeof(S.changes[0]) * (kChanges - 1));
  snprintf(S.changes[0], sizeof(S.changes[0]), "%s", line);
  if (S.changeCount < kChanges) S.changeCount++;
  logLine("%s", line);
}

const State &state() { return S; }

} // namespace KeyboardProbe
