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
// PS5 port: no .config.json (and no JSON library). The emulator asks for the
// same three things as on the other ports; there is nothing to configure.
#ifndef FILECONFIG_H
#define FILECONFIG_H

#include "JoystickOnlyTextKeycode.h"
#include "fs/FileDriver.h"
#include <string>
#include <vector>

class FileConfig {
public:
  static void loadConfig(FileDriver &, const std::string &) {}
  static std::string getAutostartGame() { return std::string(); }
  static std::vector<JoystickOnlyTextKeycode> getJoystickOnlyKeycodes() {
    return std::vector<JoystickOnlyTextKeycode>();
  }
};

#endif // FILECONFIG_H
