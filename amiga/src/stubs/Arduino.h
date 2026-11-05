// Arduino.h - PS5 stand-in: the Amiga emulator's board code only uses
// Serial.print for its log.
#ifndef PS5_ARDUINO_H
#define PS5_ARDUINO_H
struct PS5Serial {
  void print(const char *s);
};
extern PS5Serial Serial;
#endif
