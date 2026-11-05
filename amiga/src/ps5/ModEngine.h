// ModEngine.h
// A ProTracker MOD replayer for the MOD player screen (AmigaModUI): plays
// 4-channel modules (M.K., M!K!, FLT4, 4CHN, 15-sample Soundtracker) and
// 2-8 channel ones (6CHN, 8CHN, xxCH...), with the ProTracker effects, at
// 48000 samples a second on the PS5 (44100 on the board), mono.
//
// The module stays where the caller loaded it; the engine only points into
// it. No Arduino code in here, so it can be tested on a desktop PC.
//
// Not thread safe on its own: the caller keeps render() and the other
// calls apart (AmigaModUI does it with a lock). Reading the information
// fields from another task while it plays is fine - at worst a value is
// one tick old.

#ifndef MODENGINE_H
#define MODENGINE_H

#include <cstddef>
#include <cstdint>

class ModEngine {
public:
  static const int MAX_CH = 8;
  static const int RATE = 48000; // (PS5 port: AudioOut takes 48 kHz only; 44100 on the board)
  static const int SCOPE_N = 128; // samples kept per channel for the scopes

  // Start playing a module (data must stay valid while it plays). Returns
  // nullptr or why it cannot be played.
  const char *load(const uint8_t *data, size_t len);
  void unload();
  // n samples of mono sound; silence when nothing is loaded or paused
  void render(int16_t *out, int n);
  // jump to a song position (order), from row 0
  void setPosition(int order);

  bool loaded() const { return mod != nullptr; }
  bool paused = false;

  // ---- what is playing (for the screen)
  char title[21] = "";
  char format[5] = "";
  int channels = 0;
  int songLength = 0;   // orders
  int position = 0;     // order now playing
  int pattern = 0;
  int row = 0;
  int speed = 6, tempo = 125;
  uint32_t playedSamples = 0;
  bool ended = false;   // the song has come back round to a part it played

  struct ChannelView {
    int sample;         // 1-31, 0 = none yet
    int period;         // 0 = silent
    int volume;         // 0-64
    uint8_t effect, param;
    int level;          // loudness of the last bit played, 0-127
    int8_t scope[SCOPE_N];
    int scopePos;
  };
  ChannelView view[MAX_CH] = {};
  const char *sampleName(int s) const; // 1-31; 22 characters at most, not 0-ended
  int numSamples() const { return nsamples; }

  // "C-2" etc. for a period, "---" for none
  static void noteName(int period, char out[4]);

private:
  struct Sample {
    const int8_t *data;
    uint32_t length;     // bytes
    uint32_t loopStart;  // bytes
    uint32_t loopLength; // bytes; > 2 = loops
    int finetune;        // -8..7
    int volume;          // 0-64
    const char *name;
  };
  struct Channel {
    // the sound
    const Sample *smp;
    uint32_t pos;        // byte in the sample
    uint32_t frac;       // 16-bit fraction
    bool playing;
    int period;          // the note's period, with slides
    int outPeriod;       // after vibrato / arpeggio
    int volume;          // 0-64
    int outVolume;       // after tremolo
    // the row
    int sampleNum;
    int note;            // period from the pattern
    uint8_t effect, param;
    // effect memory
    int finetune;
    int portaTarget, portaSpeed;
    uint8_t vibSpeed, vibDepth, vibPos, vibWave;
    uint8_t tremSpeed, tremDepth, tremPos, tremWave;
    uint32_t offsetMem;
    int loopRow, loopCount;
    int delayTick;       // EDx: note starts at this tick
    int cutTick;         // ECx
    bool newNote;        // a note started on this row
  };

  const uint8_t *mod = nullptr;
  size_t modLen = 0;
  const uint8_t *orders = nullptr;
  const uint8_t *patterns = nullptr;
  int numPatterns = 0;
  int nsamples = 31;
  int restartPos = 0;
  Sample samples[32] = {};
  Channel ch[MAX_CH] = {};
  bool visited[128] = {};

  int tick = 0;
  int samplesLeft = 0;  // until the next tick
  int patternDelay = 0, patternDelayLeft = 0;
  bool breakPending = false;
  int breakRow = 0, jumpOrder = -1;
  bool stopped = false;
  int loopJumpRow = -1; // E6x: the row to go back to
  bool inDelay = false; // repeating a row (EEx)

  void reset();
  void doTick();
  void startRow();
  void rowEffectsTick0(int c);
  void effectsTick(int c);
  void triggerNote(int c);
  void nextRow();
  void setPeriodOut(int c);
  int periodFor(int note, int finetune, int semitones) const;
};

#endif // MODENGINE_H
