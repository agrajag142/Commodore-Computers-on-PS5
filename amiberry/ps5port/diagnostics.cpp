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

static void logAddresses(const char *what, int sig, ucontext_t *uc) {
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
  for (int i = 0; i < 2048 && n < 40; i++) {
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

static void onCrash(int sig, siginfo_t *info, void *ctx) {
  char line[96];
  snprintf(line, sizeof(line), "PS5: crash: fault address %p\n", info ? info->si_addr : nullptr);
  if (logFn) logFn(line);
  logAddresses("crash", sig, (ucontext_t *)ctx);
  _exit(1);
}
static void onPoke(int sig, siginfo_t *, void *ctx) { logAddresses("still busy", sig, (ucontext_t *)ctx); }

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
  a.sa_sigaction = onPoke;
  a.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR2, &a, nullptr);
}
extern "C" void ps5portPoke(pthread_t t) { pthread_kill(t, SIGUSR2); }

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
