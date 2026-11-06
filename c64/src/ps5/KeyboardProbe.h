// KeyboardProbe.h
// The menu's USB KEYBOARD TEST: does a USB keyboard on the PS5 reach the app
// through Sony's keyboard library (libSceKeyboard), and what does it send?
//
// The library is loaded at run time (sceKernelLoadStartModule), not linked:
// if the PS5 won't give it to a homebrew app, the test says so instead of the
// app failing to start. Its functions' arguments aren't documented here, so
// the test calls them as the PS4's were and shows the raw bytes they return.
// Every step also goes into a log file, written before each call: if a call
// crashes the app, the log's last line says which one.

#ifndef KEYBOARD_PROBE_H
#define KEYBOARD_PROBE_H

#include <stdint.h>

namespace KeyboardProbe {

static const int kLines = 26;      // the set-up steps shown
static const int kChanges = 8;     // the latest changes shown
static const int kStateBytes = 96; // the bytes shown of what a read returns

struct State {
  char steps[kLines][80];
  int stepCount;
  char logPath[64]; // "" = no log file could be written
  bool opened;      // a keyboard handle
  int readStateRet, readRet;
  int writtenState, writtenRead; // bytes the reads wrote (the rest kept their fill)
  uint8_t state[kStateBytes], idle[kStateBytes];
  uint8_t changedAt[kStateBytes]; // frames since that byte last changed (255: long ago)
  char changes[kChanges][96];     // newest first: the bytes unlike the first read
  int changeCount;
  long reads;
};

void start(int32_t initialUser); // load, init, open (once; again it only resets the display)
void frame();                    // one read a frame
const State &state();

} // namespace KeyboardProbe

#endif // KEYBOARD_PROBE_H
