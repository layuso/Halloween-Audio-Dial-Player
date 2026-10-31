#!/usr/bin/env python3
"""Recover the original embedded Halloween MP3 from a *read-only* S3 flash dump.

The S3 app links the source MP3 directly into its .rodata. This tool scans a
local flash dump for complete MPEG-1 Layer III frames and ONLY writes a file
when its SHA-256 matches the original audio recorded during the Pi build.
It does not communicate with, erase, or flash a device.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

EXPECTED_SHA256 = "0836e37495671b961843487ef149e994ddb65fd496815bc4f807db670ac307e9"
MAX_MP3_BYTES = 1024 * 1024
BITRATES_KBPS = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0)
SAMPLE_RATES = (44100, 48000, 32000)


def frame_length(data: bytes, pos: int) -> int:
    if pos + 4 > len(data) or data[pos] != 0xFF:
        return 0
    b1, b2 = data[pos + 1], data[pos + 2]
    # Sync + MPEG-1 + Layer III; 0xFA indicates a protected frame and 0xFB
    # indicates an unprotected frame. Require real bitrate / sample-rate IDs.
    if (b1 & 0xFE) != 0xFA:
        return 0
    rate_idx, sample_idx = b2 >> 4, (b2 >> 2) & 3
    if rate_idx in (0, 15) or sample_idx == 3:
        return 0
    length = (144000 * BITRATES_KBPS[rate_idx]) // SAMPLE_RATES[sample_idx]
    length += (b2 >> 1) & 1
    return length if pos + length <= len(data) else 0


def possible_starts(data: bytes, frame_start: int) -> list[int]:
    starts = [frame_start]
    # Include an optional ID3v2 tag if it terminates exactly at the first
    # MPEG frame (the common layout).
    search_from = max(0, frame_start - 1024 * 1024)
    tag = data.rfind(b"ID3", search_from, frame_start)
    if tag >= 0 and tag + 10 <= frame_start:
        hdr = data[tag:tag + 10]
        if hdr[3] < 0xFF and all(x < 0x80 for x in hdr[6:10]):
            tag_size = (hdr[6] << 21) | (hdr[7] << 14) | (hdr[8] << 7) | hdr[9]
            footer = 10 if hdr[5] & 0x10 else 0
            if tag + 10 + tag_size + footer == frame_start:
                starts.append(tag)
    return starts


def recover(dump: bytes) -> bytes | None:
    pos = 0
    examined = 0
    target = bytes.fromhex(EXPECTED_SHA256)
    while True:
        pos = dump.find(b"\xff", pos)
        if pos < 0 or pos + 4 >= len(dump):
            break
        candidate = pos
        pos += 1
        length = frame_length(dump, candidate)
        if length == 0:
            continue
        second = candidate + length
        next_len = frame_length(dump, second)
        if not next_len or not frame_length(dump, second + next_len):
            continue
        examined += 1
        for start in possible_starts(dump, candidate):
            cursor = candidate
            frames = 0
            digest = hashlib.sha256(dump[start:candidate])
            while cursor - start < MAX_MP3_BYTES:
                length = frame_length(dump, cursor)
                if not length or cursor + length - start > MAX_MP3_BYTES:
                    break
                digest.update(dump[cursor:cursor + length])
                cursor += length
                frames += 1
                if frames >= 3 and digest.digest() == target:
                    return dump[start:cursor]
            # Some MP3s append ID3v1 or a short trailer. Check bounded
            # trailer lengths without copying/writing unverified audio.
            if frames >= 3:
                for extra in range(1, min(4096, MAX_MP3_BYTES - (cursor - start)) + 1):
                    if cursor + extra > len(dump):
                        break
                    h = digest.copy()
                    h.update(dump[cursor:cursor + extra])
                    if h.digest() == target:
                        return dump[start:cursor + extra]
    print(f"Scanned flash; found {examined} plausible MP3 frame sequences.", file=sys.stderr)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash_dump", type=Path, help="Local read_flash output from the S3")
    parser.add_argument("output_mp3", type=Path, help="Destination for the verified original MP3")
    args = parser.parse_args()
    blob = args.flash_dump.read_bytes()
    if len(blob) < 100000:
        parser.error("Flash dump is too small to contain the S3 application")
    audio = recover(blob)
    if audio is None:
        print("NO VERIFIED AUDIO RECOVERED. Nothing written. Use the original MP3 if available.", file=sys.stderr)
        return 1
    digest = hashlib.sha256(audio).hexdigest()
    if digest != EXPECTED_SHA256:
        raise RuntimeError("Internal integrity error; refusing to write")
    if args.output_mp3.exists():
        if hashlib.sha256(args.output_mp3.read_bytes()).hexdigest() == digest:
            print(f"Already verified: {args.output_mp3} ({len(audio)} bytes)")
            return 0
        parser.error(f"Destination exists with different content: {args.output_mp3}")
    args.output_mp3.parent.mkdir(parents=True, exist_ok=True)
    args.output_mp3.write_bytes(audio)
    print(f"VERIFIED original MP3 recovered: {args.output_mp3}")
    print(f"Bytes: {len(audio)}  SHA256: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
