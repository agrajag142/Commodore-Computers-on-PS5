#!/usr/bin/env python3
"""Makes two tiny test tunes for the music players, written here from
scratch (free to use):

  test.sid   a PSID tune with 2 songs: a scale on a sawtooth (song 1) and the
             same a fifth higher on a pulse (song 2)
  test.mod   a 4-channel ProTracker module (M.K.): a melody, a bass and a beat
             made of two generated samples, 2 patterns

    python3 tools/make_test_music.py c64/assets/c64/music       (writes test.sid)
    python3 tools/make_test_music.py amiga/assets/amiga/mods    (writes test.mod)
    python3 tools/make_test_music.py <folder> --both
"""
import math
import os
import sys


# ---------------------------------------------------------------- SID
def make_sid():
    load = 0x1000
    code = bytearray()
    labels, fixups = {}, []

    def emit(*b):
        code.extend(b)

    def here():
        return load + len(code)

    def label(n):
        labels[n] = here()

    def ref(n):  # an absolute address, filled in later
        fixups.append((len(code), n))
        code.extend((0, 0))

    VAR_SONG, VAR_TICK, VAR_STEP = 0x10F0, 0x10F1, 0x10F2
    lo, hi = lambda a: a & 0xff, lambda a: a >> 8

    emit(0x4C); ref('init')               # $1000 JMP init
    emit(0x4C); ref('play')               # $1003 JMP play
    label('init')
    emit(0x8D, lo(VAR_SONG), hi(VAR_SONG))  # STA song (0 or 1)
    for reg, val in ((0x18, 0x0F), (0x05, 0x09), (0x06, 0xA0), (0x02, 0x00), (0x03, 0x08)):
        emit(0xA9, val, 0x8D, reg, 0xD4)  # LDA #val, STA $D4reg
    emit(0xA9, 0x00, 0x8D, lo(VAR_TICK), hi(VAR_TICK))
    emit(0x8D, lo(VAR_STEP), hi(VAR_STEP))
    emit(0x60)                            # RTS
    label('play')
    emit(0xEE, lo(VAR_TICK), hi(VAR_TICK))  # INC tick
    emit(0xAD, lo(VAR_TICK), hi(VAR_TICK))  # LDA tick
    emit(0x29, 0x0F)                      # AND #$0F: a note every 16 frames
    emit(0xD0, 0)                         # BNE done (offset below)
    bne_at = len(code) - 1
    emit(0xEE, lo(VAR_STEP), hi(VAR_STEP))  # INC step
    emit(0xAD, lo(VAR_STEP), hi(VAR_STEP))  # LDA step
    emit(0x29, 0x07, 0xAA)                # AND #7, TAX
    emit(0xAD, lo(VAR_SONG), hi(VAR_SONG))  # LDA song
    emit(0xF0, 3)                         # BEQ +3 (song 1)
    emit(0x8A, 0x09, 0x08, 0xAA)          # TXA, ORA #8, TAX (song 2: second table)
    emit(0xBD); ref('table')              # LDA table,X
    emit(0x8D, 0x01, 0xD4)                # STA $D401 (frequency, high byte)
    emit(0xAD, lo(VAR_SONG), hi(VAR_SONG))
    emit(0xF0, 4)                         # BEQ saw
    emit(0xA9, 0x40, 0xD0, 2)             # LDA #$40 (pulse), BNE +2
    emit(0xA9, 0x20)                      # saw: LDA #$20
    emit(0x8D, 0x04, 0xD4)                # STA $D404: gate off
    emit(0x09, 0x01, 0x8D, 0x04, 0xD4)    # ORA #1, STA $D404: gate on
    label('done')
    code[bne_at] = labels['done'] - (load + bne_at + 1)
    emit(0x60)                            # RTS
    label('table')
    # a major scale (high bytes of the SID frequency, PAL), then a fifth up
    emit(0x11, 0x13, 0x15, 0x16, 0x19, 0x1C, 0x1F, 0x22)
    emit(0x19, 0x1C, 0x1F, 0x21, 0x25, 0x2A, 0x2F, 0x32)
    for at, n in fixups:
        code[at], code[at + 1] = lo(labels[n]), hi(labels[n])

    def s32(t):
        return t.encode('latin-1')[:32].ljust(32, b'\0')

    header = bytearray(b'PSID')
    header += (2).to_bytes(2, 'big') + (0x7C).to_bytes(2, 'big')
    header += (0).to_bytes(2, 'big')              # load address: in the data
    header += (0x1000).to_bytes(2, 'big') + (0x1003).to_bytes(2, 'big')
    header += (2).to_bytes(2, 'big') + (1).to_bytes(2, 'big')   # 2 songs, start 1
    header += (0).to_bytes(4, 'big')              # speed: vertical blank
    header += s32('PS5 Test Tune') + s32('Commodore Computers on PS5') + s32('2026 Test')
    header += (0x0014).to_bytes(2, 'big')         # flags: PAL, 6581
    header += bytes(4)                            # start page, page length, 2nd and 3rd SID
    assert len(header) == 0x7C
    return bytes(header) + load.to_bytes(2, 'little') + bytes(code)


# ---------------------------------------------------------------- MOD
PERIODS = {'C-1': 856, 'D-1': 762, 'E-1': 678, 'F-1': 640, 'G-1': 570, 'A-1': 508, 'B-1': 453,
           'C-2': 428, 'D-2': 381, 'E-2': 339, 'F-2': 320, 'G-2': 285, 'A-2': 254, 'B-2': 226, 'C-3': 214}


def make_mod():
    # sample 1: a soft square wave (looped); sample 2: a short noise burst
    sq = bytes((40 if i < 16 else -40) & 0xff for i in range(32))
    seed, noise = 12345, bytearray()
    for i in range(2000):
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        noise.append(int(((seed >> 16) % 200 - 100) * (1 - i / 2000)) & 0xff)
    samples = [(b'square', sq, 0, 16), (b'noise', bytes(noise), 0, 0)]

    out = bytearray(b'PS5 TEST MODULE'.ljust(20, b'\0'))
    for i in range(31):
        if i < len(samples):
            name, data, ls, ll = samples[i]
            out += name.ljust(22, b'\0') + (len(data) // 2).to_bytes(2, 'big') + bytes([0, 64])
            out += ls.to_bytes(2, 'big') + (ll if ll else 1).to_bytes(2, 'big')
        else:
            out += bytes(22) + bytes(2) + bytes([0, 0]) + bytes(2) + (1).to_bytes(2, 'big')
    out += bytes([4, 127]) + bytes([0, 1, 0, 1]) + bytes(124) + b'M.K.'

    def cell(sample, note, effect=0, param=0):
        p = PERIODS.get(note, 0)
        return bytes([(sample & 0xF0) | (p >> 8), p & 0xff, ((sample & 0x0F) << 4) | effect, param])

    melody = [['C-2', 'E-2', 'G-2', 'C-3', 'G-2', 'E-2', 'D-2', 'B-1'],
              ['A-1', 'C-2', 'E-2', 'A-2', 'G-2', 'F-2', 'E-2', 'D-2']]
    bass = [['C-1', 'G-1', 'A-1', 'F-1'], ['A-1', 'E-1', 'F-1', 'G-1']]
    for pat in range(2):
        for row in range(64):
            c0 = cell(1, melody[pat][row // 8]) if row % 8 == 0 else bytes(4)
            c1 = cell(1, bass[pat][row // 16], 0xC, 40) if row % 16 == 0 else bytes(4)
            c2 = cell(2, 'C-2', 0xC, 48) if row % 8 == 4 else bytes(4)
            c3 = cell(0, '', 0xF, 5) if (pat, row) == (0, 0) else bytes(4)  # speed 5
            out += c0 + c1 + c2 + c3
    for _, data, _, _ in samples:
        out += data
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    both = '--both' in sys.argv
    want_sid = both or 'c64' in folder.lower() or 'music' in folder.lower()
    want_mod = both or 'mod' in folder.lower() or 'amiga' in folder.lower()
    if want_sid:
        open(os.path.join(folder, 'test.sid'), 'wb').write(make_sid())
        print('test.sid')
    if want_mod:
        open(os.path.join(folder, 'test.mod'), 'wb').write(make_mod())
        print('test.mod')


if __name__ == '__main__':
    main()
