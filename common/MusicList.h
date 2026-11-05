// MusicList.h
// The music players' list of folders and tunes: music.txt, made on the PC by
// tools/make_music_list.py (a PS5 app can't list a folder). Shared by the
// C64's SID player and the Amiga's MOD player; the build scripts copy it in.
//
// The whole file is read into one block from the system (a PS5 app's malloc
// heap is small, and the High Voltage SID Collection's list is several MB);
// the entries point into it.

#ifndef MUSICLIST_H
#define MUSICLIST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct MusicEntry {
  char kind;           // 'D' folder, 'S' SID tune, 'M' module
  const char *path;    // relative to the music folder
  const char *name;    // the last part of path
  const char *parent;  // the folder it is in (not 0-ended: parentLen)
  int parentLen;
  const char *title;   // S: title,    M: title
  const char *author;  // S: author,   M: format (M.K. ...)
  const char *extra;   // S: released, M: channels
  int songs, start;    // S only
  const char *lengths; // S only: ms per song, comma-separated ("" = unknown)
};

class MusicList {
public:
  // alloc: big blocks (from the system, not malloc)
  bool load(const char *file, void *(*alloc)(size_t), void (*release)(void *)) {
    clear(release);
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
      fclose(f);
      return false;
    }
    text = (char *)alloc((size_t)size + 1);
    if (!text) {
      fclose(f);
      return false;
    }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;
    // count the lines, then make the entries
    size_t lines = 1;
    for (size_t i = 0; i < got; i++) lines += text[i] == '\n';
    entries = (MusicEntry *)alloc(lines * sizeof(MusicEntry));
    if (!entries) return false;
    count = 0;
    char *p = text;
    while (*p) {
      char *line = p;
      char *end = strchr(p, '\n');
      if (end) {
        *end = 0;
        p = end + 1;
      } else {
        p += strlen(p);
      }
      size_t n = strlen(line);
      if (n && line[n - 1] == '\r') line[n - 1] = 0;
      if (line[0] != 'D' && line[0] != 'S' && line[0] != 'M') continue;
      if (line[1] != '\t') continue;
      char *field[9] = {};
      int nf = 0;
      char *q = line;
      while (nf < 9) {
        field[nf++] = q;
        char *tab = strchr(q, '\t');
        if (!tab) break;
        *tab = 0;
        q = tab + 1;
      }
      MusicEntry &e = entries[count];
      memset(&e, 0, sizeof(e));
      e.kind = line[0];
      e.path = nf > 1 ? field[1] : "";
      if (!e.path[0]) continue;
      const char *slash = strrchr(e.path, '/');
      e.name = slash ? slash + 1 : e.path;
      e.parent = e.path;
      e.parentLen = slash ? (int)(slash - e.path) : 0;
      e.title = e.author = e.extra = e.lengths = "";
      if (e.kind == 'S') {
        if (nf > 2) e.title = field[2];
        if (nf > 3) e.author = field[3];
        if (nf > 4) e.extra = field[4];
        e.songs = nf > 5 ? atoi(field[5]) : 1;
        e.start = nf > 6 ? atoi(field[6]) : 1;
        if (nf > 7) e.lengths = field[7];
        if (e.songs < 1) e.songs = 1;
        if (e.start < 1 || e.start > e.songs) e.start = 1;
      } else if (e.kind == 'M') {
        if (nf > 2) e.title = field[2];
        if (nf > 3) e.author = field[3];
        if (nf > 4) e.extra = field[4];
      }
      count++;
    }
    return true;
  }

  void clear(void (*release)(void *)) {
    if (release) {
      if (text) release(text);
      if (entries) release(entries);
    }
    text = nullptr;
    entries = nullptr;
    count = 0;
  }

  int size() const { return count; }
  const MusicEntry &operator[](int i) const { return entries[i]; }

  // What is in a folder ("" = the top): its folders first, then its tunes,
  // in the list's order (the PC tool sorts them). Fills out, returns how many.
  int children(const char *folder, int *out, int max) const {
    int flen = (int)strlen(folder), n = 0;
    for (int pass = 0; pass < 2; pass++)
      for (int i = 0; i < count && n < max; i++) {
        const MusicEntry &e = entries[i];
        if ((e.kind == 'D') != (pass == 0)) continue;
        if (e.parentLen == flen && strncmp(e.parent, folder, (size_t)flen) == 0) out[n++] = i;
      }
    return n;
  }

  // Song "song" (1-based) of a SID tune: its length in ms, 0 = not known
  static uint32_t songLength(const MusicEntry &e, int song) {
    const char *p = e.lengths;
    for (int s = 1; *p; s++) {
      uint32_t v = (uint32_t)strtoul(p, nullptr, 10);
      if (s == song) return v;
      const char *c = strchr(p, ',');
      if (!c) break;
      p = c + 1;
    }
    return 0;
  }

private:
  char *text = nullptr;
  MusicEntry *entries = nullptr;
  int count = 0;
};

#endif // MUSICLIST_H
