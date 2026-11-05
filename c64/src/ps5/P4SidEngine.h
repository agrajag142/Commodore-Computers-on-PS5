// P4SidEngine.h
// Plays SID music files (PSID and RSID, the .sid files of the High Voltage
// SID Collection) on the emulated C64.
//
// A .sid file holds a C64 music routine plus a header: where the code goes
// in memory, its "init" routine (called once with the song number) and its
// "play" routine (called 50 times a second, or at the rate a CIA timer is
// set to). For each tune the engine:
//   1. puts the C64 back into the state it was in just after start-up
//      (a copy of memory taken then, and the chips reset),
//   2. copies the music into memory,
//   3. adds a small 6502 driver that calls init, then sets up a timer
//      interrupt that calls play - as the standard SID players do,
//   4. starts the C64 at the driver.
// The music then plays on the same emulated SID chip as games do.
//
// The touch screen (another task) asks for things with load() / stop() /
// setPaused(); the work is done on the emulator's task at the end of a
// frame, through C64Sys::frameHook, when it is safe to change the machine.
// No Arduino code in here, so it can also be tested on a desktop PC.
//
// In the C64 emulator the player is a second screen. enter() saves the
// whole C64 (memory, CPU and chips) and gives the machine to the player;
// leave() puts the C64 back exactly as it was, so a game carries on.
// Between the two the engine does nothing to the machine.

#ifndef P4SIDENGINE_H
#define P4SIDENGINE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

class C64Sys;

// PS5 port: std::mutex lives in the C++ library a PS5 app doesn't link, so a
// small spin lock stands in (lock_guard and unique_lock work with it)
struct SidSpinLock {
  std::atomic_flag f = ATOMIC_FLAG_INIT;
  void lock() {
    while (f.test_and_set(std::memory_order_acquire)) {
    }
  }
  bool try_lock() { return !f.test_and_set(std::memory_order_acquire); }
  void unlock() { f.clear(std::memory_order_release); }
};

struct SidTuneInfo {
  bool rsid;            // "real C64" tune: sets up its own interrupts
  uint16_t version;
  uint16_t loadAddr;
  uint16_t initAddr;
  uint16_t playAddr;    // 0 = the tune installs its own interrupt
  uint16_t dataLen;     // bytes of C64 code and data
  uint8_t songs;        // 1-255
  uint8_t startSong;    // 1-based
  uint32_t speed;       // bit n set: song n+1 is timed by a CIA timer
  uint8_t clock;        // 0 unknown, 1 PAL, 2 NTSC, 3 both
  uint16_t sidModel;    // 0 unknown, 6581, 8580, or 1 = either
  bool basic;           // RSID that must be started with BASIC's RUN
  char name[33];
  char author[33];
  char released[33];
};

class P4SidEngine {
public:
  static const size_t MAX_FILE = 65536 + 0x7c + 2;

  static P4SidEngine &get();

  // Hook the engine into the emulator (call once, after the emulator starts)
  void attach(C64Sys *cpu);

  // Read the header of a .sid file. Returns nullptr if it is fine, or a
  // short reason (in capitals, for the screen) if it cannot be played.
  static const char *parse(const uint8_t *file, size_t len, SidTuneInfo &info);

  // Start playing song "song" (1-based; 0 = the tune's own start song) of a
  // .sid file. The file is copied, so the caller's buffer is free again when
  // this returns. Returns nullptr, or why it cannot be played.
  const char *load(const uint8_t *file, size_t len, int song);

  // Play another song of the tune loaded last (1-based)
  void selectSong(int song);

  void stop();
  void setPaused(bool paused);

  // Take the C64 over for the player (saving it) / give it back as it was.
  // Done at the end of the next frame; isActive() tells when.
  void enter();
  void leave();
  bool isActive() const { return active.load(std::memory_order_acquire); }

  // SID chip to play on: 0 = what each tune was written for (6581 when the
  // file does not say), 6581 or 8580
  void setModelMode(int mode) { modelMode.store(mode, std::memory_order_release); }
  int getModelMode() const { return modelMode.load(std::memory_order_acquire); }
  int modelInUse() const { return modelUsed.load(std::memory_order_acquire); }
  bool isPaused() const { return paused; }

  // ---- status, for the screen ----
  bool ready() const { return snapshotTaken.load(std::memory_order_acquire); }
  bool isPlaying() const { return playing.load(std::memory_order_acquire); }
  int currentSong() const { return song.load(std::memory_order_acquire); }
  // frames (1/50 s) the current song has played
  uint32_t framesPlayed() const { return frames.load(std::memory_order_acquire); }
  // frames in a row with no sound
  uint32_t silentFrames() const { return silent.load(std::memory_order_acquire); }
  const SidTuneInfo &tuneInfo() const { return info; }
  C64Sys *machine() const { return cpu; }

  // Build the driver for a tune (also used by the tests).
  // Returns its length, or 0 if there is no free place for it.
  static size_t buildDriver(const SidTuneInfo &info, int song, uint8_t *out,
                            uint16_t &addr);

  // the emulator calls this at the end of every frame (C64Sys::frameHook)
  static void frameHookStatic(C64Sys *c);

private:
  enum Request : uint8_t { REQ_NONE, REQ_LOAD, REQ_STOP };

  C64Sys *cpu = nullptr;
  uint8_t *snapshot = nullptr; // C64 memory just after start-up
  uint8_t *pending = nullptr;  // the tune's code and data, waiting to start
  uint8_t *c64State = nullptr; // the C64 as it was when the player took over
  SidTuneInfo info = {};
  SidTuneInfo pendingInfo = {};
  int pendingSong = 1;
  uint32_t bootFrames = 0;
  bool paused = false;

  SidSpinLock pendingMutex; // guards pending, pendingInfo, pendingSong
  std::atomic<uint8_t> request{REQ_NONE};
  std::atomic<bool> snapshotTaken{false};
  std::atomic<bool> playing{false};
  std::atomic<int> song{0};
  std::atomic<uint32_t> frames{0};
  std::atomic<uint32_t> silent{0};
  std::atomic<int> modelMode{0};
  std::atomic<int> modelUsed{6581};
  std::atomic<bool> wantActive{false};
  std::atomic<bool> active{false};

  void onFrame();
  void resetMachine();
  void startPending();
  void idleMachine();
};

#endif // P4SIDENGINE_H
