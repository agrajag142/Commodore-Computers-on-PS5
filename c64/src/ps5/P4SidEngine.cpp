// P4SidEngine.cpp
// Plays .sid music files on the emulated C64. See P4SidEngine.h.

#include "P4SidEngine.h"
#include "../core/C64Sys.h"
#include <cstring>

// The C64 has finished starting up (BASIC's READY) well within this many
// frames after power-on; then a copy of its memory is taken.
static const uint32_t BOOT_FRAMES = 200;

P4SidEngine &P4SidEngine::get() {
  static P4SidEngine engine;
  return engine;
}

void P4SidEngine::attach(C64Sys *c) {
  cpu = c;
  // (PS5 port: fixed buffers, not from the app's small malloc heap)
  static uint8_t snapshotBuf[65536], pendingBuf[65536], stateBuf[C64Sys::STATE_SIZE];
  snapshot = snapshotBuf;
  pending = pendingBuf;
  c64State = stateBuf;
  cpu->frameHook = &P4SidEngine::frameHookStatic;
}

void P4SidEngine::frameHookStatic(C64Sys *) { get().onFrame(); }

// ---------------------------------------------------------------- header

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void copyText(char *dst, const uint8_t *src) {
  for (int i = 0; i < 32; i++) {
    uint8_t c = src[i];
    if (c == 0) {
      dst[i] = 0;
      return;
    }
    dst[i] = (c < 32 || c > 126) ? ' ' : (char)c; // keep it to plain ASCII
  }
  dst[32] = 0;
}

const char *P4SidEngine::parse(const uint8_t *f, size_t len, SidTuneInfo &in) {
  memset(&in, 0, sizeof(in));
  if (len < 0x76) {
    return "NOT A SID FILE";
  }
  bool psid = memcmp(f, "PSID", 4) == 0;
  bool rsid = memcmp(f, "RSID", 4) == 0;
  if (!psid && !rsid) {
    return "NOT A SID FILE";
  }
  in.rsid = rsid;
  in.version = be16(f + 4);
  uint16_t dataOffset = be16(f + 6);
  in.loadAddr = be16(f + 8);
  in.initAddr = be16(f + 0x0a);
  in.playAddr = be16(f + 0x0c);
  in.songs = (uint8_t)(be16(f + 0x0e) > 255 ? 255 : be16(f + 0x0e));
  in.startSong = (uint8_t)(be16(f + 0x10) > 255 ? 1 : be16(f + 0x10));
  in.speed = ((uint32_t)f[0x12] << 24) | ((uint32_t)f[0x13] << 16) |
             ((uint32_t)f[0x14] << 8) | f[0x15];
  copyText(in.name, f + 0x16);
  copyText(in.author, f + 0x36);
  copyText(in.released, f + 0x56);
  if (in.version >= 2 && len >= 0x7c) {
    uint16_t flags = be16(f + 0x76);
    in.basic = rsid && (flags & 0x02);
    in.clock = (flags >> 2) & 3;
    switch ((flags >> 4) & 3) {
    case 1:
      in.sidModel = 6581;
      break;
    case 2:
      in.sidModel = 8580;
      break;
    case 3:
      in.sidModel = 1;
      break;
    default:
      in.sidModel = 0;
    }
  }
  if (dataOffset < 0x76 || dataOffset >= len) {
    return "DAMAGED SID FILE";
  }
  size_t dataLen = len - dataOffset;
  if (in.loadAddr == 0) {
    // the load address is the first two bytes of the data, as in a .prg
    if (dataLen < 3) {
      return "DAMAGED SID FILE";
    }
    in.loadAddr = f[dataOffset] | (f[dataOffset + 1] << 8);
    dataLen -= 2;
  }
  if (in.loadAddr + dataLen > 0x10000) {
    dataLen = 0x10000 - in.loadAddr;
  }
  in.dataLen = (uint16_t)dataLen;
  if (in.loadAddr < 0x0200) {
    return "LOADS INTO ZERO PAGE";
  }
  if (in.songs == 0) {
    in.songs = 1;
  }
  if (in.startSong == 0 || in.startSong > in.songs) {
    in.startSong = 1;
  }
  if (in.basic) {
    if (in.loadAddr != 0x0801) {
      return "BASIC TUNE NOT AT $0801";
    }
  } else if (in.initAddr == 0) {
    in.initAddr = in.loadAddr;
  }
  if (rsid) {
    in.playAddr = 0; // RSID tunes always set up their own interrupts
  }
  return nullptr;
}

// Where the tune's code and data start in the file
static size_t dataStart(const uint8_t *f, const SidTuneInfo &) {
  size_t off = be16(f + 6);
  if (be16(f + 8) == 0) {
    off += 2;
  }
  return off;
}

// ---------------------------------------------------------------- driver

static bool overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
  return a0 < b1 && b0 < a1; // [a0, a1) and [b0, b1)
}

// Value of the C64's memory configuration register ($01) for running code
// at addr: everything above the code's address that would hide it is
// switched out (as the standard SID players do).
static uint8_t bankFor(uint16_t addr) {
  if (addr < 0xa000) return 0x37;  // BASIC, I/O and KERNAL in
  if (addr < 0xd000) return 0x36;  // BASIC out
  if (addr >= 0xe000) return 0x35; // BASIC and KERNAL out, I/O in
  return 0x34;                     // all RAM
}

namespace {
struct Asm {
  uint8_t *out;
  uint16_t org;
  size_t n = 0;
  void b(uint8_t v) { out[n++] = v; }
  void w(uint16_t v) {
    b(v & 0xff);
    b(v >> 8);
  }
  uint16_t pc() const { return (uint16_t)(org + n); }
  void lda(uint8_t v) { b(0xa9); b(v); }
  void sta(uint16_t a) {
    if (a < 0x100) { b(0x85); b((uint8_t)a); } else { b(0x8d); w(a); }
  }
  void ldaAbs(uint16_t a) { b(0xad); w(a); }
  void jsr(uint16_t a) { b(0x20); w(a); }
  void jmp(uint16_t a) { b(0x4c); w(a); }
};
} // namespace

size_t P4SidEngine::buildDriver(const SidTuneInfo &in, int song, uint8_t *out,
                                uint16_t &addr) {
  const size_t NEED = 128;
  uint32_t t0 = in.loadAddr, t1 = (uint32_t)in.loadAddr + in.dataLen;

  // find a free place
  int found = -1;
  if (!overlaps(0x0334, 0x0334 + NEED, t0, t1) && 0x0334 + NEED <= 0x0400) {
    found = 0x0334;
  }
  if (found < 0) {
    static const uint8_t ranges[][2] = {{0x04, 0x9f}, {0xc0, 0xcf}};
    for (auto &r : ranges) {
      for (int page = r[0]; page <= r[1] && found < 0; page++) {
        if (!overlaps(page << 8, (page << 8) + NEED, t0, t1)) {
          found = page << 8;
        }
      }
      if (found >= 0) break;
    }
  }
  if (found < 0) {
    return 0;
  }
  addr = (uint16_t)found;

  Asm a{out, addr};
  uint8_t songIndex = (uint8_t)(song - 1);
  bool tuneHas0314 = overlaps(0x0314, 0x0316, t0, t1);
  bool tuneHasFFFE = overlaps(0xfffe, 0x10000, t0, t1);

  if (in.basic) {
    // RSID tune written in BASIC: set BASIC's pointers to the end of the
    // program, put RUN in the keyboard buffer and go to BASIC's main loop
    uint16_t end = (uint16_t)t1;
    a.b(0x78); // SEI
    a.lda(songIndex);
    a.sta(0x030c); // SYS / song number, as the RSID format asks
    for (uint16_t zp = 0x2d; zp <= 0x32; zp += 2) {
      a.lda(end & 0xff);
      a.sta(zp);
      a.lda(end >> 8);
      a.sta(zp + 1);
    }
    const char *run = "RUN\r";
    for (int i = 0; i < 4; i++) {
      a.lda((uint8_t)run[i]);
      a.sta(0x0277 + i);
    }
    a.lda(4);
    a.sta(0x00c6);
    a.b(0x58); // CLI
    a.jmp(0xa474); // BASIC: READY and read the next line
    return a.n;
  }

  if (in.rsid || in.playAddr == 0) {
    // The tune sets up its own interrupt. Leave the C64 as BASIC left it
    // (KERNAL interrupt running) and call init.
    a.b(0x78); // SEI
    a.lda(bankFor(in.initAddr));
    a.sta(0x0001);
    a.b(0xa2); a.b(0x00); // LDX #0
    a.b(0xa0); a.b(0x00); // LDY #0
    a.lda(songIndex);
    a.jsr(in.initAddr);
    a.b(0x58); // CLI
    uint16_t loop = a.pc();
    a.jmp(loop);
    return a.n;
  }

  // Ordinary tune: quiet machine, init, then a timer interrupt calls play
  uint32_t bit = (song - 1) < 32 ? (song - 1) : 31;
  bool ciaSpeed = (in.speed >> bit) & 1;

  a.b(0x78); // SEI
  a.lda(0x7f);
  a.sta(0xdc0d); // CIA interrupts off
  a.sta(0xdd0d);
  a.ldaAbs(0xdc0d); // and acknowledged
  a.ldaAbs(0xdd0d);
  a.lda(0x00);
  a.sta(0xd01a); // no video chip interrupts
  a.lda(bankFor(in.initAddr));
  a.sta(0x0001);
  a.b(0xa2); a.b(0x00); // LDX #0
  a.b(0xa0); a.b(0x00); // LDY #0
  a.lda(songIndex);
  a.jsr(in.initAddr);
  a.b(0x78); // SEI (init may have allowed interrupts)
  a.lda(0x37);
  a.sta(0x0001);
  if (!ciaSpeed) {
    // 50 times a second: one PAL frame is 312 x 63 = 19656 cycles
    a.lda((19656 - 1) & 0xff);
    a.sta(0xdc04);
    a.lda((19656 - 1) >> 8);
    a.sta(0xdc05);
  }
  // (a CIA-timed tune has set the timer in init; if not, BASIC's 60 Hz
  // setting stays)
  size_t vecPatch = a.n; // interrupt vectors, filled in below
  a.lda(0); a.sta(0x0314);
  a.lda(0); a.sta(0x0315);
  a.lda(0); a.sta(0xfffe);
  a.lda(0); a.sta(0xffff);
  a.lda(0x81);
  a.sta(0xdc0d); // timer A interrupt on
  a.lda(0x11);
  a.sta(0xdc0e); // load the timer and start it
  a.b(0x58); // CLI
  uint16_t loop = a.pc();
  a.jmp(loop);

  // interrupt, KERNAL switched out: the CPU comes here through $FFFE
  uint16_t irqRaw = a.pc();
  a.b(0x48); a.b(0x8a); a.b(0x48); a.b(0x98); a.b(0x48); // PHA TXA PHA TYA PHA
  // interrupt through the KERNAL (it has saved A, X, Y): via $0314
  uint16_t irqKernal = a.pc();
  a.b(0xa5); a.b(0x01); // LDA $01
  a.b(0x48);            // PHA
  a.lda(bankFor(in.playAddr));
  a.sta(0x0001);
  a.jsr(in.playAddr);
  a.b(0x68);            // PLA
  a.sta(0x0001);
  a.ldaAbs(0xdc0d);     // acknowledge the timer
  a.b(0x68); a.b(0xa8); a.b(0x68); a.b(0xaa); a.b(0x68); // PLA TAY PLA TAX PLA
  a.b(0x40);            // RTI

  // fill in the vectors (skip one the tune's own data covers)
  out[vecPatch + 1] = irqKernal & 0xff;
  out[vecPatch + 6] = irqKernal >> 8;
  out[vecPatch + 11] = irqRaw & 0xff;
  out[vecPatch + 16] = irqRaw >> 8;
  if (tuneHas0314) {
    for (int i = 0; i < 10; i++) out[vecPatch + i] = 0xea; // NOP
  }
  if (tuneHasFFFE) {
    for (int i = 10; i < 20; i++) out[vecPatch + i] = 0xea;
  }
  return a.n;
}

// ---------------------------------------------------------------- requests

const char *P4SidEngine::load(const uint8_t *file, size_t len, int s) {
  if (cpu == nullptr || pending == nullptr) {
    // attach() was never called: the sketch's .ino must call
    // P4SidEngine::get().attach(&c64Emu.cpu) after c64Emu.setup()
    return "PLAYER NOT STARTED";
  }
  SidTuneInfo in;
  const char *err = parse(file, len, in);
  if (err) {
    return err;
  }
  if (s <= 0 || s > in.songs) {
    s = in.startSong;
  }
  uint8_t drv[160];
  uint16_t drvAddr;
  if (buildDriver(in, s, drv, drvAddr) == 0) {
    return "NO ROOM FOR THE PLAYER";
  }
  {
    std::lock_guard<SidSpinLock> lock(pendingMutex);
    memcpy(pending, file + dataStart(file, in), in.dataLen);
    pendingInfo = in;
    pendingSong = s;
    request.store(REQ_LOAD, std::memory_order_release);
  }
  if (paused) {
    setPaused(false);
  }
  return nullptr;
}

void P4SidEngine::selectSong(int s) {
  std::lock_guard<SidSpinLock> lock(pendingMutex);
  if (pendingInfo.songs == 0) {
    return; // nothing loaded yet
  }
  if (s < 1) s = pendingInfo.songs;
  if (s > pendingInfo.songs) s = 1;
  pendingSong = s;
  request.store(REQ_LOAD, std::memory_order_release);
  if (paused) {
    setPaused(false);
  }
}

void P4SidEngine::stop() {
  request.store(REQ_STOP, std::memory_order_release);
  if (paused) {
    setPaused(false);
  }
}

void P4SidEngine::enter() {
  if (paused) {
    setPaused(false);
  }
  wantActive.store(true, std::memory_order_release);
}

void P4SidEngine::leave() {
  if (paused) {
    setPaused(false);
  }
  wantActive.store(false, std::memory_order_release);
}

void P4SidEngine::setPaused(bool p) {
  paused = p;
  if (cpu != nullptr) {
    // the emulator stops running the C64 (and so the music) while halted
    cpu->cpuhalted = p;
  }
}

// ---------------------------------------------------------------- emulator side

void P4SidEngine::resetMachine() {
  cpu->cia1.init(true);
  cpu->cia2.init(false);
  cpu->vic.initVarsAndRegs();
  uint8_t vol = cpu->sid.getEmuVolume();
  cpu->sid.initSID();
  cpu->sid.setEmuVolume(vol);
  memcpy(cpu->getRam(), snapshot, 65536);
  cpu->setMem(0x0001, 0x37);
  // the chips as the KERNAL sets them up at start-up
  cpu->setMem(0xdc0d, 0x7f);
  cpu->setMem(0xdd0d, 0x7f);
  cpu->setMem(0xdc02, 0xff);
  cpu->setMem(0xdc03, 0x00);
  cpu->setMem(0xdc00, 0x7f);
  cpu->setMem(0xdd02, 0x3f);
  cpu->setMem(0xdd00, 0x07);
  cpu->setMem(0xd418, 0x00);
  // KERNAL's 60 Hz timer interrupt (keyboard, clock)
  cpu->setMem(0xdc04, 0x25);
  cpu->setMem(0xdc05, 0x40);
  cpu->setMem(0xdc0d, 0x81);
  cpu->setMem(0xdc0e, 0x11);
  cpu->setStack(0xff);
  cpu->setIFlag(true);
}

void P4SidEngine::startPending() {
  resetMachine();
  uint8_t *ram = cpu->getRam();
  memcpy(ram + pendingInfo.loadAddr, pending, pendingInfo.dataLen);
  uint8_t drv[160];
  uint16_t drvAddr = 0;
  size_t n = buildDriver(pendingInfo, pendingSong, drv, drvAddr);
  memcpy(ram + drvAddr, drv, n);
  cpu->setPC(drvAddr);
  info = pendingInfo;
  song.store(pendingSong, std::memory_order_release);
  frames.store(0, std::memory_order_release);
  silent.store(0, std::memory_order_release);
  playing.store(true, std::memory_order_release);
}

void P4SidEngine::idleMachine() {
  resetMachine();
  // a tiny program that keeps the SID quiet: SEI, volume 0, loop
  static const uint8_t idle[] = {0x78, 0xa9, 0x00, 0x8d, 0x18, 0xd4,
                                 0x4c, 0x3a, 0x03};
  memcpy(cpu->getRam() + 0x0334, idle, sizeof(idle));
  cpu->setPC(0x0334);
  playing.store(false, std::memory_order_release);
}

void P4SidEngine::onFrame() {
  if (!snapshotTaken.load(std::memory_order_relaxed)) {
    if (++bootFrames >= BOOT_FRAMES) {
      memcpy(snapshot, cpu->getRam(), 65536);
      snapshotTaken.store(true, std::memory_order_release);
    }
    return;
  }
  // switch between the C64 screen and the player
  bool want = wantActive.load(std::memory_order_acquire);
  if (want != active.load(std::memory_order_relaxed)) {
    if (want) {
      size_t n = cpu->saveState(c64State);
      (void)n; // always <= C64Sys::STATE_SIZE
      cpu->setCartSuspended(true); // a cartridge would hide the tune's memory
      idleMachine();
    } else {
      cpu->loadState(c64State); // the SID chip model and cartridge map come back with it
      playing.store(false, std::memory_order_release);
    }
    request.store(REQ_NONE, std::memory_order_release);
    active.store(want, std::memory_order_release);
    return;
  }
  if (!want) {
    return; // the C64 screen: leave the machine alone
  }

  // SID chip model: chosen on the screen, or what the tune asks for
  int mode = modelMode.load(std::memory_order_acquire);
  int model = mode ? mode
                   : ((playing.load(std::memory_order_relaxed) && info.sidModel == 8580) ? 8580 : 6581);
  if (cpu->sid.getModel() != model) {
    cpu->sid.setModel(model);
  }
  modelUsed.store(model, std::memory_order_release);

  uint8_t req = request.load(std::memory_order_acquire);
  if (req == REQ_LOAD) {
    // the touch screen may be handing over a tune right now: then next frame
    std::unique_lock<SidSpinLock> lock(pendingMutex, std::try_to_lock);
    if (lock.owns_lock()) {
      startPending();
      request.store(REQ_NONE, std::memory_order_release);
    }
    return;
  }
  if (req == REQ_STOP) {
    idleMachine();
    request.store(REQ_NONE, std::memory_order_release);
    return;
  }
  if (playing.load(std::memory_order_relaxed)) {
    frames.fetch_add(1, std::memory_order_relaxed);
    const SID &sid = cpu->sid;
    bool quiet = (sid.sidreg[0x18] & 0x0f) == 0 ||
                 (sid.sidVoice[0].env == 0 && sid.sidVoice[1].env == 0 &&
                  sid.sidVoice[2].env == 0);
    if (quiet) {
      silent.fetch_add(1, std::memory_order_relaxed);
    } else {
      silent.store(0, std::memory_order_relaxed);
    }
  }
}
