// AmigaCore.h
// The emulated Amiga 500 as one unit: memory, 68000 CPU (Musashi), the
// chipset (copper, blitter, Denise, Paula, CIAs, floppy) - all from
// amiga500-esp32 in src/amiga - plus the picture it makes.
//
// The emulator task (core 1) calls amigaRunFrame() 50 times a second. Every
// picture line Denise draws that differs from what is on the screen is
// written straight into the panel's frame buffer, twice (two screen rows
// per Amiga line), 640 pixels wide.
//
// No Arduino code in here, so it can also be tested on a desktop PC.

#ifndef AMIGACORE_H
#define AMIGACORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

// The picture: the 256 lines of a PAL screen from line AMIGA_FIRST_LINE,
// 640 pixels across (low-resolution pixels are doubled)
static const int AMIGA_FRAME_W = 640;
static const int AMIGA_FRAME_H = 256;
static const int AMIGA_FIRST_LINE = 44; // the usual top of the display window (DIWSTRT $2C)

// Where the picture goes: a frame buffer (RGB565), its width in pixels,
// the top left corner of the picture, and a function that makes written
// memory visible to the display (cache write-back)
struct AmigaOutput {
  uint16_t *fb;
  int stride;
  int x, y;
  void (*sync)(void *p, size_t bytes);
};

// Memory blocks: chip RAM 512K, slow RAM 512K, Kickstart 256K. The caller
// allocates them (internal RAM or PSRAM) and loads the Kickstart.
bool amigaCoreInit(uint8_t *chip, uint8_t *slow, uint8_t *rom, const AmigaOutput &out);

// Power-on reset (keeps the disk in the drive)
void amigaReset();

// Run one PAL frame (312 lines). Returns the microseconds spent, split up.
struct AmigaFrameTimes {
  uint32_t cpuUs, chipUs, renderUs, totalUs;
  uint32_t blitUs; // the blitter (on core 0 for area blits when amigaUseBlitterCore is used)
  uint32_t blitWords; // words the blitter did (pixels for lines)
  uint32_t outUs;  // drawing the lines (on the drawing core when it is used)
};
void amigaRunFrame(AmigaFrameTimes *times);

// Draw every line again in the next frame (e.g. after the screen was covered)
void amigaMarkAllDirty();

// ---- Disks ----
// Put a disk image (901120 bytes, kept by the caller until amigaDiskBusy()
// is false) in DF0:. boot = also restart the Amiga so it starts from it;
// otherwise the old disk comes out for half a second first, so the Amiga
// notices the change.
void amigaRequestDisk(const uint8_t *adf, bool boot);
bool amigaDiskBusy();
// PS5 port: reset (as Ctrl-Amiga-Amiga) at the start of the next frame
void amigaRequestReset();

// ---- Picture on or off ----
// Off: lines are not written to the screen (something else is shown
// there). On again: the whole picture is drawn again.
void amigaShowPicture(bool on);

// ---- Drawing on another core ----
// After amigaUseDrawingCore(), the emulator only captures each picture line
// (bitplane data, colours, sprites) into a queue; amigaDrawPending(),
// called from a task on the other core, draws them and puts them on the
// screen. notify is called when the queue stops being empty (wake the
// drawing task); waitWhileFull is called while the queue is full.
void amigaUseDrawingCore(void (*notify)(), void (*waitWhileFull)());

// Area blits on another core (stage 11): kick() is called when a blit is
// waiting; the task on that core then calls amigaRunBlitter(). Meanwhile
// the 68000 carries on, as on the real machine.
void amigaUseBlitterCore(void (*kick)());

// Pause the Amiga (the MOD player is open): the emulator task stops
// running frames until it is let go again
void amigaPause(bool on);
bool amigaPaused();
void amigaRunBlitter();
// Draw the captured lines; returns how many
int amigaDrawPending();

// Time source for the frame statistics (microseconds); set by the sketch
extern int64_t (*amigaMicros)();

#endif // AMIGACORE_H
