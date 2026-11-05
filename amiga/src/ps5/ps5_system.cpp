// ps5_system.cpp
// What the Amiga emulator's board code expects from the ESP32 and Arduino
// (the stand-ins in ../stubs), done the PS5 way, and the app's small helpers
// (ps5_system.h): the folders, the clock, threads, notifications.

#include "../stubs/Arduino.h"
#include "../stubs/esp_heap_caps.h"
#include "ps5_sce.h"
#include "ps5_system.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

// ============================================================ folders
#ifdef PS5_SIM
const char *ps5AppDir() {
  const char *d = getenv("SIM_APP0");
  return d ? d : "sim-app0";
}
#else
const char *ps5AppDir() { return "/app0"; }
#endif

// ============================================================ memory
// Big blocks (the CPU's tables, a disk, Paula's ring) straight from the
// system with mmap: a PS5 app's malloc heap is small. Each block starts with
// a 64-byte header saying how big it is, for heap_caps_free.
extern "C" void *heap_caps_malloc(size_t n, unsigned) {
  size_t total = (n + 64 + 0xfff) & ~(size_t)0xfff;
  void *p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) {
    p = malloc(n + 64);
    if (!p) return nullptr;
    ((size_t *)p)[0] = 0; // (0: from malloc)
    return (uint8_t *)p + 64;
  }
  ((size_t *)p)[0] = total;
  return (uint8_t *)p + 64;
}
extern "C" void heap_caps_free(void *q) {
  if (!q) return;
  uint8_t *p = (uint8_t *)q - 64;
  size_t total = ((size_t *)p)[0];
  if (total) munmap(p, total);
  else free(p);
}
extern "C" size_t heap_caps_get_largest_free_block(unsigned) { return (size_t)256 << 20; }

// ============================================================ the emulator's log
PS5Serial Serial;
void PS5Serial::print(const char *s) {
#ifdef PS5_SIM
  if (getenv("SIM_LOG")) fputs(s, stderr);
#else
  (void)s; // (the console's log would only fill up)
#endif
}

// ============================================================ helpers
uint64_t ps5NowUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  static uint64_t t0 = 0;
  uint64_t t = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
  if (t0 == 0) t0 = t;
  return t - t0;
}

void ps5Notify(const char *msg) {
  static struct {
    uint8_t reserved[45];
    char message[3075];
  } req;
  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

namespace {
struct ThreadStart {
  void (*fn)(void *);
  void *arg;
};
void *threadTrampoline(void *p) {
  ThreadStart t = *(ThreadStart *)p;
  delete (ThreadStart *)p;
  t.fn(t.arg);
  return nullptr;
}
} // namespace

void ps5StartThread(void (*fn)(void *), void *arg, size_t stackBytes) {
  ThreadStart *t = new ThreadStart{fn, arg};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stackBytes);
  pthread_t th;
  // (no thread names: pthread_setname_np hangs on the PS5)
  if (pthread_create(&th, &attr, threadTrampoline, t) == 0) {
    pthread_detach(th);
  } else {
    delete t;
    ps5Notify("Amiga 500: a thread did not start");
  }
  pthread_attr_destroy(&attr);
}
