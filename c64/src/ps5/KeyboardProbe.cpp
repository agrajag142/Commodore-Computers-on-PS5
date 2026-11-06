// KeyboardProbe.cpp - see KeyboardProbe.h

#include "KeyboardProbe.h"
#include "ps5_sce.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

  static const char *logs[] = {"/data/homebrew/PPSA99904/kbdtest.txt", "/download0/kbdtest.txt"};
  for (const char *p : logs)
    if ((logFile = fopen(p, "w")) != nullptr) {
      snprintf(S.logPath, sizeof(S.logPath), "%s", p);
      break;
    }
  logLine("USB KEYBOARD TEST (C64 for PS5). Initial user %d (0x%x)", (int)initialUser, (unsigned)initialUser);

  // 1. the library
  static const char *paths[] = {"libSceKeyboard.sprx", "/system/common/lib/libSceKeyboard.sprx",
                                "/system/priv/lib/libSceKeyboard.sprx"};
  int mod = -1;
  for (const char *p : paths) {
    logLine("about to load %s", p);
    mod = sceKernelLoadStartModule(p, 0, nullptr, 0, nullptr, nullptr);
    step("LOAD %s: %08X", p, (unsigned)mod);
    if (mod >= 0) break;
  }
  if (mod < 0) {
    step("THE KEYBOARD LIBRARY COULD NOT BE LOADED");
    return;
  }

  // 2. its functions
  void *fInit = nullptr, *fOpen = nullptr, *fState = nullptr, *fRead = nullptr;
  sceKernelDlsym(mod, "sceKeyboardInit", &fInit);
  sceKernelDlsym(mod, "sceKeyboardOpen", &fOpen);
  sceKernelDlsym(mod, "sceKeyboardReadState", &fState);
  sceKernelDlsym(mod, "sceKeyboardRead", &fRead);
  step("FOUND: INIT %s OPEN %s READSTATE %s READ %s", fInit ? "Y" : "N", fOpen ? "Y" : "N", fState ? "Y" : "N",
       fRead ? "Y" : "N");
  readState = (FnReadState)fState;
  readData = (FnRead)fRead;

  // 3. init and open
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
