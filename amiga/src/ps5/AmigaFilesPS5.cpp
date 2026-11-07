// AmigaFilesPS5.cpp
// The Kickstart ROM and the disk images (.adf), from the app's assets/amiga
// folder: the board's AmigaFiles.cpp with files instead of the SD card.
//
// The PS5's sandbox won't list a folder, so the build writes
// assets/amiga/files.txt with the names in it; if the folder can be listed
// after all (the PC simulator), that's used too.

#include "../stubs/esp_heap_caps.h"
#include "ps5_system.h"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const size_t ROM_256K = 0x40000;
static const size_t ADF_SIZE = 901120;

static std::string folder() { return std::string(ps5AppDir()) + "/assets/amiga/"; }

static bool endsWith(const std::string &n, const char *ext) {
  size_t e = strlen(ext);
  return n.size() > e && strcasecmp(n.c_str() + n.size() - e, ext) == 0;
}

static bool exists(const std::string &name) {
  FILE *f = fopen((folder() + name).c_str(), "rb");
  if (f) fclose(f);
  return f != nullptr;
}

// Every file name in the folder: files.txt, the folder itself if it can be
// listed, and the usual names if they are there (so a Kickstart, rom.key or
// default.adf copied in over FTP is found without a new files.txt)
static std::vector<std::string> allNames() {
  std::vector<std::string> out;
  auto add = [&out](const std::string &n) {
    for (auto &o : out)
      if (strcasecmp(o.c_str(), n.c_str()) == 0) return;
    out.push_back(n);
  };
  FILE *f = fopen((folder() + "files.txt").c_str(), "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
      if (n) add(line);
    }
    fclose(f);
  }
  static const char *usual[] = {"kick.rom", "kick34005.A500", "kick13.rom", "amiga-os-130.rom", "rom.key",
                                "default.adf", "df0.adf", "df1.adf", "disk1.adf", "disk2.adf", "default.hdf",
                                // (the later Kickstarts: the usual names, Amiga Forever's among them)
                                "kick20.rom", "kick204.rom", "kick205.rom", "kick31.rom", "kick37175.A500",
                                "kick37350.A600", "kick40063.A600", "kick40068.A1200", "kick39106.A1200",
                                "amiga-os-204.rom", "amiga-os-205.rom", "amiga-os-205-a600.rom", "amiga-os-300-a1200.rom",
                                "amiga-os-310-a500.rom", "amiga-os-310-a600.rom", "amiga-os-310-a1200.rom"};
  for (const char *u : usual)
    if (exists(u)) add(u);
  DIR *d = opendir(folder().c_str());
  if (d) {
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') add(e->d_name);
    closedir(d);
  }
  std::sort(out.begin(), out.end(),
            [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  return out;
}

// A whole file (up to max bytes) into a new buffer (from the system, not the
// small malloc heap: a Kickstart is 256-512K)
static uint8_t *readFile(const std::string &path, size_t max, size_t &len) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) size = 0;
  if ((size_t)size > max) size = (long)max;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(size ? (size_t)size : 1, 0);
  len = buf ? fread(buf, 1, (size_t)size, f) : 0;
  fclose(f);
  return buf;
}

static uint32_t crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xffffffff;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & (0 - (c & 1)));
  }
  return ~c;
}

namespace AmigaFiles {

std::string loadKickstart(uint8_t *rom) {
  // which file: one of these names, else the first .rom (or .a500) there
  std::vector<std::string> names = allNames();
  static const char *preferred[] = {"kick.rom", "kick34005.A500", "kick13.rom", "amiga-os-130.rom"};
  std::string found;
  for (const char *p : preferred)
    for (auto &n : names)
      if (found.empty() && strcasecmp(n.c_str(), p) == 0) found = n;
  for (auto &n : names)
    if (found.empty() && (endsWith(n, ".rom") || endsWith(n, ".a500"))) found = n;
  if (found.empty()) return "NO KICKSTART ROM IN THE APP";

  size_t len = 0;
  uint8_t *data = readFile(folder() + found, 0x80000 + 16, len);
  if (data == nullptr) return "CANNOT READ " + found;
  uint8_t *p = data;
  // Amiga Forever (Cloanto) encrypted ROM: "AMIROMTYPE1" + ROM xor rom.key
  if (len > 11 && memcmp(data, "AMIROMTYPE1", 11) == 0) {
    p = data + 11;
    len -= 11;
    size_t keyLen = 0;
    uint8_t *key = readFile(folder() + "rom.key", 4096, keyLen);
    if (key == nullptr || keyLen == 0) {
      heap_caps_free(data);
      heap_caps_free(key);
      return "ENCRYPTED ROM - PUT rom.key NEXT TO IT";
    }
    for (size_t i = 0; i < len; i++) p[i] ^= key[i % keyLen];
    heap_caps_free(key);
  }
  if (len == 2 * ROM_256K && memcmp(p, p + ROM_256K, ROM_256K) == 0) len = ROM_256K; // 256K stored twice
  if (len != ROM_256K) {
    heap_caps_free(data);
    if (len == 2 * ROM_256K) return "512K KICKSTART (2.0 OR LATER) - USE 1.3";
    return "KICKSTART IS NOT 256K: " + found;
  }
  if (p[0] == 0x14 && p[1] == 0x11) // a byte-swapped dump
    for (size_t i = 0; i < len; i += 2) std::swap(p[i], p[i + 1]);
  memcpy(rom, p, ROM_256K);
  heap_caps_free(data);
  char msg[96];
  uint32_t crc = crc32(rom, ROM_256K);
  snprintf(msg, sizeof(msg), "Amiga: Kickstart %s, CRC32 %08X%s\n", found.c_str(), (unsigned)crc,
           crc == 0xC4F0F55F ? " (Kickstart 1.3 A500)" : "");
  printf("%s", msg);
  return "";
}

void forEachKickstart(void (*fn)(const char *name, const uint8_t *data, size_t len)) {
  static const char *exts[] = {".rom", ".a500", ".a600", ".a1200", ".a4000", ".bin"};
  for (auto &n : allNames()) {
    bool rom = false;
    for (const char *e : exts) rom |= endsWith(n, e);
    if (!rom) continue;
    size_t len = 0;
    uint8_t *data = readFile(folder() + n, 0x80000 + 16, len);
    if (!data) continue;
    uint8_t *p = data;
    if (len > 11 && memcmp(data, "AMIROMTYPE1", 11) == 0) {
      p = data + 11;
      len -= 11;
      size_t keyLen = 0;
      uint8_t *key = readFile(folder() + "rom.key", 4096, keyLen);
      if (!key || !keyLen) {
        heap_caps_free(data);
        heap_caps_free(key);
        continue;
      }
      for (size_t i = 0; i < len; i++) p[i] ^= key[i % keyLen];
      heap_caps_free(key);
    }
    fn(n.c_str(), p, len);
    heap_caps_free(data);
  }
}

std::vector<std::string> listHardDrives() {
  std::vector<std::string> out;
  for (auto &n : allNames())
    if (endsWith(n, ".hdf") || endsWith(n, ".hdz")) out.push_back(n);
  return out;
}

std::string pathOf(const std::string &name) { return folder() + name; }

std::vector<std::string> listDisks() {
  std::vector<std::string> disks;
  for (auto &n : allNames())
    if (endsWith(n, ".adf")) disks.push_back(n);
  return disks;
}

std::string readDisk(const std::string &name, uint8_t *buf) {
  FILE *f = fopen((folder() + name).c_str(), "rb");
  if (!f) return "CANNOT OPEN " + name;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size != (long)ADF_SIZE) {
    fclose(f);
    return "NOT AN 880K DISK IMAGE: " + name;
  }
  size_t n = fread(buf, 1, ADF_SIZE, f);
  fclose(f);
  return n == ADF_SIZE ? "" : "CANNOT READ " + name;
}

} // namespace AmigaFiles
