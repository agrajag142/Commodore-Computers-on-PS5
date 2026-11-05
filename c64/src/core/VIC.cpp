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
#include "VIC.h"
#include "Config.h"
#include "display/DisplayFactory.h"
#include "platform/PlatformManager.h"
#include <cstring>
#include <stdexcept>

static const char *TAG = "VIC";

// Changed October 2026 in the Waveshare ESP32-P4 port (sprites and picture):
// - multicolour graphics: bit pair 01 counts as background, as on the real
//   chip, so sprites set to go behind the scenery are no longer hidden by
//   it, and no false sprite-background collisions are reported
// - fine horizontal scrolling no longer shifts the map used for sprite
//   priority and collisions by up to 7 pixels against the picture
// - sprites: real sprite DMA (a started sprite runs its 21 lines even if a
//   multiplexer moves it meanwhile), priority between sprites resolved
//   before the background priority, double-width sprites no longer write
//   past the right edge into the next line (or past the end of the buffer)
// - the sprite collision interrupt fires when a collision register goes
//   from empty to non-empty, not again on every line
// - the 38 column border is 7 pixels on the left and 9 on the right
// - the picture is triple-buffered: the display only ever shows complete
//   frames, so moving sprites no longer tear

// multicolour bit pairs 00 and 01 are background, 10 and 11 foreground
static bool collArr[4] = {false, false, true, true};

VIC::VIC() {
  bitmap = nullptr;
  for (uint8_t i = 0; i < 3; i++) {
    buffers[i] = nullptr;
  }
}

void VIC::drawemptyline() {
  uint8_t colBM = vicreg[0x20] & 15;
  for (uint16_t i = 0; i < 320; i++) {
    bitmap[idx++] = colBM;
  }
}

bool VIC::shiftDy(uint8_t bgcol) {
  if ((line < deltay) || (deltay < line - 199)) {
    for (uint16_t xp = 0; xp < 40 * 8; xp++) {
      bitmap[idx++] = bgcol;
    }
    return true;
  }
  return false;
}

void VIC::shiftDx(uint8_t bgcol) {
  for (uint8_t i = 0; i < deltax; i++) {
    bitmap[idx++] = bgcol;
  }
  // keep the priority / collision map in step with the picture
  xp += deltax;
}

// 38 column mode: the border covers 7 pixels on the left, 9 on the right
void VIC::drawOnly38ColsFrame(uint16_t linestart) {
  bool only38cols = !(vicreg[0x16] & 8);
  if (only38cols) {
    uint8_t framecol = vicreg[0x20] & 15;
    for (uint8_t xp = 0; xp < 7; xp++) {
      bitmap[linestart + xp] = framecol;
    }
    for (uint16_t xp = 320 - 9; xp < 320; xp++) {
      bitmap[linestart + xp] = framecol;
    }
  }
}

void VIC::drawByteStdData(uint8_t data, uint16_t &idx, uint16_t &xp,
                          uint8_t col, uint8_t bgcol, uint8_t dx) {
  uint8_t bitval = 128;
  for (uint8_t i = 0; i < 8 - dx; i++) {
    if (data & bitval) {
      bitmap[idx++] = col;
      spritedatacoll[xp++] = true;
    } else {
      bitmap[idx++] = bgcol;
      xp++;
    }
    bitval >>= 1;
  }
}

void VIC::drawByteMCData(uint8_t data, uint16_t &idx, uint16_t &xp,
                         uint8_t *tftColArr, bool *collArr, uint8_t dx) {
  // 8 - dx pixels; dx may be odd, so the last pair may be cut in half
  for (uint8_t i = 0; i < 8 - dx; i++) {
    uint8_t bitpair = (data >> (6 - ((i >> 1) << 1))) & 0x03;
    bitmap[idx++] = tftColArr[bitpair];
    spritedatacoll[xp++] = collArr[bitpair];
  }
}

void VIC::drawidleline(uint8_t ghostbyte) {
  uint8_t col = 0;
  uint8_t bgcol = vicreg[0x20] & 15;
  uint16_t xp = 0;
  for (uint16_t i = 0; i < 40; i++) {
    drawByteStdData(ghostbyte, idx, xp, col, bgcol, 0);
  }
}

void VIC::drawStdCharModeInt(uint8_t *screenMap, uint8_t bgcol, uint8_t row,
                             uint8_t dx, uint16_t &xp, uint16_t yidx,
                             uint16_t &idx) {
  uint8_t col = colormap[yidx] & 15;
  uint8_t ch = screenMap[yidx];
  uint16_t idxch = ch << 3;
  uint8_t chardata = charset[idxch + row];
  drawByteStdData(chardata, idx, xp, col, bgcol, dx);
}

void VIC::drawStdCharMode(uint8_t *screenMap, uint8_t bgColor) {
  uint8_t bgcol = bgColor & 15;
  if (shiftDy(bgcol)) {
    return;
  }
  shiftDx(bgcol);
  drawStdCharModeInt(screenMap, bgcol, row, 0, xp, yidx++, idx);
  for (uint8_t x = 1; x < 39; x++) {
    drawStdCharModeInt(screenMap, bgcol, row, 0, xp, yidx++, idx);
  }
  drawStdCharModeInt(screenMap, bgcol, row, deltax, xp, yidx, idx);
}

void VIC::drawMCCharModeInt(uint8_t *screenMap, uint8_t bgcol,
                            uint8_t *tftColArr, uint8_t row, uint8_t dx,
                            uint16_t &xp, uint16_t yidx, uint16_t &idx) {
  uint8_t colc64 = colormap[yidx] & 15;
  uint8_t ch = screenMap[yidx];
  uint16_t idxch = ch << 3;
  uint8_t chardata = charset[idxch + row];
  if (colc64 & 8) {
    tftColArr[3] = colc64 & 7;
    drawByteMCData(chardata, idx, xp, tftColArr, collArr, dx);
  } else {
    drawByteStdData(chardata, idx, xp, colc64, bgcol, dx);
  }
}

void VIC::drawMCCharMode(uint8_t *screenMap, uint8_t bgColor, uint8_t color1,
                         uint8_t color2) {
  uint8_t bgcol = bgColor & 15;
  if (shiftDy(bgcol)) {
    return;
  }
  shiftDx(bgcol);
  uint8_t tftColArr[4];
  tftColArr[0] = bgcol;
  tftColArr[1] = color1 & 15;
  tftColArr[2] = color2 & 15;
  drawMCCharModeInt(screenMap, bgcol, tftColArr, row, 0, xp, yidx++, idx);
  for (uint8_t x = 1; x < 39; x++) {
    drawMCCharModeInt(screenMap, bgcol, tftColArr, row, 0, xp, yidx++, idx);
  }
  drawMCCharModeInt(screenMap, bgcol, tftColArr, row, deltax, xp, yidx, idx);
}

void VIC::drawExtBGColCharModeInt(uint8_t *screenMap, uint8_t *bgColArr,
                                  uint8_t row, uint8_t dx, uint16_t &xp,
                                  uint16_t yidx, uint16_t &idx) {
  uint8_t col = colormap[yidx] & 15;
  uint8_t ch = screenMap[yidx];
  uint8_t ch6bits = ch & 0x3f;
  uint8_t bgcol = bgColArr[ch >> 6] & 15;
  uint16_t idxch = ch6bits << 3;
  uint8_t chardata = charset[idxch + row];
  drawByteStdData(chardata, idx, xp, col, bgcol, dx);
}

void VIC::drawExtBGColCharMode(uint8_t *screenMap, uint8_t *bgColArr) {
  uint8_t bgcol0 = bgColArr[0];
  if (shiftDy(bgcol0)) {
    return;
  }
  shiftDx(bgcol0);
  drawExtBGColCharModeInt(screenMap, bgColArr, row, 0, xp, yidx++, idx);
  for (uint8_t x = 1; x < 39; x++) {
    drawExtBGColCharModeInt(screenMap, bgColArr, row, 0, xp, yidx++, idx);
  }
  drawExtBGColCharModeInt(screenMap, bgColArr, row, deltax, xp, yidx, idx);
}

void VIC::drawMCBitmapModeInt(uint8_t *multicolorBitmap, uint8_t *colorMap1,
                              uint8_t *tftColArr, uint16_t cidx, uint16_t mcidx,
                              uint8_t row, uint8_t dx, uint16_t &xp,
                              uint16_t &idx) {
  uint8_t color1 = colorMap1[cidx];
  uint8_t color2 = colormap[cidx];
  tftColArr[1] = (color1 >> 4) & 0x0f;
  tftColArr[2] = color1 & 0x0f;
  tftColArr[3] = color2 & 0x0f;
  uint8_t data = multicolorBitmap[mcidx + row];
  drawByteMCData(data, idx, xp, tftColArr, collArr, dx);
}

void VIC::drawMCBitmapMode(uint8_t *multicolorBitmap, uint8_t *colorMap1,
                           uint8_t backgroundColor) {
  uint8_t tftColArr[4];
  tftColArr[0] = backgroundColor & 0x0f;
  if (shiftDy(tftColArr[0])) {
    return;
  }
  shiftDx(tftColArr[0]);
  uint16_t mcidx = (y * 40) << 3;
  drawMCBitmapModeInt(multicolorBitmap, colorMap1, tftColArr, yidx++, mcidx,
                      row, 0, xp, idx);
  mcidx += 8;
  for (uint8_t x = 1; x < 39; x++) {
    drawMCBitmapModeInt(multicolorBitmap, colorMap1, tftColArr, yidx++, mcidx,
                        row, 0, xp, idx);
    mcidx += 8;
  }
  drawMCBitmapModeInt(multicolorBitmap, colorMap1, tftColArr, yidx, mcidx, row,
                      deltax, xp, idx);
}

void VIC::drawStdBitmapModeInt(uint8_t *hiresBitmap, uint8_t *colorMap,
                               uint16_t hiidx, uint16_t &colidx, uint8_t row,
                               uint8_t dx, uint16_t &xp, uint16_t &idx) {
  uint8_t color = colorMap[colidx++];
  uint8_t colorfg = (color & 0xf0) >> 4;
  uint8_t colorbg = color & 0x0f;
  uint8_t data = hiresBitmap[hiidx + row];
  drawByteStdData(data, idx, xp, colorfg, colorbg, dx);
}

void VIC::drawStdBitmapMode(uint8_t *hiresBitmap, uint8_t *colorMap) {
  // todo: background color is specific for each "tile"
  if (shiftDy(0)) {
    return;
  }
  shiftDx(0);
  uint16_t hiidx = (y * 40) << 3;
  drawStdBitmapModeInt(hiresBitmap, colorMap, hiidx, yidx, row, 0, xp, idx);
  hiidx += 8;
  for (uint8_t x = 1; x < 39; x++) {
    drawStdBitmapModeInt(hiresBitmap, colorMap, hiidx, yidx, row, 0, xp, idx);
    hiidx += 8;
  }
  drawStdBitmapModeInt(hiresBitmap, colorMap, hiidx, yidx, row, deltax, xp,
                       idx);
}

// Bytes the VIC reads from its 16 KB bank. In banks 0 and 2 it sees the
// character ROM at $1000-$1FFF instead of RAM.
uint8_t VIC::vicRead(uint16_t addr) {
  addr &= 0x3fff;
  if (!(vicmem & 0x4000) && ((addr & 0x3000) == 0x1000)) {
    return chrom[addr & 0x0fff];
  }
  return ram[vicmem + addr];
}

// Draw one line of every sprite whose DMA is running.
// Sprite 0 has the highest priority. Sprites are drawn 0 first, and a pixel
// already taken by a higher priority sprite stays taken even when that
// sprite is hidden behind the background there - as on the real chip.
void VIC::drawSprites() {
  uint8_t multicolorreg = vicreg[0x1c];
  uint8_t spritesdoublex = vicreg[0x1d];
  uint8_t bgprio = vicreg[0x1b];
  uint8_t mccol[4] = {0, (uint8_t)(vicreg[0x25] & 0x0f), 0,
                      (uint8_t)(vicreg[0x26] & 0x0f)};
  bool only38cols = !(vicreg[0x16] & 8);
  int16_t low = only38cols ? 7 : 0;
  int16_t high = only38cols ? 320 - 9 : 320;
  uint8_t *row = bitmap + line * 320;
  uint8_t prevcollss = vicreg[0x1e];
  uint8_t prevcollsd = vicreg[0x1f];
  memset(spritespritecoll, 0, sizeof(spritespritecoll));
  for (uint8_t nr = 0; nr < 8; nr++) {
    if (!sprdma[nr]) {
      continue;
    }
    uint8_t bit = 1 << nr;
    int16_t x = vicreg[0x00 + nr * 2] - 24;
    if (vicreg[0x10] & bit) {
      x += 256;
    }
    if (x >= high) {
      continue;
    }
    uint32_t data = sprdata[nr]; // fetched at the end of the previous line
    if (data == 0) {
      continue;
    }
    // the sprite's pixels for this line: 0xff = transparent
    uint8_t pix[48];
    uint8_t w = 0;
    uint8_t rep = (spritesdoublex & bit) ? 2 : 1;
    if (multicolorreg & bit) {
      mccol[2] = vicreg[0x27 + nr] & 0x0f;
      for (int8_t sh = 22; sh >= 0; sh -= 2) {
        uint8_t pair = (data >> sh) & 3;
        uint8_t c = pair ? mccol[pair] : 0xff;
        for (uint8_t k = 0; k < 2 * rep; k++) {
          pix[w++] = c;
        }
      }
    } else {
      uint8_t col = vicreg[0x27 + nr] & 0x0f;
      for (int8_t sh = 23; sh >= 0; sh--) {
        uint8_t c = ((data >> sh) & 1) ? col : 0xff;
        for (uint8_t k = 0; k < rep; k++) {
          pix[w++] = c;
        }
      }
    }
    bool behind = bgprio & bit;
    for (uint8_t i = 0; i < w; i++) {
      int16_t sx = x + i;
      if (sx < low) {
        continue;
      }
      if (sx >= high) {
        break;
      }
      uint8_t c = pix[i];
      if (c == 0xff) {
        continue;
      }
      uint8_t occ = spritespritecoll[sx];
      if (occ) {
        // sprite - sprite collision
        vicreg[0x1e] |= occ | bit;
      }
      spritespritecoll[sx] = occ | bit;
      bool fg = spritedatacoll[sx];
      if (fg) {
        // sprite - data collision
        vicreg[0x1f] |= bit;
      }
      if (occ || (fg && behind)) {
        continue;
      }
      row[sx] = c;
    }
  }
  // the interrupt fires when a collision register stops being empty
  if ((prevcollsd == 0) && (vicreg[0x1f] != 0)) {
    vicreg[0x19] |= (vicreg[0x1a] & 2) ? 0x82 : 0x02;
  }
  if ((prevcollss == 0) && (vicreg[0x1e] != 0)) {
    vicreg[0x19] |= (vicreg[0x1a] & 4) ? 0x84 : 0x04;
  }
}

// Sprite DMA, run at the end of every raster line (0-311).
// A sprite starts when it is enabled, its DMA is off and its Y coordinate
// equals the raster line; from the next line on it shows one row of its data
// per line (two with Y expansion) until all 21 rows have been shown. Moving
// or disabling the sprite meanwhile does not cut it short, as on the real
// chip, which is what sprite multiplexers rely on.
void VIC::updateSpriteDMA(uint16_t rline) {
  uint8_t enabled = vicreg[0x15];
  uint8_t yexp = vicreg[0x17];
  for (uint8_t nr = 0; nr < 8; nr++) {
    uint8_t bit = 1 << nr;
    if (sprdma[nr]) {
      if (yexp & bit) {
        if (sprexpflop[nr]) {
          sprmc[nr] += 3;
        }
        sprexpflop[nr] = !sprexpflop[nr];
      } else {
        sprmc[nr] += 3;
        sprexpflop[nr] = true;
      }
      if (sprmc[nr] >= 63) {
        sprdma[nr] = false;
      }
    }
    // On the real chip the DMA of a finished sprite is switched off early
    // in the line showing its last row, and the Y check comes later in that
    // same line - so a sprite moved to start right below itself (stacked
    // sprites, multiplexers) starts again without a gap.
    if (!sprdma[nr] && (enabled & bit) &&
               (vicreg[0x01 + nr * 2] == (uint8_t)(rline & 0xff))) {
      sprdma[nr] = true;
      sprmc[nr] = 0;
      sprexpflop[nr] = false;
    }
    // Fetch the data for the next line now, as the real chip does at the
    // end of the line (pointer and three bytes). Games often switch the
    // pointer a line or two before a sprite ends, counting on this; reading
    // it later showed a stray line of the next sprite's graphics.
    if (sprdma[nr]) {
      uint8_t ptr = ram[screenmemstart + 1016 + nr];
      uint16_t addr = (uint16_t)(ptr * 64 + sprmc[nr]);
      sprdata[nr] = ((uint32_t)vicRead(addr) << 16) |
                    ((uint32_t)vicRead(addr + 1) << 8) | vicRead(addr + 2);
    }
  }
}

// Called by the emulator when a frame has been drawn completely: hand it to
// the display and continue in a free buffer.
void VIC::frameDone() {
  uint8_t prev = readyBuf.exchange(backBuf | 0x80, std::memory_order_acq_rel);
  backBuf = prev & 0x7f;
  bitmap = buffers[backBuf];
}

void VIC::initVarsAndRegs() {
  for (uint8_t i = 0; i < 0x40; i++) {
    vicreg[i] = 0;
  }
  vicreg[0x11] = 0x1b;
  vicreg[0x16] = 0xc8;
  vicreg[0x18] = 0x15;
  vicreg[0x19] = 0x71;
  vicreg[0x1a] = 0xf0;

  cntRefreshs.store(0, std::memory_order_release);
  vicmem = 0;
  bitmapstart = 0x2000;
  screenmemstart = 1024;
  rasterline = 0;
  charset = chrom;
  vertborder = true;
  doiactive[0] = false;
  doiactive[1] = false;
  for (uint8_t i = 0; i < 8; i++) {
    sprdma[i] = false;
    sprmc[i] = 0;
    sprexpflop[i] = false;
    sprdata[i] = 0;
  }
}

void VIC::init(uint8_t *ram, const uint8_t *charrom) {
  if (bitmap != nullptr) {
    // init method must be called only once
    return;
  }
  this->ram = ram;
  this->chrom = charrom;

  // allocate bitmap memory to be transfered to LCD
  // three pictures: one being drawn, one finished and waiting, one shown
  for (uint8_t i = 0; i < 3; i++) {
#ifdef USE_PSRAM
    if (psramFound()) {
      buffers[i] = (uint8_t *)ps_malloc(320 * 200);
    } else {
      buffers[i] = new uint8_t[320 * 200];
    }
#else
    buffers[i] = new uint8_t[320 * 200];
#endif
    memset(buffers[i], 0, 320 * 200);
  }
  backBuf = 0;
  readyBuf.store(1, std::memory_order_release);
  frontBuf = 2;
  bitmap = buffers[backBuf];

  // init display
  PlatformManager::getInstance().log(LOG_INFO, TAG, "init display");
  display = Display::create();
  display->init();

  // div init
  colormap = new uint8_t[1024]();
  initVarsAndRegs();
}

void VIC::refresh() {
  dispOverlayInfo();
  // take the newest finished picture, if there is one
  if (readyBuf.load(std::memory_order_acquire) & 0x80) {
    uint8_t prev = readyBuf.exchange(frontBuf, std::memory_order_acq_rel);
    frontBuf = prev & 0x7f;
  }
  display->drawBitmap(buffers[frontBuf], vicreg, this->ram);
  display->drawFrame(vicreg[0x20] & 15);
  cntRefreshs.fetch_add(1, std::memory_order_release);
}

void VIC::drawOverlay(uint8_t doiidx) {
  if (!doiactive[doiidx]) {
    return;
  }
  uint8_t cline = line >> 3;
  if ((cline < doistarty[doiidx]) ||
      (cline >= doistarty[doiidx] + doih[doiidx])) {
    return;
  }
  uint8_t crow = doistartx[doiidx];
  uint8_t bgcol = doibgcol[doiidx] & 15;
  uint8_t fgcol = doicol[doiidx] & 15;
  uint16_t idxdoi = line * 320 + crow * 8;
  uint16_t tidxdoi = crow + cline * 40;
  uint8_t rowdoi = line & 7;
  for (uint8_t x = 0; x < doiw[doiidx]; x++) {
    uint8_t ch = doitextmap[tidxdoi++];
    uint16_t idxch = ch << 3;
    uint8_t data = chrom[idxch + rowdoi];
    uint8_t bitval = 128;
    for (uint8_t i = 0; i < 8; i++) {
      if (data & bitval) {
        bitmap[idxdoi++] = fgcol;
      } else {
        bitmap[idxdoi++] = bgcol;
      }
      bitval >>= 1;
    }
  }
}

uint8_t VIC::nextRasterline() {
  // end of the line just finished
  updateSpriteDMA(rasterline);
  rasterline++;
  uint8_t d011 = vicreg[0x11];
  if (rasterline > 311) {
    rasterline = 0;
  } else if (rasterline == 49) {
    denbadline = (d011 & 0x10) ? true : false;
  }
  bool rsel = d011 & 8;
  if ((rasterline == 51) && rsel && denbadline) {
    vertborder = false;
  } else if ((rasterline == 55) && (!rsel) && denbadline) {
    vertborder = false;
  } else if ((rasterline == 247) && (!rsel)) {
    vertborder = true;
  } else if ((rasterline == 251) && rsel) {
    vertborder = true;
  }
  uint8_t raster8 = (rasterline >= 256) ? 0x80 : 0;
  uint8_t raster7 = (rasterline & 0xff);
  vicreg[0x12] = raster7;
  if ((latchd012 == raster7) && ((latchd011 & 0x80) == raster8)) {
    if (vicreg[0x1a] & 1) {
      vicreg[0x19] |= 0x81;
    } else {
      vicreg[0x19] |= 0x01;
    }
  }
  // calculate cycles used by VIC
  if (!denbadline) {
    return 0;
  }
  uint8_t viccycles = 0;
  // badline?
  if (((vicreg[0x11] & 7) == (rasterline & 7)) && (rasterline >= 0x30) &&
      (rasterline <= 0xf7)) {
    viccycles = 40;
    caccbadlinecnt = 8;
  }
  // sprites being fetched on this line
  for (uint8_t nr = 0; nr < 8; nr++) {
    if (sprdma[nr]) {
      viccycles += 2;
    }
  }
  if (viccycles > 54) {
    viccycles = 54;
  }
  return viccycles;
}

void VIC::drawRasterline() {
  if ((rasterline >= 51) && (rasterline < 251)) {
    line = rasterline - 51;
    idx = line * 320;
    if (!vertborder) {
      uint8_t d011 = vicreg[0x11];
      deltay = (d011 & 7) - 3;
      bool ecm = d011 & 64;
      if ((caccbadlinecnt > 0) || (deltay < line - 199)) {
        caccbadlinecnt--;
        memset(spritedatacoll, false, sizeof(bool) * sizeof(spritedatacoll));
        uint8_t d016 = vicreg[0x16];
        deltax = d016 & 7;
        bool bmm = d011 & 32;
        bool mcm = d016 & 16;
        // variables for the draw* methods
        int16_t dl = (int16_t)line - deltay;
        if (dl < 0) {
          dl = 0;
        }
        row = dl & 7;
        y = dl >> 3;
        yidx = y * 40;
        xp = 0;
        if (ecm && (bmm || mcm)) {
          // illegal VIC-II modes: the real chip shows black
          for (uint16_t i = 0; i < 320; i++) {
            bitmap[idx++] = 0;
          }
        } else if (bmm) {
          if (mcm) {
            drawMCBitmapMode(ram + bitmapstart, ram + screenmemstart,
                             vicreg[0x21]);
          } else {
            drawStdBitmapMode(ram + bitmapstart, ram + screenmemstart);
          }
        } else {
          if ((!ecm) && (!mcm)) {
            drawStdCharMode(ram + screenmemstart, vicreg[0x21]);
          } else if ((!ecm) && mcm) {
            drawMCCharMode(ram + screenmemstart, vicreg[0x21], vicreg[0x22],
                           vicreg[0x23]);
          } else if (ecm && (!mcm)) {
            uint8_t bgColArr[] = {vicreg[0x21], vicreg[0x22], vicreg[0x23],
                                  vicreg[0x24]};
            drawExtBGColCharMode(ram + screenmemstart, bgColArr);
          }
        }
      } else {
        uint8_t ghostbyte = ecm ? ram[vicmem + 0x39ff] : ram[vicmem + 0x3fff];
        memset(spritedatacoll, false, sizeof(spritedatacoll));
        drawidleline(ghostbyte);
      }
      drawOnly38ColsFrame(line * 320);
      drawSprites();
      // draw overlay
      drawOverlay(0);
      drawOverlay(1);
    } else {
      drawemptyline();
    }
  }
}

void VIC::drawDOIBox(uint8_t *box, uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                     uint8_t fgcol, uint8_t bgcol, uint16_t duration,
                     uint8_t doiidx) {
  doistartx[doiidx] = x;
  doistarty[doiidx] = y;
  doiw[doiidx] = w;
  doih[doiidx] = h;
  doicol[doiidx] = fgcol;
  doibgcol[doiidx] = bgcol;
  doiduration[doiidx] = ((int64_t)duration) * 1000 * 1000;
  for (uint8_t yi = 0; yi < h; yi++) {
    const uint8_t *sourceLine = &box[yi * w];
    uint8_t *destLine = &doitextmap[(y + yi) * 40 + x];
    std::copy(sourceLine, sourceLine + w, destLine);
  }
  doistarttime[doiidx] = PlatformManager::getInstance().getTimeUS();
  doiactive[doiidx] = true;
}

void VIC::dispOverlayInfoInt(uint8_t doiidx) {
  if (!doiactive[doiidx]) {
    return;
  }
  int64_t acttime = PlatformManager::getInstance().getTimeUS();
  if (acttime - doistarttime[doiidx] >= doiduration[doiidx]) {
    doiactive[doiidx] = false;
    return;
  }
}

void VIC::dispOverlayInfo() {
  dispOverlayInfoInt(1);
  dispOverlayInfoInt(0);
}

// ---------------------------------------------------------------- state
// Added for the Waveshare ESP32-P4 port (SID player screen): everything the
// chip remembers from one frame to the next. The three picture buffers and
// their bookkeeping are left alone - the display task owns part of them.
#include "StateBuf.h"

void VIC::saveState(StateWriter &w) const {
  w.put(sprdma);
  w.put(sprmc);
  w.put(sprexpflop);
  w.put(sprdata);
  w.put(startbyte);
  w.put(vertborder);
  w.put(denbadline);
  w.put(caccbadlinecnt);
  w.put(line);
  w.put(idx);
  w.put(y);
  w.put(row);
  w.put(yidx);
  w.put(xp);
  w.put(deltay);
  w.put(deltax);
  w.bytes(colormap, 1024);
  w.put(charset);
  w.put(vicreg);
  w.put(latchd011);
  w.put(latchd012);
  w.put(vicmem);
  w.put(bitmapstart);
  w.put(screenmemstart);
  w.put(rasterline);
}

void VIC::loadState(StateReader &r) {
  r.get(sprdma);
  r.get(sprmc);
  r.get(sprexpflop);
  r.get(sprdata);
  r.get(startbyte);
  r.get(vertborder);
  r.get(denbadline);
  r.get(caccbadlinecnt);
  r.get(line);
  r.get(idx);
  r.get(y);
  r.get(row);
  r.get(yidx);
  r.get(xp);
  r.get(deltay);
  r.get(deltax);
  r.bytes(colormap, 1024);
  r.get(charset);
  r.get(vicreg);
  r.get(latchd011);
  r.get(latchd012);
  r.get(vicmem);
  r.get(bitmapstart);
  r.get(screenmemstart);
  r.get(rasterline);
  doiactive[0] = false;
  doiactive[1] = false;
}
