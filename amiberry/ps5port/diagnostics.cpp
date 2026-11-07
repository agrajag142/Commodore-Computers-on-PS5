// diagnostics.cpp - PS5 port: checks for the adapter's log (built with C++
// exceptions, as Amiberry is; the adapter itself is built without)
#include <exception>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <filesystem>
#include <stdexcept>
#include <string>

static void (*logFn)(const char *) = nullptr;
static bool ownHandlersSet = false; // (our crash handlers are in: Amiberry's replacements are refused)

static void onTerminate() {
  if (logFn) {
    logFn("PS5: std::terminate (an exception nobody caught, or one that could not be thrown)\n");
    if (auto e = std::current_exception()) {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception &x) {
        logFn("PS5: the exception: ");
        logFn(x.what());
        logFn("\n");
      } catch (...) {
      }
    }
  }
  __builtin_trap();
}

// ---- where a crash or a hang happened: the instruction and the code
// addresses on the stack, as offsets from this file's ps5portDiagnostics
// (its address in llvm-pie.elf turns them into functions: tools/symbolize)
extern "C" int ps5portDiagnostics(void (*log)(const char *));

[[maybe_unused]] static void logAddresses(const char *what, int sig, ucontext_t *uc) {
  if (!logFn) return;
  char line[160];
  uintptr_t ref = (uintptr_t)&ps5portDiagnostics;
  uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip, rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
  snprintf(line, sizeof(line), "PS5: %s (signal %d) at ref%+lld, fault address %p\n", what, sig,
           (long long)(rip - ref), (void *)0);
  logFn(line);
  logFn("PS5: code addresses on the stack (ref+offset):");
  const uintptr_t *sp = (const uintptr_t *)rsp;
  int n = 0;
  // (only near the top of the stack: past its end the memory may not be there)
  for (int i = 0; i < 48 && n < 24; i++) {
    uintptr_t v = sp[i];
    long long d = (long long)(v - ref);
    if (d > -(64ll << 20) && d < (64ll << 20)) {
      snprintf(line, sizeof(line), " %+lld", d);
      logFn(line);
      n++;
    }
  }
  logFn("\n");
}

static volatile int crashing = 0;
static void dumpContext(void *ctx) {
  if (!logFn || !ctx) return;
  char line[64];
  uintptr_t ref = (uintptr_t)&ps5portDiagnostics;
  logFn("PS5: context words (ref+offset where it looks like code):");
  const uintptr_t *w = (const uintptr_t *)ctx;
  for (int i = 0; i < 64; i++) {
    long long d = (long long)(w[i] - ref);
    if (d > -(64ll << 20) && d < (64ll << 20)) snprintf(line, sizeof line, " [%d]ref%+lld", i, d);
    else snprintf(line, sizeof line, " [%d]%lx", i, (unsigned long)w[i]);
    logFn(line);
  }
  logFn("\n");
}
#include <setjmp.h>
#include <sys/mman.h>
static sigjmp_buf testJump;
static volatile int testing = 0;
static void onCrash(int sig, siginfo_t *info, void *ctx) {
  if (testing) { // (the self-test's own fault: back to it)
    testing = 0;
    siglongjmp(testJump, sig);
  }
  if (crashing++) _exit(2); // (a crash while reporting one)
  char line[96];
  snprintf(line, sizeof(line), "PS5: crash: fault address %p\n", info ? info->si_addr : nullptr);
  if (logFn) logFn(line);
  dumpContext(ctx);
  (void)sig;
  _exit(1);
}
// (only that it happened: the PS5's register layout in ctx isn't known
// here, and reading the stack through it took the app down)
static void onPoke(int, siginfo_t *, void *) {
  if (logFn) logFn("PS5: the emulator thread is still busy\n");
}
// The other signals that end an app by default: which one, and from where
// (SIGSYS, a system call the PS5 doesn't have, and SIGPIPE are logged and
// the call carries on with an error)
static void onOther(int sig, siginfo_t *info, void *ctx) {
  char line[96];
  snprintf(line, sizeof(line), "PS5: signal %d (code %d)\n", sig, info ? info->si_code : 0);
  if (logFn) logFn(line);
  dumpContext(ctx);
  if (sig == SIGSYS || sig == SIGPIPE) return;
  _exit(3);
}

// The crash signals logged; SIGUSR2 to a thread: where it is (for hangs)
extern "C" void ps5portWatch(void) {
  static char altStack[65536];
  stack_t ss{};
  ss.ss_sp = altStack;
  ss.ss_size = sizeof(altStack);
  sigaltstack(&ss, nullptr);
  struct sigaction a{};
  a.sa_sigaction = onCrash;
  a.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&a.sa_mask);
  for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &a, nullptr);
  ownHandlersSet = true;
  a.sa_sigaction = onOther;
  for (int s : {SIGSYS, SIGPIPE, SIGTRAP, SIGEMT, SIGXCPU, SIGXFSZ, SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGALRM,
                SIGVTALRM, SIGPROF, SIGUSR1})
    sigaction(s, &a, nullptr);
  a.sa_sigaction = onPoke;
  a.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR2, &a, nullptr);
}
extern "C" void ps5portPoke(pthread_t t) { pthread_kill(t, SIGUSR2); }

// Amiberry installs its own handlers for these (for its JIT, which this
// build doesn't have): ours stay, so a crash is reported. Wrapped at the link.
extern "C" int __real_sigaction(int, const struct sigaction *, struct sigaction *);
extern "C" void (*__real_signal(int, void (*)(int)))(int);
extern "C" void (*__wrap_signal(int sig, void (*fn)(int)))(int) {
  if (ownHandlersSet && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT)) {
    if (logFn) logFn("PS5: kept our handler (Amiberry called signal())\n");
    return SIG_DFL;
  }
  return __real_signal(sig, fn);
}
extern "C" int __wrap_sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  if (ownHandlersSet && act && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT)) {
    if (logFn) {
      char line[96];
      snprintf(line, sizeof line, "PS5: kept our handler for signal %d (Amiberry asked to replace it)\n", sig);
      logFn(line);
    }
    return __real_sigaction(sig, nullptr, old);
  }
  return __real_sigaction(sig, act, old);
}

// Every C++ exception, as it is thrown (the link wraps __cxa_throw): its type,
// its message and who threw it, into the log
#include <typeinfo>
extern "C" [[noreturn]] void __real___cxa_throw(void *obj, std::type_info *type, void (*dtor)(void *));
extern "C" [[noreturn]] void __wrap___cxa_throw(void *obj, std::type_info *type, void (*dtor)(void *)) {
  if (logFn) {
    char line[256];
    long long from = (long long)((uintptr_t)__builtin_return_address(0) - (uintptr_t)&ps5portDiagnostics);
    snprintf(line, sizeof(line), "PS5: throw %s from ref%+lld", type ? type->name() : "?", from);
    logFn(line);
    // (what() if it is a std::exception: the common case in Amiberry)
    // (the standard library's own exceptions all derive from std::exception
    // first: their names start with St / NSt)
    const char *n = type ? type->name() : "";
    if (!strncmp(n, "St", 2) || !strncmp(n, "NSt", 3)) {
      logFn(": ");
      logFn(static_cast<std::exception *>(obj)->what());
    }
    logFn("\n");
  }
  __real___cxa_throw(obj, type, dtor);
}

// The ways out of the app, wrapped at the link: who called them, in the log
extern "C" [[noreturn]] void __real_exit(int);
extern "C" [[noreturn]] void __real__exit(int);
extern "C" [[noreturn]] void __real_abort(void);
static void logExit(const char *what, int code, void *from) {
  if (!logFn) return;
  char line[160];
  snprintf(line, sizeof(line), "PS5: %s(%d) called from ref%+lld\n", what, code,
           (long long)((uintptr_t)from - (uintptr_t)&ps5portDiagnostics));
  logFn(line);
}
extern "C" [[noreturn]] void __wrap_exit(int code) {
  logExit("exit", code, __builtin_return_address(0));
  __real_exit(code);
}
extern "C" [[noreturn]] void __wrap__exit(int code) {
  logExit("_exit", code, __builtin_return_address(0));
  __real__exit(code);
}
extern "C" [[noreturn]] void __wrap_abort(void) {
  logExit("abort", 0, __builtin_return_address(0));
  __real_abort();
}

// Memory mapping, wrapped at the link: every call and its result, in the
// log (the app dies in frame 2, when Amiberry remaps the Amiga's memory)
#include <sys/mman.h>
extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void *, size_t);
extern "C" int __real_mprotect(void *, size_t, int);
extern "C" int __real_madvise(void *, size_t, int);
static void logMem(const char *what, void *addr, size_t len, int a, int b, long r, void *from) {
  if (!logFn) return;
  char line[200];
  snprintf(line, sizeof(line), "PS5: %s(%p, 0x%zx, %d, 0x%x) = %lx (errno %d) from ref%+lld\n", what, addr, len, a, b, r,
           errno, (long long)((uintptr_t)from - (uintptr_t)&ps5portDiagnostics));
  logFn(line);
}
extern "C" void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  void *from = __builtin_return_address(0);
  if (logFn) logMem("mmap before", addr, len, prot, flags, 0, from);
  void *r = __real_mmap(addr, len, prot, flags, fd, off);
  logMem("mmap", addr, len, prot, flags, (long)r, from);
  return r;
}
extern "C" int __wrap_munmap(void *addr, size_t len) {
  void *from = __builtin_return_address(0);
  logMem("munmap before", addr, len, 0, 0, 0, from);
  int r = __real_munmap(addr, len);
  logMem("munmap", addr, len, 0, 0, r, from);
  return r;
}
extern "C" int __wrap_mprotect(void *addr, size_t len, int prot) {
  void *from = __builtin_return_address(0);
  logMem("mprotect before", addr, len, prot, 0, 0, from);
  int r = __real_mprotect(addr, len, prot);
  logMem("mprotect", addr, len, prot, 0, r, from);
  return r;
}
extern "C" int __wrap_madvise(void *addr, size_t len, int adv) {
  void *from = __builtin_return_address(0);
  int r = __real_madvise(addr, len, adv);
  logMem("madvise", addr, len, adv, 0, r, from);
  return r;
}

// 1: an exception thrown and caught here (the unwinder works)
extern "C" int ps5portDiagnostics(void (*log)(const char *)) {
  logFn = log;
  std::set_terminate(onTerminate);
  int ok = 0;
  try {
    throw std::runtime_error("test");
  } catch (const std::exception &e) {
    ok = std::string(e.what()) == "test";
  }
  log(ok ? "PS5: C++ exceptions work\n" : "PS5: C++ exceptions: caught the wrong thing\n");
  std::error_code ec;
  bool d = std::filesystem::is_directory("/download0", ec);
  log(d ? "PS5: std::filesystem sees /download0\n" : "PS5: std::filesystem can't see /download0\n");
  if (ec) {
    log("PS5: std::filesystem error: ");
    log(ec.message().c_str());
    log("\n");
  }
  return ok;
}

// Function tracing (the core files built with -finstrument-functions): while
// on, every function the emulator thread enters, as ref+offset, into the log
// (the same function twice in a row: once)
static volatile int traceOn = 0;
static pthread_t traceThread;
static int traceCount = 0;
// (the threads started through ps5port/threads.c: their trace lines are
// marked A, B, C...; the emulator thread's T, any other thread's U)
static pthread_t traceThreads[16];
static volatile int traceThreadCount = 0;
static uintptr_t traceLast[18];
extern "C" __attribute__((no_instrument_function)) void ps5portTraceThread(void) {
  int i = __atomic_fetch_add(&traceThreadCount, 1, __ATOMIC_SEQ_CST);
  if (i < 16) traceThreads[i] = pthread_self();
}
extern "C" __attribute__((no_instrument_function)) void ps5portTrace(int on) {
  traceThread = pthread_self();
  memset(traceLast, 0, sizeof traceLast);
  traceOn = on;
  if (logFn) logFn(on ? "PS5: trace on\n" : "PS5: trace off\n");
}
extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *) {
  if (!traceOn || !logFn) return;
  pthread_t self = pthread_self();
  int slot = 17;
  char tag = 'U';
  if (pthread_equal(self, traceThread)) slot = 16, tag = 'T';
  else
    for (int i = 0; i < traceThreadCount && i < 16; i++)
      if (pthread_equal(self, traceThreads[i])) { slot = i; tag = (char)('A' + i); break; }
  if ((uintptr_t)fn == traceLast[slot]) return;
  traceLast[slot] = (uintptr_t)fn;
  if (__atomic_add_fetch(&traceCount, 1, __ATOMIC_RELAXED) > 400000) { traceOn = 0; logFn("PS5: trace full\n"); return; }
  char line[32];
  snprintf(line, sizeof line, "%c%llx\n", tag, (long long)((uintptr_t)fn - (uintptr_t)&ps5portDiagnostics));
  logFn(line);
}
extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *, void *) {}
extern "C" void ps5portLogThread(void *fn, int result) {
  if (!logFn) return;
  char line[96];
  snprintf(line, sizeof line, "PS5: thread ref%+lld started (%d)\n",
           (long long)((uintptr_t)fn - (uintptr_t)&ps5portDiagnostics), result);
  logFn(line);
}

// Self-test: does a fault reach our handler, on the thread's own stack and
// on a libco stack (where Amiberry runs), for a missing page and for a
// reserved (no access) page as Amiberry's memory is?
extern "C" void *co_active(void);
extern "C" void *co_create(unsigned int, void (*)(void));
extern "C" void co_switch(void *);
extern "C" void co_delete(void *);
static void *testMain, *testPage;
static const char *testWhat;
static void faultTest(const char *what, volatile char *where) {
  char line[128];
  snprintf(line, sizeof line, "PS5: self-test %s...\n", what);
  if (logFn) logFn(line);
  int sig = sigsetjmp(testJump, 1);
  if (!sig) {
    testing = 1;
    *where = 1;
    testing = 0;
    snprintf(line, sizeof line, "PS5: self-test %s: no fault at all\n", what);
  } else {
    snprintf(line, sizeof line, "PS5: self-test %s: reported (signal %d)\n", what, sig);
  }
  if (logFn) logFn(line);
}
static void testFiber() {
  faultTest("fiber, missing page", (volatile char *)8);
  faultTest("fiber, reserved page", (volatile char *)testPage);
  co_switch(testMain);
}
extern "C" void ps5portSelfTest(void) {
  testPage = mmap(nullptr, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  faultTest("thread, missing page", (volatile char *)8);
  if (testPage != MAP_FAILED) faultTest("thread, reserved page", (volatile char *)testPage);
  testMain = co_active();
  void *f = co_create(256 * 1024, testFiber);
  if (f) {
    co_switch(f);
    co_delete(f);
  }
  // (how much memory one mapping can have, read-write and reserved-only)
  for (int prot : {PROT_READ | PROT_WRITE, PROT_NONE}) {
    size_t lo = 0, hi = (size_t)4096 << 20;
    while (hi - lo > ((size_t)1 << 20)) {
      size_t mid = (lo + hi) / 2 & ~(size_t)0xfffff;
      void *p = __real_mmap(nullptr, mid, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (p != MAP_FAILED) {
        __real_munmap(p, mid);
        lo = mid;
      } else {
        hi = mid;
      }
    }
    char line[96];
    snprintf(line, sizeof line, "PS5: largest %s mapping: %zu MB\n", prot ? "read-write" : "reserved", lo >> 20);
    if (logFn) logFn(line);
  }
  if (logFn) logFn("PS5: self-test done\n");
  (void)testWhat;
}
extern "C" void ps5portLogLine(const char *line) {
  if (logFn) logFn(line);
}
