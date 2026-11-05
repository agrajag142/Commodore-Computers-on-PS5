// StateBuf.h
// Added for the Waveshare ESP32-P4 port: helpers to copy the emulated
// machine's state into a block of memory and back (C64Sys::saveState), so
// the SID player screen can borrow the C64 and give it back as it was.

#ifndef STATEBUF_H
#define STATEBUF_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct StateWriter {
  uint8_t *p;
  void bytes(const void *src, size_t n) {
    memcpy(p, src, n);
    p += n;
  }
  template <typename T> void put(const T &v) { bytes(&v, sizeof(T)); }
  template <typename T> void put(const std::atomic<T> &v) {
    T t = v.load(std::memory_order_acquire);
    bytes(&t, sizeof(T));
  }
};

struct StateReader {
  const uint8_t *p;
  void bytes(void *dst, size_t n) {
    memcpy(dst, p, n);
    p += n;
  }
  template <typename T> void get(T &v) { bytes(&v, sizeof(T)); }
  template <typename T> void get(std::atomic<T> &v) {
    T t;
    bytes(&t, sizeof(T));
    v.store(t, std::memory_order_release);
  }
};

#endif // STATEBUF_H
