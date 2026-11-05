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
#ifndef VIC_H
#define VIC_H

#include "display/DisplayDriver.h"
#include <atomic>
#include <cstdint>

struct StateWriter;
struct StateReader;

class VIC {
private:
  uint8_t *ram;
  uint8_t *bitmap;
  uint8_t spritespritecoll[320];
  bool spritedatacoll[320];

  // sprite DMA state (see updateSpriteDMA)
  bool sprdma[8];
  uint8_t sprmc[8];
  bool sprexpflop[8];
  uint32_t sprdata[8]; // the three data bytes for the line being drawn

  // triple buffer: the emulator draws into buffers[backBuf], the display
  // shows buffers[frontBuf], readyBuf is the newest finished picture
  // (bit 7 set = not shown yet)
  uint8_t *buffers[3];
  uint8_t backBuf;
  uint8_t frontBuf;
  std::atomic<uint8_t> readyBuf;
  uint8_t startbyte;
  bool vertborder;
  bool denbadline;
  uint8_t caccbadlinecnt;

  // doi
  uint8_t doitextmap[1000];
  int64_t doistarttime[2];
  int64_t doiduration[2];
  uint8_t doicol[2];
  uint8_t doibgcol[2];
  uint8_t doistartx[2];
  uint8_t doistarty[2];
  uint8_t doiw[2];
  uint8_t doih[2];

  // members to reduce repetition of code
  uint8_t line;
  uint16_t idx;
  uint8_t y;
  uint8_t row;
  uint16_t yidx;
  uint16_t xp;
  int8_t deltay;
  uint8_t deltax;

  inline void drawByteStdData(uint8_t data, uint16_t &idx, uint16_t &xp,
                              uint8_t col, uint8_t bgcol, uint8_t dx)
      __attribute__((always_inline));
  inline void drawByteMCData(uint8_t data, uint16_t &idx, uint16_t &xp,
                             uint8_t *tftColArr, bool *collArr, uint8_t dx)
      __attribute__((always_inline));
  void drawemptyline();
  void drawidleline(uint8_t ghostbyte);
  inline bool shiftDy(uint8_t bgcol) __attribute__((always_inline));
  inline void shiftDx(uint8_t bgcol) __attribute__((always_inline));
  inline void drawOnly38ColsFrame(uint16_t linestart)
      __attribute__((always_inline));
  inline void drawStdCharModeInt(uint8_t *screenMap, uint8_t bgcol, uint8_t row,
                                 uint8_t dx, uint16_t &xp, uint16_t idxmap,
                                 uint16_t &idx) __attribute__((always_inline));
  void drawStdCharMode(uint8_t *screenMap, uint8_t bgColor);
  inline void drawExtBGColCharModeInt(uint8_t *screenMap, uint8_t *bgColArr,
                                      uint8_t row, uint8_t dx, uint16_t &xp,
                                      uint16_t idxmap, uint16_t &idx)
      __attribute__((always_inline));
  void drawExtBGColCharMode(uint8_t *screenMap, uint8_t *bgColArr);
  inline void drawMCCharModeInt(uint8_t *screenMap, uint8_t bgcol,
                                uint8_t *tftColArr, uint8_t row, uint8_t dx,
                                uint16_t &xp, uint16_t idxmap, uint16_t &idx)
      __attribute__((always_inline));
  void drawMCCharMode(uint8_t *screenMap, uint8_t bgColor1, uint8_t bgColor2,
                      uint8_t bgColor3);
  inline void drawMCBitmapModeInt(uint8_t *multicolorBitmap, uint8_t *colorMap1,
                                  uint8_t *tftColArr, uint16_t cidx,
                                  uint16_t mcidx, uint8_t row, uint8_t dx,
                                  uint16_t &xp, uint16_t &idx)
      __attribute__((always_inline));
  void drawMCBitmapMode(uint8_t *multicolorBitmap, uint8_t *colorMap1,
                        uint8_t backgroundColor);
  inline void drawStdBitmapModeInt(uint8_t *hiresBitmap, uint8_t *colorMap,
                                   uint16_t hiidx, uint16_t &colidx,
                                   uint8_t row, uint8_t dx, uint16_t &xp,
                                   uint16_t &idx)
      __attribute__((always_inline));
  void drawStdBitmapMode(uint8_t *hiresBitmap, uint8_t *colorMap);
  inline uint8_t vicRead(uint16_t addr) __attribute__((always_inline));
  void drawSprites();
  void updateSpriteDMA(uint16_t rline);
  inline void checkFrameColor() __attribute__((always_inline));
  void dispOverlayInfoInt(uint8_t doiidx);
  void dispOverlayInfo();
  void drawOverlay(uint8_t doiidx);

public:
  // public only for CYD special logic
  DisplayDriver *display;

  // profiling info
  std::atomic<uint8_t> cntRefreshs;

  uint8_t *colormap;
  const uint8_t *charset;
  const uint8_t *chrom;

  uint8_t vicreg[0x40];
  uint8_t latchd011;
  uint8_t latchd012;
  uint16_t vicmem;
  uint16_t bitmapstart;
  uint16_t screenmemstart;
  uint16_t rasterline;

  bool doiactive[2];

  VIC();
  void initVarsAndRegs();
  void init(uint8_t *ram, const uint8_t *charrom);
  void refresh();
  uint8_t nextRasterline();
  void drawRasterline();
  void frameDone();
  void drawDOIBox(uint8_t *box, uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                  uint8_t fgcol, uint8_t bgcol, uint16_t duration,
                  uint8_t doiidx);
  // the chip's state, for C64Sys::saveState (not the picture buffers)
  void saveState(StateWriter &w) const;
  void loadState(StateReader &r);
};
#endif // VIC_H
