// ps5_app.h
// What the parts of the PS5 app share: the folders, the C64 picture as the
// emulator last finished it, and what the controller tells the C64 (its
// keyboard and joysticks).
//
//   ps5_main.cpp     the TV picture, the DualSense, the on-screen keyboard
//                    and the menu; the program (main)
//   PS5Drivers.cpp   the emulator's display, sound, keyboard and joystick
//                    drivers, which read and write what is declared here
//   PS5File.cpp      drive 8: the app's c64 folder
//   PlatformPS5.cpp  threads, timers and the clock for the emulator

#ifndef PS5_APP_H
#define PS5_APP_H

#include <atomic>
#include <stdint.h>
#include <string>
#include <vector>

// ---- folders
// The app's own files (read-only): /app0 on the console. The C64 programs and
// disks are in <appDir>/assets/c64.
const char *ps5AppDir();
// The app's writable storage: /download0 on the console (an app can't write
// anywhere else). SAVE"NAME",8 writes to <dataDir>/c64.
const char *ps5DataDir();

// ---- the C64 picture (written by the display driver, on the main thread,
// inside VIC::refresh())
extern uint8_t g_c64Pixels[320 * 200]; // colour numbers 0-15
extern uint8_t g_c64Border;            // border colour 0-15

// ---- what the C64 sees of the controller (written by the main thread, read
// by the emulator's thread)
// The keyboard: one key of the 8 x 8 matrix (dc00 in bits 0-7, dc01 in bits
// 8-15, 0xff each = no key) and the modifier keys in bits 16-23 (1 SHIFT,
// 2 CTRL, 4 C=).
extern std::atomic<uint32_t> g_c64Key;
// The joysticks in port 1 and 2, as the C64 reads them: bit 0 up, 1 down,
// 2 left, 3 right, 4 fire; 0 = pressed (0xff = nothing).
extern std::atomic<uint8_t> g_c64Joy[2];

// ---- sound
void ps5SoundSetVolume(int percent); // 0-100

// ---- drive 8 (PS5File.cpp): the programs (.prg) and disks (.d64) there,
// sorted by name
std::vector<std::string> ps5ListC64Files();

// ---- small helpers
void ps5Notify(const char *msg); // a system notification (the pop-up)
uint64_t ps5NowUs();             // microseconds since the app started
void ps5StartThread(void (*fn)(void *), void *arg, size_t stackBytes);

#endif // PS5_APP_H
