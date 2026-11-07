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
extern "C" void ps5portTrace(int) {}
extern "C" void ps5portSelfTest(void) {}

extern "C" DIR *opendir(const char *name) {
  if (getenv("SIM_SANDBOX")) {
    errno = EPERM;
    return nullptr;
  }
  static DIR *(*real)(const char *) = (DIR * (*)(const char *)) dlsym(RTLD_NEXT, "opendir");
  return real(name);
}

// (the PS5: lstat of a folder is refused)
#include <sys/stat.h>
extern "C" int lstat(const char *path, struct stat *st) {
  static int (*real)(const char *, struct stat *) = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "lstat");
  int r = real(path, st);
  if (r == 0 && getenv("SIM_SANDBOX") && S_ISDIR(st->st_mode)) {
    errno = EPERM;
    return -1;
  }
  return r;
}
extern "C" int lstat64(const char *path, struct stat64 *st) {
  static int (*real)(const char *, struct stat64 *) = (int (*)(const char *, struct stat64 *))dlsym(RTLD_NEXT, "lstat64");
  int r = real(path, st);
  if (r == 0 && getenv("SIM_SANDBOX") && S_ISDIR(st->st_mode)) {
    errno = EPERM;
    return -1;
  }
  return r;
}
// (and realpath: refused)
extern "C" char *realpath(const char *path, char *resolved) {
  if (getenv("SIM_SANDBOX")) {
    errno = EPERM;
    return nullptr;
  }
  static char *(*real)(const char *, char *) = (char *(*)(const char *, char *))dlsym(RTLD_NEXT, "realpath");
  return real(path, resolved);
}
