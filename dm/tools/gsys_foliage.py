#!/usr/bin/env python3
"""Turns layers of a gsys game's own foliage off (Nintendo EPD's engine): writes false into named booleans of every scene's foliage settings (the BYML files whose
name holds --file in the pack --pack), and writes the pack as a RomFS overlay file (DM_ROMFS_OVERLAY, dm/host/hle_fs.c). The game then places none of those layers:
in Zelda: Ocarina of Time (0400486024DEC000) Pack/Foliage.pack.zs holds 61 Foliage/ResFoliageScene/<scene>.game__foliage__ResFoliageScene.bgyml files whose
IsEnableMoss, IsEnableStone, IsEnableUndergrowth4 and IsEnableUndergrowth8 switch the moss, the stones and the two undergrowth layers that its task/mesh foliage
programs draw (IsEnableTurf, the grass, is a layer the profile does not name). The idea is AnkaNative's "Ground plants and stones" (GPL: the idea only; this is
written from the files and gsys_dynres.py).

The values are found by key name anywhere in each file, written in place (a BYML bool is a 32-bit word: the file and the pack keep their sizes, so the game's
ResourceSizeTable stays right), and the pack is compressed again with the RomFS's dictionary (gsys_dynres.py: the original frame's dictionary ID, content size and
checksum flag). The new frame is decompressed and compared: only the edited words differ.

  gsys_foliage.py --romfs '<game>.nup:update/131072/romfs' --off IsEnableMoss,IsEnableStone --out '<NSP folder>/<id>-work/romfs_overlay_foliage'
      [--pack Pack/Foliage.pack.zs] [--zsdic Pack/ZsDic.pack.zs] [--file ResFoliageScene] [--level 19]

--romfs is a RomFS folder, a RomFS image or a NUP package (romfs.py). Exit 1 on any error, and when no file holds any of the keys (another build). The output goes to
the game's work folder (or, on the phone, the app's overlay folder: FoliageOverlay), never into this repository."""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romfs  # noqa: E402
import gsys_dynres as g  # noqa: E402

D_BOOL = 0xD0


def edit(raw, file_part, keys, log=print):
    """The edited pack bytes, the edited value word offsets and {key: (files that had it on, files where it was already off)}, from the decompressed pack raw"""
    files = g.sarc_files(raw)
    hits = sorted(k for k in files if file_part in k and k.endswith('.bgyml'))
    if not hits:
        raise g.Error('no .bgyml file whose name holds %r in the pack' % file_part)
    buf = bytearray(raw)
    words, seen = [], {k: [0, 0] for k in keys}
    for name in hits:
        o, _n = files[name]
        by = g.Byml(buf, o)
        found = []
        done = set()

        def walk(off, by=by, found=found, done=done):
            if off in done:
                return
            done.add(off)
            for key, t, w in by.items(off):
                if t in g.CONTAINERS:
                    walk(by.u32(w))
                elif t == D_BOOL and key in seen:
                    found.append((key, w))
        if by.root:
            walk(by.root)
        changed = []
        for key, w in found:
            if by.u32(w):
                struct.pack_into(by.e + 'I', buf, w, 0)
                words.append(w)
                seen[key][0] += 1
                changed.append(key)
            else:
                seen[key][1] += 1
        if changed:
            log('%s: %s off' % (name.rsplit('/', 1)[-1], ', '.join(changed)))
    if not words:
        raise g.Error('nothing to edit: no file has %s on (%d files read)' % (', '.join(keys), len(hits)))
    return bytes(buf), words, seen


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', required=True, help='RomFS folder, image, or <file>.nup:update/<version>/romfs')
    ap.add_argument('--off', required=True, help='the booleans to write false, comma-separated (the profile\'s foliage_off rows)')
    ap.add_argument('--pack', default='Pack/Foliage.pack.zs', help='the pack, in the RomFS (default %(default)s)')
    ap.add_argument('--zsdic', default='Pack/ZsDic.pack.zs', help='the dictionaries, in the RomFS (default %(default)s)')
    ap.add_argument('--file', default='ResFoliageScene', help='part of the edited files\' names in the pack (default %(default)s)')
    ap.add_argument('--level', type=int, default=19, help='zstd level (default %(default)s)')
    ap.add_argument('--out', required=True, help='overlay root: the pack is written at <out>/<pack>')
    a = ap.parse_args(argv)
    try:
        keys = [k for k in (x.strip() for x in a.off.split(',')) if k]
        if not keys or any(not k.isidentifier() for k in keys):
            raise g.Error('--off takes BYML key names, comma-separated: %r' % a.off)
        src = a.romfs.rstrip('/\\') + '/' + a.pack
        with romfs.open(src, 'rb') as f:
            packed = f.read()
        with romfs.open(a.romfs.rstrip('/\\') + '/' + a.zsdic, 'rb') as f:
            dictionary = g.zsdic(f.read())
        size, did, checksum = g.frame_info(packed)
        raw = g.decompress(packed, dictionary)
        if size is not None and len(raw) != size:
            raise g.Error('the pack decompressed to %d bytes, its frame says %d' % (len(raw), size))
        print('%s: %d bytes, %d decompressed (dictionary ID %#x%s)' % (src, len(packed), len(raw), did, ', checksum' if checksum else ''))
        new, words, seen = edit(raw, a.file, keys)
        for k in keys:
            print('%s: turned off in %d files, already off in %d' % (k, seen[k][0], seen[k][1]))
        out = g.compress(new, dictionary, a.level, checksum, did != 0)
        size2, did2, _ = g.frame_info(out)
        back = g.decompress(out, dictionary)
        if back != new or size2 != len(new) or did2 != did:
            raise g.Error('the new frame does not decompress to the edited pack (size %s, dictionary ID %#x)' % (size2, did2))
        allowed = {w + k for w in words for k in range(4)}
        if len(back) != len(raw) or any(raw[i] != back[i] and i not in allowed for i in range(len(raw))):
            raise g.Error('bytes outside the edited words changed')
        dst = os.path.join(a.out, *a.pack.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(out)
        print('wrote %s: %d bytes (%d words edited, size kept at %d decompressed)' % (dst, len(out), len(words), len(back)))
        return 0
    except (g.Error, OSError, ValueError, struct.error) as e:
        print('gsys_foliage: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
