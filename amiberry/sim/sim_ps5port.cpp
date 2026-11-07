// sim_ps5port.cpp - the PC simulator's stand-ins for amiberry/ps5port's
// diagnostics (the PS5 ones read FreeBSD's registers), and SIM_SANDBOX: no
// folder listing, as in the PS5's sandbox.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

extern "C" int ps5portDiagnostics(void (*log)(const char *)) {
  log("PS5 (simulator): diagnostics skipped\n");
  return 1;
}
extern "C" void ps5portWatch(void) {}
extern "C" void ps5portPoke(pthread_t) {}

extern "C" DIR *opendir(const char *name) {
  if (getenv("SIM_SANDBOX")) {
    errno = EPERM;
    return nullptr;
  }
  static DIR *(*real)(const char *) = (DIR * (*)(const char *)) dlsym(RTLD_NEXT, "opendir");
  return real(name);
}
