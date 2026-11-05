// Cartridge.cpp
// Added for the Waveshare ESP32-P4 port: cartridges from .crt files.
// See Cartridge.h.

#include "Cartridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

Cartridge &Cartridge::get() {
  static Cartridge cart;
  return cart;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// The hardware types of the .crt format (as numbered by VICE)
const char *Cartridge::typeName(uint16_t type) {
  static const char *names[] = {"NORMAL",        "ACTION REPLAY",  "KCS POWER",
                                "FINAL CART III", "SIMONS BASIC",   "OCEAN",
                                "EXPERT",        "FUN PLAY",       "SUPER GAMES",
                                "ATOMIC POWER",  "EPYX FASTLOAD",  "WESTERMANN",
                                "REX UTILITY",   "FINAL CART I",   "MAGIC FORMEL",
                                "SYSTEM 3",      "WARP SPEED",     "DINAMIC",
                                "ZAXXON",        "MAGIC DESK",     "SUPER SNAPSHOT",
                                "COMAL 80",      "STRUCT BASIC",   "ROSS",
                                "DELA EP64",     "DELA EP7X8",     "DELA EP256",
                                "REX EP256",     "MIKRO ASSEMBLER", "FINAL CART PLUS",
                                "ACTION REPLAY 4", "STARDOS",      "EASYFLASH"};
  if (type < sizeof(names) / sizeof(names[0])) return names[type];
  return "SPECIAL";
}

// copy n bytes to an 8K slot; a chip smaller than 8K appears again in the
// rest of its 8K
static void put8k(uint8_t *dst, const uint8_t *src, uint32_t n) {
  if (n == 0) return;
  if (n > 8192) n = 8192;
  for (uint32_t o = 0; o < 8192; o += n) {
    memcpy(dst + o, src, (o + n <= 8192) ? n : 8192 - o);
  }
}

const char *Cartridge::parse(const uint8_t *f, size_t len) {
  if (request.load(std::memory_order_acquire) != REQ_NONE) {
    return "BUSY - TRY AGAIN";
  }
  if (len < 0x40 || memcmp(f, "C64 CARTRIDGE   ", 16) != 0) {
    return "NOT A .CRT FILE";
  }
  uint32_t hdrLen = be32(f + 0x10);
  if (hdrLen < 0x40) hdrLen = 0x40; // some files have it wrong
  uint16_t t = be16(f + 0x16);
  uint8_t exrom = f[0x18], game = f[0x19];
  if (t != T_NORMAL && t != T_OCEAN && t != T_SYSTEM3 && t != T_MAGICDESK) {
    static char msg[48];
    snprintf(msg, sizeof(msg), "%s CARTRIDGE - NOT SUPPORTED", typeName(t));
    return msg;
  }
  Mode m;
  if (t == T_MAGICDESK || t == T_SYSTEM3) {
    m = MODE_8K; // whatever the header says
  } else if (exrom == 0 && game != 0) {
    m = MODE_8K;
  } else if (exrom == 0 && game == 0) {
    m = MODE_16K;
  } else if (exrom != 0 && game == 0 && t == T_NORMAL) {
    m = MODE_ULTIMAX;
  } else {
    return "CARTRIDGE STARTS SWITCHED OFF";
  }

  // first pass: check the chips, find the highest bank
  uint32_t maxBank = 0;
  int chips = 0;
  size_t pos = hdrLen;
  while (pos + 0x10 <= len && memcmp(f + pos, "CHIP", 4) == 0) {
    uint32_t packet = be32(f + pos + 4);
    uint16_t bank = be16(f + pos + 0x0a);
    uint16_t size = be16(f + pos + 0x0e);
    if (packet < 0x10 || pos + 0x10 + size > len) {
      return "BROKEN .CRT FILE";
    }
    uint32_t last = bank + (size > 8192 ? (size - 1) / 8192 : 0);
    if (t != T_NORMAL && last > maxBank) maxBank = last;
    chips++;
    pos += packet;
  }
  if (chips == 0) {
    return "NO ROM IN THE .CRT FILE";
  }
  if (maxBank >= 128) {
    return "TOO MANY BANKS";
  }
  uint32_t banks = 1;
  while (banks <= maxBank) banks <<= 1;

  // room for it (the old waiting cartridge, if any, goes)
  size_t loBytes = (size_t)banks * 8192;
  if (pendLo == nullptr || pendLoSize < loBytes) {
    free(pendLo);
    pendLo = (uint8_t *)malloc(loBytes);
    pendLoSize = pendLo ? loBytes : 0;
  }
  if (pendHi == nullptr) pendHi = (uint8_t *)malloc(8192);
  if (pendLo == nullptr || pendHi == nullptr) {
    return "NO MEMORY";
  }
  memset(pendLo, 0xff, loBytes);
  memset(pendHi, 0xff, 8192);

  // second pass: copy the ROM
  pos = hdrLen;
  while (pos + 0x10 <= len && memcmp(f + pos, "CHIP", 4) == 0) {
    uint32_t packet = be32(f + pos + 4);
    uint16_t bank = be16(f + pos + 0x0a);
    uint16_t addr = be16(f + pos + 0x0c);
    uint16_t size = be16(f + pos + 0x0e);
    const uint8_t *data = f + pos + 0x10;
    if (t != T_NORMAL) {
      // banked: each 8K of the chip is the next bank (load address $8000
      // or, for big Ocean cartridges, $A000)
      for (uint32_t o = 0; o < size; o += 8192) {
        uint32_t b = (bank + o / 8192) & (banks - 1);
        put8k(pendLo + b * 8192, data + o, (size - o) < 8192 ? size - o : 8192);
      }
    } else if (addr == 0x8000 && size > 0x2000) {
      put8k(pendLo, data, 0x2000);
      put8k(pendHi, data + 0x2000, size - 0x2000);
    } else if (addr >= 0x8000 && addr < 0xa000) {
      uint32_t off = addr - 0x8000;
      if (off == 0) put8k(pendLo, data, size);
      else memcpy(pendLo + off, data, off + size <= 0x2000 ? size : 0x2000 - off);
    } else if ((addr >= 0xa000 && addr < 0xc000) || addr >= 0xe000) {
      uint32_t off = addr - ((addr >= 0xe000) ? 0xe000 : 0xa000);
      uint32_t n = size > 0x2000 - off ? 0x2000 - off : size;
      if (off == 0) {
        put8k(pendHi, data, n);
      } else {
        // e.g. 4K at $F000: also seen at $E000
        memcpy(pendHi + off, data, n);
        if (off == 0x1000 && n == 0x1000) memcpy(pendHi, data, n);
      }
    }
    pos += packet;
  }
  pendMode = m;
  pendType = t;
  pendMask = banks - 1;
  char name[33];
  memcpy(name, f + 0x20, 32);
  name[32] = 0;
  for (int i = 31; i >= 0 && (name[i] == ' ' || name[i] == 0); i--) name[i] = 0;
  strcpy(pendName, name[0] ? name : "CARTRIDGE");
  return nullptr;
}

// On the emulator task. The waiting and plugged-in cartridges swap places,
// so nothing big is copied; the screen task only touches the waiting slot
// while no request is open.
void Cartridge::apply(uint8_t req) {
  if (req == REQ_INSERT && pendMode != NONE && pendLo != nullptr) {
    std::swap(lo, pendLo);
    std::swap(loSize, pendLoSize);
    std::swap(hi, pendHi);
    mode = pendMode;
    type = pendType;
    bankMask = pendMask;
    pendMode = NONE; // the slot now holds the old cartridge
    strcpy(activeName, pendName);
    insertedFlag.store(true, std::memory_order_release);
  } else {
    mode = NONE;
    activeName[0] = 0;
    insertedFlag.store(false, std::memory_order_release);
  }
}
