// PS5File.cpp
// Drive 8 on the PS5: the app's c64 folder.
//
//   <app>/assets/c64 the programs (.prg) and disks (.d64) put in the app when it
//                    was built (c64/assets/c64), or copied there later over FTP.
//                    Read-only.
//   <data>/c64       what SAVE"NAME",8 writes (the app's own storage)
//
// The PS5's sandbox won't list a folder (opendir is refused), so the build
// writes c64/files.txt with the names in it, and SAVE keeps its own
// <data>/c64/files.txt. If the folder can be listed after all (the PC
// simulator, files copied over FTP), that's used too.
//
// To the C64: LOAD"NAME",8 finds NAME.prg (any case, "NA*" and "?" work),
// LOAD"$",8 makes a directory listing (as the Waveshare port's built-in
// drive does), and the menu attaches .d64 disks (the emulator's own floppy
// code then reads them).

#include "../core/fs/FileFactory.h"
#include "ps5_app.h"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

std::string appFolder() { return std::string(ps5AppDir()) + "/assets/c64/"; }
std::string dataFolder() { return std::string(ps5DataDir()) + "/c64/"; }

bool lowerEndsWith(const std::string &s, const char *end) {
  size_t n = strlen(end);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)s[s.size() - n + i]) != end[i]) return false;
  return true;
}

bool isC64File(const std::string &s) { return lowerEndsWith(s, ".prg") || lowerEndsWith(s, ".d64"); }

// "abc", "ab*", "a?c" and "*", ignoring case
bool nameMatches(const char *text, const char *pattern) {
  const char *retryText = nullptr, *retryPattern = nullptr;
  while (*text) {
    if (*pattern == '*') {
      retryPattern = ++pattern;
      retryText = text;
    } else if (*pattern == '?' || tolower((unsigned char)*text) == tolower((unsigned char)*pattern)) {
      pattern++;
      text++;
    } else if (retryText) {
      pattern = retryPattern;
      text = ++retryText;
    } else {
      return false;
    }
  }
  while (*pattern == '*') pattern++;
  return !*pattern;
}

// The names in a folder's files.txt, and in the folder itself if it can be listed
void readFolder(const std::string &folder, std::vector<std::string> &out) {
  FILE *f = fopen((folder + "files.txt").c_str(), "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
      if (n && isC64File(line)) out.push_back(line);
    }
    fclose(f);
  }
  DIR *d = opendir(folder.c_str());
  if (d) {
    while (dirent *e = readdir(d)) {
      std::string n = e->d_name;
      if (isC64File(n)) out.push_back(n);
    }
    closedir(d);
  }
}

struct Entry {
  std::string name;   // as it is in the folder
  std::string folder; // where
};

// Every program and disk: the saved ones first (they win a name clash)
std::vector<Entry> allFiles() {
  std::vector<Entry> all;
  std::vector<std::string> names;
  readFolder(dataFolder(), names);
  for (auto &n : names) all.push_back({n, dataFolder()});
  names.clear();
  readFolder(appFolder(), names);
  for (auto &n : names) all.push_back({n, appFolder()});
  // one of each name (ignoring case)
  std::vector<Entry> out;
  for (auto &e : all) {
    bool seen = false;
    for (auto &o : out) seen = seen || strcasecmp(o.name.c_str(), e.name.c_str()) == 0;
    if (!seen) out.push_back(e);
  }
  std::sort(out.begin(), out.end(),
            [](const Entry &a, const Entry &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
  return out;
}

std::string upper(std::string s) {
  for (auto &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

class PS5File : public FileDriver {
  FILE *fp = nullptr;
  bool writing = false;
  std::string writeName;
  std::vector<uint8_t> listing; // the "$" directory, when open
  size_t listPos = 0;
  bool listOpen = false;
  std::vector<std::string> listNames; // for listnextentry
  size_t listIndex = 0;

  void buildListing() {
    listing.clear();
    auto addLine = [this](unsigned number, const std::string &text) {
      listing.push_back(0x01); // "next line" pointer: anything not 0, BASIC fixes it
      listing.push_back(0x01);
      listing.push_back((uint8_t)(number & 0xff));
      listing.push_back((uint8_t)(number >> 8));
      for (char c : text) listing.push_back((uint8_t)c);
      listing.push_back(0);
    };
    listing.push_back(0x01); // load address $0801
    listing.push_back(0x08);
    addLine(0, "\x12\"PLAYSTATION 5   \" PS 2A");
    for (auto &e : allFiles()) {
      if (!lowerEndsWith(e.name, ".prg")) continue; // (disks are attached from the menu)
      long bytes = 0;
      FILE *f = fopen((e.folder + e.name).c_str(), "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        bytes = ftell(f);
        fclose(f);
      }
      unsigned blocks = (unsigned)((bytes + 253) / 254);
      std::string name = upper(e.name.substr(0, e.name.size() - 4));
      if (name.size() > 16) name.resize(16);
      std::string line(blocks < 10 ? 3 : (blocks < 100 ? 2 : 1), ' ');
      line += '"' + name + '"';
      line.append(16 - name.size(), ' ');
      line += " PRG";
      addLine(blocks, line);
    }
    addLine(0, "BLOCKS FREE.");
    listing.push_back(0);
    listing.push_back(0);
  }

public:
  ~PS5File() override { close(); }

  bool init() override {
    mkdir(ps5DataDir(), 0777);
    mkdir(dataFolder().c_str(), 0777);
    return true;
  }

  bool open(const std::string &path, const char *mode) override {
    close();
    std::string name = path;
    size_t colon = name.find(':'); // ("0:NAME")
    if (colon != std::string::npos) name.erase(0, colon + 1);
    if (name.empty() || name[0] == '.') return false; // (.config.json: none)

    if (mode && (mode[0] == 'w' || mode[0] == 'a')) {
      // SAVE: into the app's own storage, and into its files.txt
      for (auto &c : name)
        if (c == '/' || c == '\\') c = '_';
      fp = fopen((dataFolder() + name).c_str(), "wb");
      if (!fp) return false;
      writing = true;
      writeName = name;
      return true;
    }

    if (name == "$" || name == "$.prg") {
      buildListing();
      listPos = 0;
      listOpen = true;
      return true;
    }
    // the first program or disk the name matches (".prg" or ".d64" given
    // by the emulator; the name may have * and ?)
    if (!isC64File(name)) name += ".prg";
    for (auto &e : allFiles()) {
      if (nameMatches(e.name.c_str(), name.c_str())) {
        fp = fopen((e.folder + e.name).c_str(), mode ? mode : "rb");
        if (!fp && mode && strchr(mode, '+')) fp = fopen((e.folder + e.name).c_str(), "rb");
        return fp != nullptr;
      }
    }
    return false;
  }

  size_t read(void *buffer, size_t count) override {
    if (listOpen) {
      size_t left = listing.size() - listPos;
      if (count > left) count = left;
      memcpy(buffer, listing.data() + listPos, count);
      listPos += count;
      return count;
    }
    return fp ? fread(buffer, 1, count, fp) : 0;
  }

  size_t write(const void *buffer, size_t count) override { return fp ? fwrite(buffer, 1, count, fp) : 0; }

  bool seek(long offset, int origin) override {
    if (listOpen) {
      long target = origin == SEEK_SET ? offset : origin == SEEK_CUR ? (long)listPos + offset : (long)listing.size() + offset;
      if (target < 0 || target > (long)listing.size()) return false;
      listPos = (size_t)target;
      return true;
    }
    return fp && fseek(fp, offset, origin) == 0;
  }

  long tell() const override {
    if (listOpen) return (long)listPos;
    return fp ? ftell(fp) : -1;
  }

  bool eof() override {
    if (listOpen) return listPos >= listing.size();
    return fp && feof(fp);
  }

  int64_t size() override {
    if (listOpen) return (int64_t)listing.size();
    if (!fp) return -1;
    long at = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, at, SEEK_SET);
    return n;
  }

  void close() override {
    if (fp) {
      fclose(fp);
      fp = nullptr;
      if (writing) {
        // remember the new file (the sandbox can't list the folder)
        std::vector<std::string> names;
        readFolder(dataFolder(), names);
        bool known = false;
        for (auto &n : names) known = known || strcasecmp(n.c_str(), writeName.c_str()) == 0;
        if (!known) {
          FILE *f = fopen((dataFolder() + "files.txt").c_str(), "a");
          if (f) {
            fprintf(f, "%s\n", writeName.c_str());
            fclose(f);
          }
        }
      }
    }
    writing = false;
    listOpen = false;
    listing.clear();
  }

  // The emulator's LIST command: one name per call, "" after the last
  bool listnextentry(std::string &name, bool start) override {
    if (start) {
      listNames.clear();
      for (auto &e : allFiles()) listNames.push_back(e.name);
      listIndex = 0;
    }
    name = listIndex < listNames.size() ? listNames[listIndex++] : "";
    return true;
  }
};

} // namespace

std::vector<std::string> ps5ListC64Files() {
  std::vector<std::string> out;
  for (auto &e : allFiles()) out.push_back(e.name);
  return out;
}

namespace FileSys {
std::unique_ptr<FileDriver> create() { return std::unique_ptr<FileDriver>(new PS5File()); }
} // namespace FileSys
