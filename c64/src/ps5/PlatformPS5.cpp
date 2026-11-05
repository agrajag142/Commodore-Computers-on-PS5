// PlatformPS5.cpp
// The emulator's "platform" on the PS5 (src/core/platform/Platform.h):
// logging, the clock, waiting, threads and interval timers, with pthreads.
// Also the folders and the small helpers in ps5_app.h.

#include "../core/platform/Platform.h"
#include "../core/platform/PlatformFactory.h"
#include "ps5_app.h"
#include "ps5_sce.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================ folders
#ifdef PS5_SIM
// The PC simulator: the folders come from the environment
const char *ps5AppDir() {
  const char *d = getenv("SIM_APP0");
  return d ? d : "sim-app0";
}
const char *ps5DataDir() {
  const char *d = getenv("SIM_DOWNLOAD0");
  return d ? d : "sim-download0";
}
#else
const char *ps5AppDir() { return "/app0"; }
const char *ps5DataDir() { return "/download0"; }
#endif

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
    ps5Notify("Commodore 64: a thread did not start");
  }
  pthread_attr_destroy(&attr);
}

// ============================================================ the platform
namespace {

// A task or timer's function, kept until its thread has it
struct TaskFn {
  std::function<void(void *)> fn;
};
struct TimerFn {
  std::function<void()> fn;
  uint64_t intervalUs;
};

void taskThread(void *p) {
  TaskFn *t = (TaskFn *)p;
  t->fn(nullptr);
  delete t;
}

// Calls the function every intervalUs, on a steady beat (a late call doesn't
// push the later ones back)
void timerThread(void *p) {
  TimerFn *t = (TimerFn *)p;
  uint64_t next = ps5NowUs() + t->intervalUs;
  while (true) {
    uint64_t now = ps5NowUs();
    if (next > now) sceKernelUsleep((uint32_t)(next - now));
    t->fn();
    next += t->intervalUs;
    now = ps5NowUs();
    if (next + 100000 < now) next = now + t->intervalUs; // (fell far behind: start again)
  }
}

class PlatformPS5 : public Platform {
public:
  void log(LogLevel level, const char *tag, const char *format, ...) override {
#ifdef PS5_SIM
    static const char *levels[] = {"E", "W", "I", "D", "V"};
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s][%s] ", levels[level], tag);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
#else
    // Only errors and warnings, to the console's log
    if (level > LOG_WARN) return;
    va_list args;
    va_start(args, format);
    printf("[C64][%s] ", tag);
    vprintf(format, args);
    printf("\n");
    va_end(args);
#endif
  }

  uint8_t getRandomByte() override { return (uint8_t)(rand() >> 7); }

  int64_t getTimeUS() override { return (int64_t)ps5NowUs(); }

  // The emulator waits after every raster line (about 64 us) to keep to a
  // real C64's speed. The system's sleeps aren't that fine, so short waits
  // are skipped: the emulator then runs a little ahead and the next wait is
  // longer. It keeps time over the frame all the same.
  void waitUS(uint32_t us) override {
    if (us >= 1500) sceKernelUsleep(us);
  }

  void waitMS(uint32_t ms) override {
    if (ms) sceKernelUsleep(ms * 1000u);
  }

  void feedWDT() override {}

  void startIntervalTimer(std::function<void()> fn, uint64_t interval_us) override {
    ps5StartThread(timerThread, new TimerFn{fn, interval_us}, 64 * 1024);
  }

  void startTask(std::function<void(void *)> fn, uint8_t, uint8_t) override {
    ps5StartThread(taskThread, new TaskFn{fn}, 1024 * 1024);
  }
};

} // namespace

namespace PlatformNS {
Platform *create() { return new PlatformPS5(); }
} // namespace PlatformNS
