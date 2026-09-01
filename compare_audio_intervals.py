#!/usr/bin/env python3
"""Compare a hardware capture with the known-good FretFlow reference.

The tool intentionally uses only Python's standard library so it can run on a
fresh macOS setup.  It reports every fixed-size interval rather than relying
on a single whole-file peak, which is important when a capture contains a
short valid intro followed by silence, a buzzer, or a feedback burst.
"""

import argparse
import math
import struct
import wave
from pathlib import Path


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


def _mono(channels, channel):
    """Return one analysis channel without requiring numpy/soundfile."""
    if channel == "left":
        return channels[0]
    if channel == "right":
        return channels[1] if len(channels) > 1 else channels[0]
    # Keep the mix at the same scale as an individual channel.  Averaging
    # avoids a 6 dB increase when a stereo capture contains equal L/R data.
    if len(channels) == 1:
        return channels[0]
    count = min(len(channels[0]), len(channels[1]))
    return tuple((channels[0][i] + channels[1][i]) // 2 for i in range(count))


def _frame_db(values, block):
    """RMS dBFS for every complete/partial fixed-size interval."""
    result = []
    for start in range(0, len(values), block):
        rms, _ = stats(values[start : start + block])
        result.append(dbfs(rms))
    return result


def _median(values):
    if not values:
        return 0.0
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def _pearson(left, right):
    count = min(len(left), len(right))
    if count < 2:
        return 0.0
    left = left[:count]
    right = right[:count]
    left_mean = sum(left) / count
    right_mean = sum(right) / count
    numerator = sum((a - left_mean) * (b - right_mean) for a, b in zip(left, right))
    left_energy = sum((a - left_mean) ** 2 for a in left)
    right_energy = sum((b - right_mean) ** 2 for b in right)
    denominator = math.sqrt(left_energy * right_energy)
    return numerator / denominator if denominator else 0.0


def _best_frame_offset(reference, capture, max_offset):
    """Find capture-frame offset with the closest *whole* envelope curve.

    ``offset`` means capture[i + offset] aligns with reference[i].  We score
    demeaned RMS envelopes, so a harmless overall gain difference does not
    make a correctly timed recording look misaligned.
    """
    best_offset = 0
    best_score = -2.0
    for offset in range(-max_offset, max_offset + 1):
        ref_start = max(0, -offset)
        cap_start = max(0, offset)
        count = min(len(reference) - ref_start, len(capture) - cap_start)
        if count < 1:
            continue
        if count == 1:
            # A one-interval file has no variance for Pearson correlation;
            # still allow it to be reported as aligned when its RMS matches.
            score = 1.0 if abs(reference[ref_start] - capture[cap_start]) < 1e-9 else 0.0
        else:
            score = _pearson(
                reference[ref_start : ref_start + count],
                capture[cap_start : cap_start + count],
            )
        if score > best_score:
            best_score = score
            best_offset = offset
    return best_offset, best_score


def compare_audio(reference_path, capture_path, block_ms=100.0, channel="left", max_align_ms=500.0, print_intervals=True):
    """Compare two WAV files over every interval and return numeric metrics.

    This is deliberately a normal Python function so the hardware test can
    call it after each capture.  It compares the complete available timeline,
    including missing tail intervals and unexpected noise during reference
    silence; a single peak value can never hide those failures.
    """
    ref_rate, ref_channels = read_wav(reference_path)
    cap_rate, cap_channels = read_wav(capture_path)
    if ref_rate != cap_rate:
        raise ValueError(f"采样率不一致：reference={ref_rate}, capture={cap_rate}")
    if not ref_channels or not cap_channels:
        raise ValueError("音频没有可用声道")
    block = max(1, round(ref_rate * block_ms / 1000.0))
    ref = _mono(ref_channels, channel)
    cap = _mono(cap_channels, channel)
    ref_db = _frame_db(ref, block)
    cap_db = _frame_db(cap, block)
    max_offset = max(0, round(max_align_ms / block_ms))
    offset, envelope_corr = _best_frame_offset(ref_db, cap_db, max_offset)

    # Determine overlap in reference coordinates after alignment.
    ref_start = max(0, -offset)
    cap_start = max(0, offset)
    overlap = max(0, min(len(ref_db) - ref_start, len(cap_db) - cap_start))
    ref_overlap = ref_db[ref_start : ref_start + overlap]
    cap_overlap = cap_db[cap_start : cap_start + overlap]
    non_silent = [
        cap_value - ref_value
        for ref_value, cap_value in zip(ref_overlap, cap_overlap)
        if ref_value > -70.0
    ]
    gain_db = _median(non_silent)
    corrected_errors = [
        abs((cap_value - gain_db) - ref_value)
        for ref_value, cap_value in zip(ref_overlap, cap_overlap)
        if ref_value > -70.0
    ]
    reference_silence_noise = [
        cap_value
        for ref_value, cap_value in zip(ref_overlap, cap_overlap)
        if ref_value <= -60.0
    ]
    missing = max(0, len(ref_db) - overlap)
    unexpected = sum(
        1
        for ref_value, cap_value in zip(ref_overlap, cap_overlap)
        if ref_value <= -60.0 and cap_value > -50.0
    )

    print(
        f"comparison channel={channel} rate={ref_rate}Hz "
        f"block={block} frames ({block_ms:g}ms)"
    )
    print(
        f"reference={len(ref) / ref_rate:.3f}s capture={len(cap) / cap_rate:.3f}s "
        f"alignment={offset * block_ms:+.1f}ms"
    )
    print(f"envelope_corr={envelope_corr:.4f} gain_offset={gain_db:+.2f}dB")
    if print_intervals:
        print("interval,reference_dBFS,capture_dBFS,gain_corrected_error_dB,status")
        capture_start_ref = -offset
        capture_end_ref = len(cap_db) - offset
        first_index = min(0, capture_start_ref)
        total = max(len(ref_db), capture_end_ref)
        for index in range(first_index, total):
            ref_value = ref_db[index] if 0 <= index < len(ref_db) else None
            cap_index = index + offset
            cap_value = cap_db[cap_index] if 0 <= cap_index < len(cap_db) else None
            if ref_value is None:
                status = "EXTRA_CAPTURE"
                error = "-"
            elif cap_value is None:
                status = "MISSING_CAPTURE"
                error = "-"
            else:
                error_value = abs((cap_value - gain_db) - ref_value)
                error = f"{error_value:.2f}"
                status = "REFERENCE_SILENCE_NOISE" if ref_value <= -60 and cap_value > -50 else "OK"
            ref_text = f"{ref_value:.2f}" if ref_value is not None else "-"
            cap_text = f"{cap_value:.2f}" if cap_value is not None else "-"
            print(f"{index * block_ms / 1000.0:.3f},{ref_text},{cap_text},{error},{status}")
    metrics = {
        "alignment_ms": offset * block_ms,
        "envelope_corr": envelope_corr,
        "gain_offset_db": gain_db,
        "rms_mae_db": sum(corrected_errors) / len(corrected_errors) if corrected_errors else float("inf"),
        "reference_silence_noise_dbfs": max(reference_silence_noise) if reference_silence_noise else -90.31,
        "missing_intervals": missing,
        "unexpected_noise_intervals": unexpected,
        "reference_intervals": len(ref_db),
        "capture_intervals": len(cap_db),
    }
    print(
        f"rms_mae_after_gain={metrics['rms_mae_db']:.2f}dB "
        f"reference_silence_peak={metrics['reference_silence_noise_dbfs']:.2f}dBFS "
        f"missing_intervals={missing} unexpected_noise_intervals={unexpected}"
    )
    if missing:
        print("warning: capture shorter than reference; missing intervals are counted as failures")
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--block-ms", type=float, default=100.0)
    parser.add_argument("--channel", choices=("left", "right", "mix"), default="left")
    parser.add_argument("--max-align-ms", type=float, default=500.0)
    args = parser.parse_args()

    try:
        compare_audio(
            args.reference,
            args.capture,
            block_ms=args.block_ms,
            channel=args.channel,
            max_align_ms=args.max_align_ms,
        )
    except (OSError, ValueError, wave.Error) as exc:
        raise SystemExit(str(exc))


if __name__ == "__main__":
    main()
