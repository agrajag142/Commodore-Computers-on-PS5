// diagnostics.cpp - PS5 port: checks for the adapter's log (built with C++
// exceptions, as Amiberry is; the adapter itself is built without)
#include <exception>
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
