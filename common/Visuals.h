// Visuals.h
// The music players' full-screen visuals (Triangle in the SID and MOD
// players), shared by the C64 and Amiga apps. Each one follows the music:
// the channels' levels and notes, new notes, and the sound itself.
//
//   COPPER BARS   the Amiga's raster bars, one per channel, swinging on a sine,
//                 brighter and wider with their channel; a starfield behind, a
//                 sine scroller with the tune's name, all mirrored in water
//   GLENZ         see-through faceted vector objects (a glenz cube round a
//                 solid star), lit, turning with the music, over a warp
//                 starfield; each channel a glowing orbit
//   PLASMA        a plasma whose colours follow the notes, the sound as a
//                 glowing oscilloscope ring and line, the channels as rising
//                 sparks
//
// Draws into the app's 1920 x 1080 TV picture (0xAABBGGRR). The app gives
// its own font (Font) for the scroller and the captions.

#ifndef VISUALS_H
#define VISUALS_H

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct VisFont {
  // draw one character, scale x scale per font pixel
  void (*glyph)(uint32_t *frame, int x, int y, char c, int scale, uint32_t col);
  int w, h; // a character's size in font pixels
};

struct VisInput {
  int channels;          // 1-8
  float level[8];        // 0..1
  float pitch[8];        // 0..1 (low to high note), 0 = none
  bool hit[8];           // a note started this frame
  const int16_t *wave;   // the sound just played (oldest first)
  int waveN;
  const char *title, *author, *info; // the tune; info: song / time etc.
  float progress;        // 0..1, or < 0 when not known
  bool paused;
  const char *caption;   // e.g. "SID MUSIC" / "MOD PLAYER"
};

class Visuals {
public:
  enum Mode { COPPER, GLENZ, PLASMA, COUNT };
  static const char *name(int m) {
    static const char *n[COUNT] = {"COPPER BARS", "GLENZ VECTORS", "PLASMA"};
    return n[m % COUNT];
  }

  // One frame: dt in seconds since the last
  void draw(uint32_t *frame, int mode, const VisInput &in, const VisFont &font, float dt) {
    fb = frame;
    if (dt > 0.1f) dt = 0.1f;
    t += dt;
    // the music's energy, smoothed: a quick rise, a slower fall
    float sum = 0;
    for (int c = 0; c < in.channels && c < 8; c++) {
      float l = in.level[c];
      if (in.hit[c]) kick[c] = 1.0f;
      kick[c] *= expf(-dt * 6.0f);
      lvl[c] += (l - lvl[c]) * (l > lvl[c] ? 0.5f : 0.12f);
      sum += lvl[c];
    }
    float e = in.channels ? sum / in.channels : 0;
    energy += (e - energy) * (e > energy ? 0.4f : 0.06f);
    beat = 0;
    for (int c = 0; c < in.channels && c < 8; c++) beat = beat > kick[c] ? beat : kick[c];
    if (in.paused) energy *= 0.5f, beat = 0;

    switch (mode % COUNT) {
    case COPPER: copper(in, font, dt); break;
    case GLENZ: glenz(in, dt); break;
    default: plasma(in, dt); break;
    }
    overlay(mode, in, font);
  }

private:
  static const int W = 1920, H = 1080;
  uint32_t *fb = nullptr;
  float t = 0, energy = 0, beat = 0, scroll = 0, warp = 0;
  float lvl[8] = {}, kick[8] = {};
  float angA = 0, angB = 0, angC = 0;

  // ---------------------------------------------------------------- colours
  static uint32_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xff000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
  }
  // a hue (0..1) at full colour, brightness v (0..1+)
  static void hue(float h, float v, int &r, int &g, int &b) {
    h -= floorf(h);
    float x = h * 6;
    int i = (int)x;
    float f = x - i;
    float c[6][3] = {{1, f, 0}, {1 - f, 1, 0}, {0, 1, f}, {0, 1 - f, 1}, {f, 0, 1}, {1, 0, 1 - f}};
    r = (int)(c[i % 6][0] * 255 * v);
    g = (int)(c[i % 6][1] * 255 * v);
    b = (int)(c[i % 6][2] * 255 * v);
  }
  // add light to a pixel (saturating)
  inline void add(int x, int y, int r, int g, int b) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    uint32_t &p = fb[y * W + x];
    int pr = (int)(p & 0xff) + r, pg = (int)((p >> 8) & 0xff) + g, pb = (int)((p >> 16) & 0xff) + b;
    p = rgb(pr, pg, pb);
  }
  // mix a colour over a pixel, a: 0..256
  inline void mix(uint32_t *p, int r, int g, int b, int a) {
    int pr = (int)(*p & 0xff), pg = (int)((*p >> 8) & 0xff), pb = (int)((*p >> 16) & 0xff);
    *p = rgb(pr + ((r - pr) * a >> 8), pg + ((g - pg) * a >> 8), pb + ((b - pb) * a >> 8));
  }

  // ---------------------------------------------------------------- shapes
  // a soft light: brightest in the middle, nothing at radius rad
  void glow(float cx, float cy, float rad, int r, int g, int b, float a) {
    int x0 = (int)(cx - rad), x1 = (int)(cx + rad), y0 = (int)(cy - rad), y1 = (int)(cy + rad);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    float inv = 1.0f / (rad * rad);
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        float d = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) * inv;
        if (d >= 1) continue;
        float k = (1 - d) * (1 - d) * a;
        add(x, y, (int)(r * k), (int)(g * k), (int)(b * k));
      }
  }

  // a filled triangle: solid (a >= 256) or see-through (mixed) or added light (additive)
  void tri(float ax, float ay, float bx, float by, float cx, float cy, int r, int g, int b, int a, bool additive) {
    if (ay > by) swap(ax, ay, bx, by);
    if (ay > cy) swap(ax, ay, cx, cy);
    if (by > cy) swap(bx, by, cx, cy);
    if (cy <= ay) return;
    int yStart = (int)ceilf(ay), yEnd = (int)ceilf(cy);
    if (yStart < 0) yStart = 0;
    if (yEnd > H) yEnd = H;
    for (int y = yStart; y < yEnd; y++) {
      float fy = (float)y;
      float xl = ax + (cx - ax) * (fy - ay) / (cy - ay);
      float xr = fy < by ? (by > ay ? ax + (bx - ax) * (fy - ay) / (by - ay) : ax)
                         : (cy > by ? bx + (cx - bx) * (fy - by) / (cy - by) : bx);
      if (xl > xr) {
        float tmp = xl;
        xl = xr;
        xr = tmp;
      }
      int x0 = (int)ceilf(xl), x1 = (int)ceilf(xr);
      if (x0 < 0) x0 = 0;
      if (x1 > W) x1 = W;
      uint32_t *p = fb + y * W + x0;
      if (additive) {
        for (int x = x0; x < x1; x++, p++) {
          int pr = (int)(*p & 0xff) + (r * a >> 8), pg = (int)((*p >> 8) & 0xff) + (g * a >> 8),
              pb = (int)((*p >> 16) & 0xff) + (b * a >> 8);
          *p = rgb(pr, pg, pb);
        }
      } else if (a >= 256) {
        uint32_t c = rgb(r, g, b);
        for (int x = x0; x < x1; x++) *p++ = c;
      } else {
        for (int x = x0; x < x1; x++) mix(p++, r, g, b, a);
      }
    }
  }
  static void swap(float &ax, float &ay, float &bx, float &by) {
    float tx = ax, ty = ay;
    ax = bx;
    ay = by;
    bx = tx;
    by = ty;
  }

  // a line of light, about w pixels wide
  void line(float x0, float y0, float x1, float y1, float w, int r, int g, int b) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    int n = (int)(len / 1.5f) + 1;
    for (int i = 0; i <= n; i++) {
      float x = x0 + dx * i / n, y = y0 + dy * i / n;
      int hw = (int)(w / 2);
      for (int oy = -hw; oy <= hw; oy++)
        for (int ox = -hw; ox <= hw; ox++) add((int)x + ox, (int)y + oy, r / 3, g / 3, b / 3);
    }
  }

  void text(const VisFont &f, int x, int y, const char *s, int scale, uint32_t col) {
    for (; *s; s++, x += f.w * scale) f.glyph(fb, x, y, *s, scale, col);
  }
  static int textW(const VisFont &f, const char *s, int scale) { return (int)strlen(s) * f.w * scale; }

  // ---------------------------------------------------------------- COPPER BARS
  void copper(const VisInput &in, const VisFont &font, float dt) {
    const int water = 820; // the water line
    // the sky: deep blue at the top, purple at the horizon
    for (int y = 0; y < water; y++) {
      float k = (float)y / water;
      uint32_t c = rgb((int)(10 + 40 * k), (int)(6 + 10 * k), (int)(30 + 50 * k));
      uint32_t *p = fb + y * W;
      for (int x = 0; x < W; x++) p[x] = c;
    }
    // stars drifting left, three layers
    for (int i = 0; i < 260; i++) {
      uint32_t h = (uint32_t)(i + 7) * 2654435761u;
      int layer = i % 3;
      float sp = (40 + layer * 70) * (1 + energy * 3);
      float x = fmodf((float)(h >> 7 & 0xffff) * 0.04f * W / 2621.0f - t * sp, (float)W);
      if (x < 0) x += W;
      int y = (int)((h >> 3) % (unsigned)(water - 20));
      int v = 90 + layer * 60;
      add((int)x, y, v, v, v + 20);
      if (layer == 2) add((int)x + 1, y, v / 2, v / 2, v / 2);
    }
    // the bars, back to front: each channel's colour, a chrome gradient
    const int nb = 9; // (each channel's colour, round and round)
    for (int i = 0; i < nb; i++) {
      int c = i % (in.channels > 0 ? in.channels : 1);
      float l = lvl[c], k = kick[c];
      float y = 400 + 330 * sinf(t * 1.1f + i * 0.71f) * cosf(t * 0.37f + i * 0.29f);
      float half = 22 + 26 * l + 18 * k;
      int r, g, b;
      hue(0.62f + i * 0.11f + in.pitch[c] * 0.25f, 1.0f, r, g, b);
      for (int dy = -(int)half; dy <= (int)half; dy++) {
        int yy = (int)y + dy;
        if (yy < 0 || yy >= water) continue;
        float u = (float)dy / half;               // -1..1
        float sh = 0.25f + 0.75f * cosf(u * 1.5708f);
        float spec = expf(-(u + 0.35f) * (u + 0.35f) * 40.0f) * (0.6f + 0.4f * k); // a bright highlight
        uint32_t col = rgb((int)(r * sh + 255 * spec), (int)(g * sh + 255 * spec), (int)(b * sh + 255 * spec));
        uint32_t *p = fb + yy * W;
        for (int x = 0; x < W; x++) p[x] = col;
      }
    }
    // the scroller: the tune's name, each letter on a sine, a rainbow running through
    char msg[400];
    snprintf2(msg, sizeof(msg), in);
    int scale = font.h >= 16 ? 5 : 8, cw = font.w * scale, ch = font.h * scale;
    int len = (int)strlen(msg);
    scroll += dt * (260 + 200 * energy);
    float total = (float)(len * cw);
    float off = fmodf(scroll, total + W);
    for (int i = 0; i < len; i++) {
      float x = W - off + i * cw;
      if (x < -cw || x > W) continue;
      float y = 640 + 70 * sinf(x * 0.006f + t * 3.0f) - ch / 2;
      int r, g, b;
      hue(x * 0.0006f + t * 0.2f, 1.0f, r, g, b);
      font.glyph(fb, (int)x + 6, (int)y + 6, msg[i], scale, rgb(0, 0, 0)); // the shadow
      font.glyph(fb, (int)x, (int)y, msg[i], scale, rgb(r / 2 + 128, g / 2 + 128, b / 2 + 128));
    }
    // the water: the picture above it, upside down, rippling and darker
    for (int y = water; y < H; y++) {
      int dist = y - water;
      int src = water - 1 - dist * 3 / 2;
      if (src < 0) src = 0;
      int shift = (int)(10 * sinf(dist * 0.12f - t * 4.0f) * (1 + energy));
      uint32_t *dst = fb + y * W;
      const uint32_t *s = fb + src * W;
      for (int x = 0; x < W; x++) {
        int sx = x + shift;
        sx = sx < 0 ? 0 : sx >= W ? W - 1 : sx;
        uint32_t p = s[sx];
        dst[x] = rgb((int)(p & 0xff) * 4 / 10, (int)((p >> 8) & 0xff) * 4 / 10 + 6, (int)((p >> 16) & 0xff) * 5 / 10 + 18);
      }
    }
    for (int x = 0; x < W; x++) fb[water * W + x] = rgb(140, 160, 220);
  }

  void snprintf2(char *out, size_t n, const VisInput &in) {
    const char *t1 = in.title && in.title[0] ? in.title : "...";
    const char *a1 = in.author && in.author[0] ? in.author : "";
    size_t k = 0;
    auto put = [&](const char *s) {
      while (*s && k + 1 < n) out[k++] = *s++;
    };
    put("      *** NOW PLAYING: ");
    put(t1);
    if (a1[0]) {
      put("  BY  ");
      put(a1);
    }
    put("  ***  COMMODORE COMPUTERS ON PLAYSTATION 5  ***  TRIANGLE FOR THE NEXT VISUAL  ***");
    out[k] = 0;
  }

  // ---------------------------------------------------------------- GLENZ VECTORS
  struct V3 {
    float x, y, z;
  };
  struct Face {
    V3 p[3];
    int col;
  };
  static V3 rot(V3 p, float a, float b, float c) {
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
    V3 q = {p.x, p.y * ca - p.z * sa, p.y * sa + p.z * ca};
    V3 r = {q.x * cb + q.z * sb, q.y, -q.x * sb + q.z * cb};
    return {r.x * cc - r.y * sc, r.x * sc + r.y * cc, r.z};
  }

  void glenz(const VisInput &in, float dt) {
    // the dark: a faint violet in the middle
    for (int y = 0; y < H; y++) {
      uint32_t *p = fb + y * W;
      float dy = (y - H / 2) / (float)H;
      for (int x = 0; x < W; x += 4) {
        float dx = (x - W / 2) / (float)W;
        float k = 1 - (dx * dx + dy * dy) * 2.2f;
        if (k < 0) k = 0;
        uint32_t c = rgb((int)(18 * k), (int)(6 * k), (int)(34 * k));
        p[x] = p[x + 1] = p[x + 2] = p[x + 3] = c;
      }
    }
    // the warp: stars rushing out of the middle, faster with the music
    warp += dt * (0.25f + energy * 1.6f + beat * 0.8f);
    for (int i = 0; i < 420; i++) {
      uint32_t h = (uint32_t)(i + 3) * 2246822519u;
      float ang = (h & 0xffff) / 65536.0f * 6.2831853f;
      float z = fmodf((h >> 16) / 65536.0f - warp, 1.0f);
      if (z < 0) z += 1;
      float d0 = 0.02f + (1 - z) * (1 - z) * 1.3f, d1 = d0 * 0.92f;
      float x0 = W / 2 + cosf(ang) * d0 * W * 0.6f, y0 = H / 2 + sinf(ang) * d0 * W * 0.6f;
      float x1 = W / 2 + cosf(ang) * d1 * W * 0.6f, y1 = H / 2 + sinf(ang) * d1 * W * 0.6f;
      int v = (int)(220 * (1 - z));
      line(x1, y1, x0, y0, 1, v, v, v + 30);
    }
    // the channels: glowing lights on orbits round the objects
    float cx = W / 2, cy = H / 2;
    for (int c = 0; c < in.channels && c < 8; c++) {
      float a = t * (0.6f + c * 0.13f) + c * 6.2831853f / in.channels;
      float rad = 360 + 60 * sinf(t * 0.5f + c);
      float x = cx + cosf(a) * rad, y = cy + sinf(a) * rad * 0.42f;
      int r, g, b;
      hue(c / (float)in.channels + 0.55f, 1, r, g, b);
      glow(x, y, 30 + 70 * lvl[c] + 50 * kick[c], r, g, b, 0.5f + 0.5f * lvl[c]);
    }

    // the objects: a turning, breathing glenz cube (each face a low pyramid of
    // four triangles, see-through) round a solid star (an octahedron, lit)
    angA += dt * (0.5f + energy * 1.5f);
    angB += dt * (0.7f + energy * 0.8f);
    angC += dt * 0.3f;
    float size = 260 * (1 + 0.12f * beat + 0.08f * energy);
    V3 light = {-0.4f, -0.6f, -0.7f};
    Face faces[32];
    int nf = 0;
    static const V3 fc[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; f++) {
      V3 n = fc[f];
      // the face's corners: two axes across it
      V3 u = fabsf(n.x) > 0 ? V3{0, 1, 0} : V3{1, 0, 0};
      V3 v = {n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
      V3 cs[4];
      for (int k = 0; k < 4; k++) {
        float su = (k == 0 || k == 3) ? -1.0f : 1.0f, sv = (k < 2) ? -1.0f : 1.0f;
        cs[k] = {n.x + u.x * su + v.x * sv, n.y + u.y * su + v.y * sv, n.z + u.z * su + v.z * sv};
      }
      V3 apex = {n.x * 1.45f, n.y * 1.45f, n.z * 1.45f};
      for (int k = 0; k < 4; k++) faces[nf++] = {{cs[k], cs[(k + 1) % 4], apex}, (f + k) & 1};
    }
    drawMesh(faces, nf, size, light, true, in);
    // the gem inside: an icosahedron, turning the other way
    const float g = 1.618034f;
    static const V3 iv[12] = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
                              {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
    static const int it[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                  {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                  {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    Face gem[20];
    const float k = 0.62f;
    for (int i = 0; i < 20; i++) {
      for (int j = 0; j < 3; j++) gem[i].p[j] = {iv[it[i][j]].x * k, iv[it[i][j]].y * k, iv[it[i][j]].z * k};
      gem[i].col = 2 + (i % 3);
    }
    angA = -angA;
    angB = -angB;
    drawMesh(gem, 20, size * 0.62f, light, false, in);
    angA = -angA;
    angB = -angB;
  }

  void drawMesh(const Face *faces, int nf, float size, V3 light, bool glass, const VisInput &in) {
    struct Out {
      float sx[3], sy[3], z;
      float shade;
      bool front;
      int col;
    } out[32];
    float ll = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    for (int i = 0; i < nf; i++) {
      V3 q[3];
      for (int k = 0; k < 3; k++) q[k] = rot(faces[i].p[k], angA, angB, angC);
      V3 e1 = {q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z}, e2 = {q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z};
      V3 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
      float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z) + 1e-6f;
      // the normal pointing out of the object (away from its middle, the origin)
      V3 mid = {(q[0].x + q[1].x + q[2].x), (q[0].y + q[1].y + q[2].y), (q[0].z + q[1].z + q[2].z)};
      if (n.x * mid.x + n.y * mid.y + n.z * mid.z < 0) n = {-n.x, -n.y, -n.z};
      // the light shines from in front, up and to the left (the camera looks along +z)
      float dot = (n.x * light.x + n.y * light.y + n.z * light.z) / (nl * ll);
      Out &o = out[i];
      o.z = 0;
      for (int k = 0; k < 3; k++) {
        float z = q[k].z * size + 1400;
        o.sx[k] = W / 2 + q[k].x * size * 1100 / z;
        o.sy[k] = H / 2 + q[k].y * size * 1100 / z;
        o.z += z;
      }
      o.front = n.z < 0; // facing the camera
      o.shade = 0.25f + 0.75f * (dot > 0 ? dot : 0);
      o.col = faces[i].col;
    }
    // back to front
    int order[32];
    for (int i = 0; i < nf; i++) order[i] = i;
    for (int i = 1; i < nf; i++)
      for (int j = i; j > 0 && out[order[j]].z > out[order[j - 1]].z; j--) {
        int tmp = order[j];
        order[j] = order[j - 1];
        order[j - 1] = tmp;
      }
    float hueBase = 0.58f + 0.1f * sinf(t * 0.2f) + (in.channels ? in.pitch[0] * 0.15f : 0);
    for (int k = 0; k < nf; k++) {
      const Out &o = out[order[k]];
      int r, g, b;
      if (glass) {
        // two glass colours, the faces facing away fainter
        hue(hueBase + (o.col ? 0.0f : 0.5f), o.shade * (o.front ? 1.0f : 0.45f), r, g, b);
        tri(o.sx[0], o.sy[0], o.sx[1], o.sy[1], o.sx[2], o.sy[2], r, g, b, o.front ? 165 : 80, true);
        // the edges, bright
        if (o.front)
          for (int e = 0; e < 3; e++)
            line(o.sx[e], o.sy[e], o.sx[(e + 1) % 3], o.sy[(e + 1) % 3], 3, r / 2 + 140, g / 2 + 140, b / 2 + 140);
      } else {
        if (!o.front) continue;
        hue(hueBase + 0.3f + (o.col - 2) * 0.04f, o.shade * (0.85f + 0.4f * beat), r, g, b);
        tri(o.sx[0], o.sy[0], o.sx[1], o.sy[1], o.sx[2], o.sy[2], r, g, b, 256, false);
      }
    }
  }

  // ---------------------------------------------------------------- PLASMA
  void plasma(const VisInput &in, float dt) {
    (void)dt;
    // a quarter-size plasma (480 x 270), each value 4 x 4 pixels
    static uint32_t pal[256];
    float base = 0;
    int n = 0;
    for (int c = 0; c < in.channels && c < 8; c++)
      if (in.pitch[c] > 0) base += in.pitch[c], n++;
    base = n ? base / n : 0.5f;
    for (int i = 0; i < 256; i++) {
      float f = i / 256.0f;
      int r, g, b;
      // (round the whole palette and back: no seam where it wraps)
      float h = base * 0.6f + 0.22f * sinf(f * 6.2831853f) + 0.08f * sinf(f * 18.85f + t * 0.7f) + t * 0.03f;
      hue(h, 0.3f + 0.7f * (0.5f + 0.5f * sinf(f * 12.566f + t)), r, g, b);
      float lift = 0.55f + 0.35f * energy + 0.25f * beat;
      pal[i] = rgb((int)(r * lift), (int)(g * lift), (int)(b * lift));
    }
    float t1 = t * 0.9f, t2 = t * 0.6f, t3 = t * 1.3f;
    static float sx[480], sy[270];
    for (int x = 0; x < 480; x++) sx[x] = sinf(x * 0.021f + t1) + sinf(x * 0.013f - t3 * 0.7f);
    for (int y = 0; y < 270; y++) sy[y] = sinf(y * 0.027f + t2) + sinf(y * 0.011f + t1 * 0.6f);
    for (int y = 0; y < 270; y++) {
      uint32_t *row = fb + y * 4 * W;
      for (int x = 0; x < 480; x++) {
        float dx = x - 240 + 120 * sinf(t * 0.4f), dy = y - 135 + 60 * cosf(t * 0.3f);
        float v = sx[x] + sy[y] + sinf(sqrtf(dx * dx + dy * dy) * 0.05f - t * 2.0f);
        uint32_t c = pal[(int)((v + 3) * 42.5f) & 255];
        uint32_t *p = row + x * 4;
        p[0] = p[1] = p[2] = p[3] = c;
      }
      memcpy(row + W, row, W * 4);
      memcpy(row + 2 * W, row, W * 4);
      memcpy(row + 3 * W, row, W * 4);
    }
    // the sound: a ring of light breathing with it, and a line across
    float cx = W / 2, cy = H / 2, rad = 250 + 80 * energy + 40 * beat;
    glow(cx, cy, rad * 0.9f, 255, 255, 255, 0.10f + 0.25f * beat);
    if (in.wave && in.waveN > 16) {
      int N = 360;
      float px = 0, py = 0;
      for (int i = 0; i <= N; i++) {
        float a = i * 6.2831853f / N;
        int si = (int)((float)(i % N) / N * (in.waveN - 1));
        float s = in.wave[si] / 32768.0f;
        float r = rad + s * 160;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        if (i) line(px, py, x, y, 3, 255, 240, 220);
        px = x;
        py = y;
      }
      // the line across the bottom
      float lx = 0, ly = 0;
      for (int i = 0; i <= 240; i++) {
        int si = (int)((float)i / 240 * (in.waveN - 1));
        float x = i * W / 240.0f, y = 930 + in.wave[si] / 32768.0f * 220;
        if (i) line(lx, ly, x, y, 3, 200, 255, 255);
        lx = x;
        ly = y;
      }
    }
    // the channels: sparks rising from the bottom on each new note
    for (int c = 0; c < in.channels && c < 8; c++) {
      float x = W * (c + 0.5f) / in.channels;
      int r, g, b;
      hue(in.pitch[c] * 0.8f + 0.1f, 1, r, g, b);
      glow(x, H - 40 - 600 * (1 - kick[c]) * kick[c] * 2.5f, 40 + 80 * kick[c], r, g, b, kick[c]);
    }
  }

  // ---------------------------------------------------------------- the caption
  void overlay(int mode, const VisInput &in, const VisFont &font) {
    // a dark band along the bottom: the tune, the time, the controls
    int y0 = H - 92;
    for (int y = y0; y < H; y++) {
      uint32_t *p = fb + y * W;
      for (int x = 0; x < W; x++) mix(p + x, 0, 0, 0, 170);
    }
    int s = font.h >= 16 ? 2 : 3;
    char buf[160];
    const char *t1 = in.title && in.title[0] ? in.title : "";
    snprintf(buf, sizeof(buf), "%.48s", t1);
    text(font, 40, y0 + 14, buf, s, rgb(255, 255, 255));
    if (in.info) text(font, 40, y0 + 14 + font.h * s + 8, in.info, font.h >= 16 ? 1 : 2, rgb(170, 180, 210));
    if (in.progress >= 0) {
      int bw = 600, bx = W - 40 - bw, by = y0 + 30;
      for (int y = by; y < by + 10; y++)
        for (int x = bx; x < bx + bw; x++) mix(fb + y * W + x, 60, 70, 100, 200);
      int fill = (int)(bw * (in.progress > 1 ? 1 : in.progress));
      for (int y = by; y < by + 10; y++)
        for (int x = bx; x < bx + fill; x++) fb[y * W + x] = rgb(255, 255, 255);
    }
    snprintf(buf, sizeof(buf), "%s  -  TRIANGLE: NEXT VISUAL   CIRCLE: THE LIST", name(mode));
    int sw = font.h >= 16 ? 1 : 2;
    text(font, W - 40 - textW(font, buf, sw), y0 + 56, buf, sw, rgb(170, 180, 210));
    if (in.caption) text(font, 40, 30, in.caption, font.h >= 16 ? 2 : 3, rgb(255, 255, 255));
    if (in.paused) text(font, W - 40 - textW(font, "PAUSED", s), 30, "PAUSED", s, rgb(255, 220, 80));
  }
};

#endif // VISUALS_H
