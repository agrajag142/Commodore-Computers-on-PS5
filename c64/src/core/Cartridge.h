// Cartridge.h
// Added for the Waveshare ESP32-P4 port: cartridges from .crt files.
//
// Supported .crt hardware types:
//   0  Normal, in its three forms
//        8K       ROM at $8000-$9FFF ("ROML"); BASIC and the KERNAL stay
//        16K      ROM at $8000-$BFFF (ROML + "ROMH" in place of BASIC)
//        Ultimax  ROM at $8000-$9FFF and $E000-$FFFF (in place of the
//                 KERNAL); the C64 sees only its first 4K of RAM, and the
//                 video chip reads the top 4K of the cartridge at $3000
//                 (early Commodore cartridges: Jupiter Lander, Kickman...)
//  19  Magic Desk (also Domark, Hes Australia, and most modern one-file
//      game conversions): 8K banks at $8000. Writing to $DE00 picks the
//      bank (bits 0-6, up to 1 MB); bit 7 set switches the cartridge off.
//   5  Ocean: 8K banks; writing to $DE00 picks the bank (bits 0-5). In
//      16K mode the same bank is also seen at $A000.
//  15  C64 Game System / System 3: 8K banks; writing anything to
//      $DE00+n picks bank n.
// Other bank-switched cartridges (EasyFlash, Action Replay...) are refused
// with their type name.
//
// The screen task reads a .crt file and calls parse(), then insert() or
// remove(). The emulator task picks the request up at the end of a frame
// (C64Sys::run) and restarts the C64 with the cartridge plugged in or out.
// No Arduino code in here.

#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

class Cartridge {
public:
  enum Mode : uint8_t { NONE, MODE_8K, MODE_16K, MODE_ULTIMAX };
  enum Request : uint8_t { REQ_NONE, REQ_INSERT, REQ_REMOVE };
  enum Type : uint16_t { T_NORMAL = 0, T_OCEAN = 5, T_SYSTEM3 = 15, T_MAGICDESK = 19 };

  static Cartridge &get();

  // Read a whole .crt file into the waiting slot. Returns nullptr if it can
  // be used, or a short reason (in capitals, for the screen).
  const char *parse(const uint8_t *file, size_t len);
  // name of the cartridge parse() accepted last
  const char *parsedName() const { return pendName; }

  // Size a .crt file may have (the largest Magic Desk, 1 MB, with headers)
  static const size_t MAX_FILE = 1024 * 1024 + 128 * 16 + 0x40;

  // plug in the cartridge parse() accepted / pull out the one plugged in;
  // either restarts the C64
  void insert() { request.store(REQ_INSERT, std::memory_order_release); }
  void remove() { request.store(REQ_REMOVE, std::memory_order_release); }

  // what is plugged in (for the screen)
  bool isInserted() const { return insertedFlag.load(std::memory_order_acquire); }
  const char *name() const { return activeName; }

  // ---- emulator side (C64Sys) ----
  std::atomic<uint8_t> request{REQ_NONE};
  // take the waiting cartridge (REQ_INSERT) or none (REQ_REMOVE)
  void apply(uint8_t req);
  Mode mode = NONE;          // at start-up (Magic Desk can switch itself off)
  uint16_t type = T_NORMAL;
  uint8_t *lo = nullptr;     // the 8K banks, one after the other
  uint8_t *hi = nullptr;     // 8K: ROMH of a normal 16K / Ultimax cartridge
  uint32_t bankMask = 0;     // banks - 1 (a power of two)

  static const char *typeName(uint16_t type);

private:
  uint8_t *pendLo = nullptr, *pendHi = nullptr;
  size_t pendLoSize = 0, loSize = 0;
  uint32_t pendMask = 0;
  Mode pendMode = NONE;
  uint16_t pendType = T_NORMAL;
  char pendName[33] = "";
  char activeName[33] = "";
  std::atomic<bool> insertedFlag{false};
};

#endif // CARTRIDGE_H
