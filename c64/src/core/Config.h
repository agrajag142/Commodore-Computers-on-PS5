/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 For the complete text of the GNU General Public License see
 http://www.gnu.org/licenses/.
*/
#ifndef CONFIG_H
#define CONFIG_H

// PS5 port (November 2026): the emulator's settings for the PS5 app. The
// board files of the other ports (ESP32 boards, Linux, Windows) are left out;
// the PS5 has one set of drivers, in ../ps5 (see the factories' headers).

#include <cstdint>

#define PLATFORM_PS5
#define BOARD_PS5

// SID samples per second: the PS5's AudioOut runs at 48 kHz only
#define AUDIO_SAMPLE_RATE 48000

struct Config {
  // delay until next display refresh (not used: the PS5 refreshes on vblank)
  static const uint8_t REFRESHDELAY = 0;

  // sound level at start-up (0-255)
  static const uint8_t DEFAULT_VOLUME = 200;

  // filesystem: drive 8 is the app's c64 folder (programs, .d64 disks)
  static constexpr const char *PATH = "";
  static constexpr const char *CONFIGFILE = ".config.json";
};

#endif // CONFIG_H
