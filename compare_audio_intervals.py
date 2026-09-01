#!/usr/bin/env python3
"""Compare a hardware capture with the known-good FretFlow reference.

The tool intentionally uses only Python's standard library so it can run on a
fresh macOS setup.  It reports every fixed-size interval rather than relying
on a single whole-file peak, which is important when a capture contains a
short valid intro followed by silence, a buzzer, or a feedback burst.
"""

import argparse
import math
import wave
from pathlib import Path
import struct


def read_wav(path):
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        rate = wav.getframerate()
        frames = wav.getnframes()
        width = wav.getsampwidth()
        if wav.getcomptype() != "NONE":
            raise ValueError(f"{path}: 仅支持未压缩 PCM，实际为 {wav.getcomptype()}")
        raw = wav.readframes(frames)
    # Convert all common PCM widths to a signed 16-bit-equivalent scale.  The
    # FretFlow reference is 24-bit while ffmpeg captures from AVFoundation as
    # 16-bit, so rejecting anything other than 16-bit made interval comparison
    # impossible even though the files had the same sample rate.
    if width == 1:  # WAV 8-bit PCM is unsigned.
        samples = tuple((b - 128) << 8 for b in raw)
    elif width == 2:
        samples = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    elif width == 3:
        values = []
        for i in range(0, len(raw) - 2, 3):
            value = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
            if value & 0x800000:
                value -= 0x1000000
            values.append(value >> 8)
        samples = tuple(values)
    elif width == 4:
        values = struct.unpack("<" + "i" * (len(raw) // 4), raw)
        samples = tuple(value >> 16 for value in values)
    else:
        raise ValueError(f"{path}: 不支持 {width * 8}-bit PCM")
    by_channel = [samples[i::channels] for i in range(channels)]
    return rate, by_channel


def dbfs(value):
    return 20.0 * math.log10(max(value, 1.0) / 32768.0)


def stats(values):
    if not values:
        return 0.0, 0
    energy = sum(float(v) * float(v) for v in values)
    return math.sqrt(energy / len(values)), max(abs(v) for v in values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--block-ms", type=float, default=100.0)
    args = parser.parse_args()

    ref_rate, ref_ch = read_wav(args.reference)
    cap_rate, cap_ch = read_wav(args.capture)
    if ref_rate != cap_rate:
        raise SystemExit(f"采样率不一致：reference={ref_rate}, capture={cap_rate}")
    if not ref_ch or not cap_ch:
        raise SystemExit("音频没有可用声道")
    block = max(1, round(ref_rate * args.block_ms / 1000.0))
    ref_frames = len(ref_ch[0])
    cap_frames = len(cap_ch[0])
    count = min(ref_frames, cap_frames)
    print(f"rate={ref_rate}Hz block={block} frames ({args.block_ms:g}ms)")
    print(f"reference={ref_frames / ref_rate:.3f}s capture={cap_frames / cap_rate:.3f}s")
    print("interval,refL_dBFS,capL_dBFS,refR_dBFS,capR_dBFS,capLR_delta_dB")

    valid = 0
    for start in range(0, count, block):
        end = min(start + block, count)
        ref_l, ref_l_peak = stats(ref_ch[0][start:end])
        cap_l, cap_l_peak = stats(cap_ch[0][start:end])
        ref_r, ref_r_peak = stats((ref_ch[1] if len(ref_ch) > 1 else ref_ch[0])[start:end])
        cap_r, cap_r_peak = stats((cap_ch[1] if len(cap_ch) > 1 else cap_ch[0])[start:end])
        # Compare channel balance in the capture.  A zero right channel is
        # represented as -90 dBFS by dbfs(), not as an infinite value.
        delta = abs(dbfs(cap_l) - dbfs(cap_r))
        if cap_l_peak > 64 or cap_r_peak > 64:
            valid += 1
        print(
            f"{start / ref_rate:7.3f},"
            f"{dbfs(ref_l):8.2f},{dbfs(cap_l):8.2f},"
            f"{dbfs(ref_r):8.2f},{dbfs(cap_r):8.2f},{delta:8.2f}"
        )

    print(f"non_silent_capture_intervals={valid}")
    if cap_frames < ref_frames:
        print("warning: capture shorter than reference; missing tail is not scored")


if __name__ == "__main__":
    main()
