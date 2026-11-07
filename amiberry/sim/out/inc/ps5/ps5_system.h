// ps5_system.h
// The PS5 app's small helpers (ps5_system.cpp) and its files (AmigaFilesPS5.cpp).

#ifndef PS5_SYSTEM_H
#define PS5_SYSTEM_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// The app's own files (read-only): /app0 on the console. The Kickstart and the
// disks are in <appDir>/assets/amiga.
const char *ps5AppDir();
uint64_t ps5NowUs();             // microseconds since the app started
void ps5Notify(const char *msg); // a system notification (the pop-up)
void ps5StartThread(void (*fn)(void *), void *arg, size_t stackBytes);

namespace AmigaFiles {
// Read the Kickstart into rom (256K). Returns "" or why it failed.
std::string loadKickstart(uint8_t *rom);
// Every Kickstart-looking file (.rom and the like), decrypted if it is
// Amiga Forever's: fn(name, data, length) for each
void forEachKickstart(void (*fn)(const char *name, const uint8_t *data, size_t len));
// The .adf disks, sorted by name
std::vector<std::string> listDisks();
// Read a disk image (901120 bytes) into buf. Returns "" or why not.
std::string readDisk(const std::string &name, uint8_t *buf);
} // namespace AmigaFiles

#endif // PS5_SYSTEM_H
