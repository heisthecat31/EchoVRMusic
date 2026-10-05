"""List the Wwise events in Echo's soundbanks, and the ones that play music or Audio Input.

Usage: python tools/find_music_events.py [BNK folder]
The folder holds raw extracted soundbanks (default: ../EchoVR-Audio-Editor/BNK).

Prints:
  - every Play event whose action targets a Music Switch / Music Playlist / Music Segment / Music
    Track (the stock music, STOCK_MUSIC_EVENTS in src/echovrmusic.cpp)
  - every event that plays a sound using the Audio Input source plugin (0x00C80002), which is how
    Echo's voice chat streams PCM into Wwise (VoiceEvent in EchoVRMusic.txt)
"""
import os
import struct
import sys

MUSIC = {10: "MusicSegment", 11: "MusicTrack", 12: "MusicSwitch", 13: "MusicRanSeq"}
AUDIO_INPUT = 0x00C80002


def chunks(b):
    p = b.find(b"BKHD")
    while 0 <= p and p + 8 <= len(b):
        tag, ln = b[p:p + 4], struct.unpack_from("<I", b, p + 4)[0]
        yield tag, b[p + 8:p + 8 + ln]
        p += 8 + ln


def scan(path):
    b = open(path, "rb").read()
    bank, objs, acts, evs, sounds = None, {}, {}, {}, {}
    for tag, body in chunks(b):
        if tag == b"BKHD":
            bank = struct.unpack_from("<I", body, 4)[0]
        elif tag == b"HIRC":
            q = 4
            for _ in range(struct.unpack_from("<I", body, 0)[0]):
                t, ln = body[q], struct.unpack_from("<I", body, q + 1)[0]
                d = body[q + 5:q + 5 + ln]
                oid = struct.unpack_from("<I", d, 0)[0]
                objs[oid] = t
                if t == 2:
                    sounds[oid] = struct.unpack_from("<I", d, 4)[0]          # source plugin ID
                elif t == 3:
                    acts[oid] = (struct.unpack_from("<H", d, 4)[0], struct.unpack_from("<I", d, 6)[0])
                elif t == 4:
                    evs[oid] = struct.unpack_from("<%dI" % d[4], d, 5)
                q += 5 + ln
    return bank, objs, acts, evs, sounds


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "..", "EchoVR-Audio-Editor", "BNK")
    for fn in sorted(os.listdir(folder)):
        bank, objs, acts, evs, sounds = scan(os.path.join(folder, fn))
        if bank is None:
            continue
        for ev, actions in evs.items():
            for a in actions:
                if a not in acts:
                    continue
                kind, target = acts[a]
                if kind & 0xFF00 != 0x0400:      # Play
                    continue
                if objs.get(target) in MUSIC:
                    print("music  bank 0x%08x (%s) event 0x%08x -> %s 0x%08x"
                          % (bank, fn, ev, MUSIC[objs[target]], target))
                elif sounds.get(target) == AUDIO_INPUT:
                    print("input  bank 0x%08x (%s) event 0x%08x -> Audio Input sound 0x%08x"
                          % (bank, fn, ev, target))
                elif objs.get(target) == 6 and any(s == AUDIO_INPUT for s in sounds.values()):
                    print("input? bank 0x%08x (%s) event 0x%08x -> switch 0x%08x (holds Audio Input sounds)"
                          % (bank, fn, ev, target))


if __name__ == "__main__":
    main()
