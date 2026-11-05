// AmigaCore.cpp
// The emulated Amiga 500 as one unit. See AmigaCore.h.
// The frame loop is the one from amiga500-esp32's a500_esp32.ino.

#include "AmigaCore.h"
#include "chips/a500.h"
#include <cstddef>
#include <cstring>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif
extern "C" {
#include "chips/m68k.h"
}

// ---- from the chipset sources ----
extern uint8_t *chip_ram;
extern uint8_t *slow_ram;
extern uint8_t *kick_rom;
extern int vpos;
extern int disk_irq_pending;
void cia_tod_hsync(void);
void cia_tod_vsync(void);
extern void (*denise_line_cb)(int vpos, const uint16_t *pixels, int w);
int cur_frame = 0;

static const int LINES_PAL = 312;
static const int CYC_LINE = 455; // 68000 cycles per line (7.09 MHz / 15.6 kHz)

static AmigaOutput output = {};
static std::atomic<bool> pictureOn{true};
static std::atomic<int> diskRequest{0}; // 0 none, 1 swap, 2 boot
static const uint8_t *diskData = nullptr;
static int diskOutFrames = 0;
static std::atomic<uint32_t> outTime{0}; // drawing time, us, since last frame end

// queue of captured lines (emulator task -> drawing task)
static const uint32_t QUEUE_LINES = 64;
static denise_line_t *queue = nullptr;
static std::atomic<uint32_t> qHead{0}, qTail{0};
static void (*qNotify)() = nullptr;
static void (*qWait)() = nullptr;

// the emulator captures each line straight into the next free place in the
// queue (waiting while the queue is full), then hands it over
static denise_line_t *queueSlot() {
  uint32_t head = qHead.load(std::memory_order_relaxed);
  while (head - qTail.load(std::memory_order_acquire) >= QUEUE_LINES) {
    if (qWait) qWait();
  }
  return &queue[head % QUEUE_LINES];
}

static void pushLine(const denise_line_t *) {
  uint32_t head = qHead.load(std::memory_order_relaxed);
  bool wasEmpty = head == qTail.load(std::memory_order_acquire);
  qHead.store(head + 1, std::memory_order_release);
  if (wasEmpty && qNotify) qNotify();
}

void amigaUseDrawingCore(void (*notify)(), void (*waitWhileFull)()) {
  if (queue == nullptr) {
#ifdef ARDUINO
    // fast internal memory: the two cores pass every line through it
    queue = (denise_line_t *)heap_caps_malloc(QUEUE_LINES * sizeof(denise_line_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (queue == nullptr) queue = new denise_line_t[QUEUE_LINES];
  }
  qNotify = notify;
  qWait = waitWhileFull;
  denise_slot_cb = queueSlot;
  denise_job_cb = pushLine;
}

static uint32_t rowHash[AMIGA_FRAME_H]; // of what each line on the screen was drawn from

// Everything a captured line's pixels depend on, as one number. A line
// whose number is the same as last time is already on the screen as it
// should be: it is not drawn again (stage 10: checked before drawing, so an
// unchanged line costs almost nothing).
static uint32_t lineHash(const denise_line_t *j) {
  uint32_t h = 2166136261u ^ j->mode;
  auto mix = [&h](uint32_t w) { h = (h ^ w) * 16777619u; h ^= h >> 15; };
  mix(j->bg);
  if (j->mode >= 1) {
    for (int i = 0; i < 32; i += 2) mix((uint32_t)j->pal[i] | (uint32_t)j->pal[i + 1] << 16);
    mix(j->nspr);
    for (int k = 0; k < j->nspr; k++) {
      mix((uint32_t)(uint16_t)j->spr[k].hstart | (uint32_t)j->spr[k].num << 16);
      mix((uint32_t)j->spr[k].data | (uint32_t)j->spr[k].datb << 16);
    }
  }
  if (j->mode == 2) {
    mix(j->hires | j->nplanes << 8 | j->scroll1 << 16 | (uint32_t)j->scroll2 << 24);
    mix(j->nbytes | j->dpf << 8 | j->pf2pri << 16);
    mix((uint32_t)(uint16_t)j->x0 | (uint32_t)(uint16_t)j->clip0 << 16);
    mix((uint16_t)j->clip1);
    int n = j->nbytes;
    for (int p = 0; p < j->nplanes; p++) {
      const uint32_t *w = (const uint32_t *)j->planes[p];
      for (int i = 0; i < n / 4; i++) mix(w[i]);
      if (n & 2) mix((uint32_t)j->planes[p][n - 2] << 8 | j->planes[p][n - 1]);
    }
  }
  return h;
}

int amigaDrawPending() {
  int n = 0;
  uint32_t tail = qTail.load(std::memory_order_relaxed);
  int64_t t0 = amigaMicros();
  while (tail != qHead.load(std::memory_order_acquire)) {
    const denise_line_t *j = &queue[tail % QUEUE_LINES];
    int r = j->vpos - AMIGA_FIRST_LINE;
    if (r >= 0 && r < AMIGA_FRAME_H) {
      uint32_t h = lineHash(j);
      if (h != rowHash[r]) {
        rowHash[r] = h;
        denise_draw_line(j);
      }
    }
    tail++;
    qTail.store(tail, std::memory_order_release);
    n++;
  }
  if (n) outTime.fetch_add((uint32_t)(amigaMicros() - t0), std::memory_order_relaxed);
  return n;
}
static int64_t noMicros() { return 0; }
int64_t (*amigaMicros)() = noMicros;

// Denise hands over one finished line: 640 pixels, RGB565. Straight into
// the panel's frame buffer, two rows per Amiga line.
static void lineDone(int v, const uint16_t *pixels, int w) {
  int r = v - AMIGA_FIRST_LINE;
  if (r < 0 || r >= AMIGA_FRAME_H || output.fb == nullptr || w < AMIGA_FRAME_W) return;
  if (!pictureOn.load(std::memory_order_relaxed)) return;
  uint16_t *dst = output.fb + (size_t)(output.y + 2 * r) * output.stride + output.x;
  memcpy(dst, pixels, AMIGA_FRAME_W * 2);
  memcpy(dst + output.stride, pixels, AMIGA_FRAME_W * 2);
  if (output.sync) output.sync(dst, ((size_t)output.stride + AMIGA_FRAME_W) * 2);
}

bool amigaCoreInit(uint8_t *chip, uint8_t *slow, uint8_t *rom, const AmigaOutput &out) {
  if (!chip || !rom || !out.fb) return false;
  chip_ram = chip;
  slow_ram = slow;
  kick_rom = rom;
  output = out;
  denise_first_line = AMIGA_FIRST_LINE;
  denise_last_line = AMIGA_FIRST_LINE + AMIGA_FRAME_H;
  amigaMarkAllDirty();
  denise_line_cb = lineDone;
  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  amigaReset();
  return true;
}

void amigaReset() {
  blitter_wait();
  mem_reset();
  cia_reset();
  custom_reset();
  paula_reset();
  m68k_pulse_reset();
  logmsg("[RESET] PC=%06X (Kickstart 1.3: FC00D2)\n", m68k_get_reg(NULL, M68K_REG_PC));
}

void amigaRunFrame(AmigaFrameTimes *times) {
  int64_t t0 = amigaMicros();
  int64_t tCpu = 0, tChip = 0, tRender = 0;
  long long blit0 = blitter_ns(), words0 = blitter_words();
  cur_frame++;
  // a new disk asked for (at the start of a frame: safe to change)
  int req = diskRequest.load(std::memory_order_acquire);
  if (req == 3) { // PS5 port: a reset asked for from another thread
    amigaReset();
    diskRequest.store(0, std::memory_order_release);
  } else if (req == 2) {
    memcpy(drive_alloc_adf(), diskData, 901120);
    drive_mount_ready();
    amigaReset();
    diskRequest.store(0, std::memory_order_release);
  } else if (req == 1) {
    if (diskOutFrames == 0) {
      drive_eject();
      diskOutFrames = 1;
    } else if (++diskOutFrames > 25) { // half a second without a disk
      memcpy(drive_alloc_adf(), diskData, 901120);
      drive_mount_ready();
      diskOutFrames = 0;
      diskRequest.store(0, std::memory_order_release);
    }
  }
  input_frame(cur_frame);
  copper_vblank();
  sprite_vblank(); // sprite channels restart at the top of the frame

  static int paulaAcc = 0;
  for (int line = 0; line < LINES_PAL; line++) {
    vpos = line;
    if (__atomic_load_n(&blit_irq_pending, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&blit_irq_pending, 0, __ATOMIC_RELEASE);
      intreq_set(6);
    }
    if (disk_irq_pending) { disk_irq_pending = 0; intreq_set(1); }

    int64_t a = amigaMicros();
    copper_run_line();
    int64_t b = amigaMicros();
    tChip += b - a;
    denise_render_line();
    int64_t c = amigaMicros();
    tRender += c - b;
    m68k_execute(CYC_LINE);
    int64_t d = amigaMicros();
    tCpu += d - c;

    // Paula runs on the colour clock (half the CPU clock)
    paulaAcc += CYC_LINE;
    paula_step(paulaAcc >> 1);
    paulaAcc &= 1;

    cia_tick(CYC_LINE / 10); // E clock = CPU / 10
    cia_tod_hsync();
    tChip += amigaMicros() - d;
  }
  cia_tod_vsync();
  intreq_set(5); // vertical blank
  if (times) {
    times->cpuUs = (uint32_t)tCpu;
    times->chipUs = (uint32_t)tChip;
    times->renderUs = (uint32_t)tRender;
    times->totalUs = (uint32_t)(amigaMicros() - t0);
    times->blitUs = (uint32_t)((blitter_ns() - blit0) / 1000);
    times->blitWords = (uint32_t)(blitter_words() - words0);
    times->outUs = outTime.exchange(0, std::memory_order_relaxed);
  }
}

void amigaUseBlitterCore(void (*kick)()) { blitter_async_kick = kick; }

static std::atomic<bool> paused{false};
void amigaPause(bool on) { paused.store(on, std::memory_order_release); }
bool amigaPaused() { return paused.load(std::memory_order_acquire); }

void amigaRunBlitter() { blitter_run_pending(); }

void amigaMarkAllDirty() {
  for (int r = 0; r < AMIGA_FRAME_H; r++) rowHash[r] = 0x12345678u; // "not drawn yet"
}

void amigaRequestDisk(const uint8_t *adf, bool boot) {
  diskData = adf;
  diskRequest.store(boot ? 2 : 1, std::memory_order_release);
}

// PS5 port: a reset at the start of the next frame (safe from another thread)
void amigaRequestReset() { diskRequest.store(3, std::memory_order_release); }

bool amigaDiskBusy() { return diskRequest.load(std::memory_order_acquire) != 0; }

void amigaShowPicture(bool on) {
  if (on) amigaMarkAllDirty();
  pictureOn.store(on, std::memory_order_release);
}
