#!/usr/bin/env python3
"""Makes music.txt: the list the PS5 apps' music players browse.

A PS5 app can open a file by its name but can't list a folder, so the
players read this list instead. It holds every folder and tune under the
music folder, with what the player shows:

    python3 tools/make_music_list.py c64/assets/c64/music [--songlengths Songlengths.md5]
    python3 tools/make_music_list.py amiga/assets/amiga/mods

The build scripts run it for you. Run it yourself after copying more music
into the app over FTP (on a copy of the folder on your PC), then copy the
new music.txt over too.

  .sid files (PSID/RSID, the High Voltage SID Collection's): title, author,
      release, number of songs, start song; with HVSC's Songlengths.md5
      (C64Music/DOCUMENTS/Songlengths.md5) the length of each song too
  modules (.mod, mod.*, .nst, .stk): title, format, channels

The file, one line each (tab-separated):
    # comment
    D <folder>                                       (relative, "/" between parts)
    S <file> <title> <author> <released> <songs> <start> <lengths ms, comma-separated>
    M <file> <title> <format> <channels>
"""
import argparse
import hashlib
import os
import sys

SID_EXT = ('.sid',)
MOD_EXT = ('.mod', '.nst', '.stk')


def is_module(name):
    low = name.lower()
    return low.endswith(MOD_EXT) or low.startswith('mod.')


def text(b):
    """A header string: Latin-1, up to the first 0, no tabs or line ends."""
    s = b.split(b'\0', 1)[0].decode('latin-1', 'replace')
    return ''.join(c if c >= ' ' and c != '\t' else ' ' for c in s).strip()


def read_songlengths(path):
    """HVSC's new-format database: md5=time time ... (m:ss or m:ss.SSS)."""
    lengths = {}
    with open(path, 'r', encoding='latin-1') as f:
        for line in f:
            line = line.strip()
            if not line or line[0] in ';[':
                continue
            key, _, times = line.partition('=')
            ms = []
            for t in times.split():
                t = t.split('(')[0]
                m, _, rest = t.partition(':')
                s, _, frac = rest.partition('.')
                try:
                    v = (int(m) * 60 + int(s or 0)) * 1000
                    if frac:
                        v += int((frac + '000')[:3])
                except ValueError:
                    v = 0
                ms.append(v)
            lengths[key.strip().lower()] = ms
    return lengths


def sid_line(rel, data, lengths):
    if len(data) < 0x76 or data[:4] not in (b'PSID', b'RSID'):
        return None
    songs = max(1, int.from_bytes(data[0x0e:0x10], 'big'))
    start = int.from_bytes(data[0x10:0x12], 'big') or 1
    title, author, released = text(data[0x16:0x36]), text(data[0x36:0x56]), text(data[0x56:0x76])
    ms = lengths.get(hashlib.md5(data).hexdigest(), []) if lengths else []
    return '\t'.join(['S', rel, title, author, released, str(songs), str(start), ','.join(map(str, ms))])


def mod_line(rel, data):
    if len(data) < 600:
        return None
    title = text(data[:20])
    tag = data[1080:1084] if len(data) >= 1084 else b''
    fmt, chans = '', 4
    t = tag.decode('latin-1', 'replace')
    if t in ('M.K.', 'M!K!', 'FLT4', '4CHN'):
        fmt = t
    elif len(t) == 4 and t[0].isdigit() and t[1:] == 'CHN':
        fmt, chans = t, int(t[0])
    elif len(t) == 4 and t[:2].isdigit() and t[2:] in ('CH', 'CN'):
        fmt, chans = t, int(t[:2])
    elif t == 'FLT8':
        fmt, chans = t, 8
    else:
        fmt = '15-S'  # Soundtracker, 15 samples
    return '\t'.join(['M', rel, title, fmt, str(chans)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder', help='the music folder (music.txt is written there)')
    ap.add_argument('--songlengths', help="HVSC's Songlengths.md5 (default: one in the folder, if there)")
    a = ap.parse_args()
    root = os.path.abspath(a.folder)
    if not os.path.isdir(root):
        sys.exit(f'no folder {root}')
    db_path = a.songlengths
    if not db_path:
        for cand in ('Songlengths.md5', 'DOCUMENTS/Songlengths.md5', 'C64Music/DOCUMENTS/Songlengths.md5'):
            if os.path.isfile(os.path.join(root, cand)):
                db_path = os.path.join(root, cand)
                break
    lengths = read_songlengths(db_path) if db_path else None

    lines = ['# made by tools/make_music_list.py - the music players read this']
    nsid = nmod = nlen = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort(key=str.lower)
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, '/')
        if rel_dir != '.':
            lines.append('D\t' + rel_dir)
        for name in sorted(filenames, key=str.lower):
            if name.startswith('.'):
                continue
            rel = name if rel_dir == '.' else rel_dir + '/' + name
            low = name.lower()
            if low.endswith(SID_EXT):
                with open(os.path.join(dirpath, name), 'rb') as f:
                    line = sid_line(rel, f.read(), lengths)
                if line:
                    lines.append(line)
                    nsid += 1
                    nlen += line.rsplit('\t', 1)[1] != ''
            elif is_module(name):
                with open(os.path.join(dirpath, name), 'rb') as f:
                    line = mod_line(rel, f.read(1084))
                if line:
                    lines.append(line)
                    nmod += 1
    with open(os.path.join(root, 'music.txt'), 'w', encoding='latin-1', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    parts = []
    if nsid:
        parts.append(f'{nsid} SID tunes' + (f' ({nlen} with song lengths)' if lengths else ''))
    if nmod:
        parts.append(f'{nmod} modules')
    print(f"music.txt: {', '.join(parts) or 'no music'}")


if __name__ == '__main__':
    main()
