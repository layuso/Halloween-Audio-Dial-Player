#!/usr/bin/env python3
"""Allow FF FA CRC-protected MPEG-1 Layer III frame signatures in audio-player 1.0.7."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
p=root/'managed_components/chmorgan__esp-audio-player/audio_mp3.cpp'
if not p.exists():sys.exit('Missing managed component. Run idf.py reconfigure first.')
s=p.read_text()
a='''if((magic[0] == 0xFF) &&
            (magic[1] == 0xFB))'''
b='''if((magic[0] == 0xFF) &&
            ((magic[1] == 0xFB) || (magic[1] == 0xFA)))'''
if b in s:
    print('MP3 FF FA already supported');sys.exit(0)
if a not in s:
    if 'mp3_frame_signature_matches(magic)' in s:
        print('Newer MP3 recognizer detected; inspect FF FA acceptance');sys.exit(0)
    sys.exit('Unknown parser revision: refusing patch')
p.write_text(s.replace(a,b,1))
print('Patched FF FA MPEG Layer III recognition',p)
