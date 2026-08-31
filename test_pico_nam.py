#!/usr/bin/env python3
"""听音测试：把测试音送入 Pico NAM，并把返回音频播放到 Mac 扬声器。"""

import argparse
import fcntl
import os
import re
import signal
import shutil
import subprocess
import sys
import time
from pathlib import Path


def find_tool(name):
    return shutil.which(name) or next(
        (str(Path(p) / name) for p in ("/opt/homebrew/bin", "/usr/local/bin")
         if (Path(p) / name).exists()), None)


SWITCH = find_tool("SwitchAudioSource")
FFPLAY = find_tool("ffplay")
FFMPEG = find_tool("ffmpeg")
LOCK_PATH = Path("/tmp/pico-nam-audio-test.lock")


def acquire_single_instance_lock():
    """Keep two test runs from playing through the Pico at the same time."""
    lock = LOCK_PATH.open("a+")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.close()
        sys.exit("Pico NAM 测试已经在运行；请先在原来的终端按 Ctrl+C。")
    lock.seek(0)
    lock.truncate()
    lock.write(f"{os.getpid()}\n")
    lock.flush()
    return lock


def interrupted_by_signal(signum, frame):
    raise KeyboardInterrupt


def run(*args, capture=False):
    return subprocess.run(args, check=True, text=True,
                          capture_output=capture)


def current(kind):
    return run(SWITCH, "-c", "-t", kind, capture=True).stdout.strip()


def set_device(name, kind):
    run(SWITCH, "-s", name, "-t", kind)


def audiotoolbox_output_index(device_name):
    """Resolve a CoreAudio output name to FFmpeg's AudioToolbox index."""
    probe = subprocess.Popen([
        FFMPEG, "-hide_banner", "-loglevel", "info",
        "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo",
        "-list_devices", "true", "-f", "audiotoolbox", "dummy",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(0.7)
    probe.kill()
    _, stderr = probe.communicate()
    for line in stderr.splitlines():
        match = re.search(r"\[(\d+)\]", line)
        if match and device_name in line:
            return int(match.group(1))
    raise RuntimeError(f"AudioToolbox 找不到输出设备：{device_name}")


def stop_process(process):
    if not process or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=2)


def main():
    instance_lock = acquire_single_instance_lock()
    signal.signal(signal.SIGTERM, interrupted_by_signal)
    signal.signal(signal.SIGHUP, interrupted_by_signal)

    parser = argparse.ArgumentParser(
        description="把音频送入 Pico NAM，并从 Mac 扬声器监听返回音频。"
    )
    parser.add_argument("audio", nargs="?", help="可选的 mp3/wav 文件；省略时播放 440 Hz")
    args = parser.parse_args()

    if not SWITCH or not FFPLAY or not FFMPEG:
        sys.exit("需要先安装 SwitchAudioSource 和 ffplay（brew install switchaudio-osx ffmpeg）")

    audio_path = Path(args.audio).expanduser().resolve() if args.audio else None
    if audio_path and not audio_path.is_file():
        sys.exit(f"找不到音频文件：{audio_path}")

    old_out, old_in = current("output"), current("input")
    capture = None
    monitor = None
    playback_decoder = None
    try:
        # 先打开 Mac 扬声器监听，再把系统输出切到 Pico NAM。
        set_device("MacBook Pro Speakers", "output")
        speaker_index = audiotoolbox_output_index("MacBook Pro Speakers")
        pico_index = audiotoolbox_output_index("Pico NAM")
        set_device("Pico NAM", "input")
        capture = subprocess.Popen([
            FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "avfoundation",
            "-i", ":0", "-f", "s16le", "-ar", "48000", "-ac", "2", "-",
        ], stdout=subprocess.PIPE, start_new_session=True)
        monitor = subprocess.Popen([
            FFMPEG, "-hide_banner", "-loglevel", "error",
            "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "-",
            "-audio_device_index", str(speaker_index),
            "-f", "audiotoolbox", "pico-nam-monitor",
        ], stdin=capture.stdout, start_new_session=True)
        capture.stdout.close()
        time.sleep(0.2)
        if audio_path:
            print(f"正在通过 Pico NAM 播放：{audio_path.name}")
            print("短按 BOOTSEL 对比效果；LED 亮=NAM，LED 灭=旁路。")
            # Pico exposes only 48 kHz USB audio. Decode/resample files such as
            # 44.1 kHz MP3 to raw 48 kHz stereo before handing them to ffplay.
            playback_decoder = subprocess.Popen([
                FFMPEG, "-hide_banner", "-loglevel", "error", "-i", str(audio_path),
                "-ar", "48000", "-ac", "2",
                "-audio_device_index", str(pico_index),
                "-f", "audiotoolbox", "pico-nam-playback",
            ], start_new_session=True)
            playback_decoder.wait()
        else:
            print("正在测试；你应该听到 440 Hz 测试音。按 Ctrl+C 结束。")
            while True:
                playback_decoder = subprocess.Popen([
                    FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                    "-i", "sine=frequency=440:sample_rate=48000:duration=2",
                    "-audio_device_index", str(pico_index),
                    "-f", "audiotoolbox", "pico-nam-tone",
                ], start_new_session=True)
                playback_decoder.wait()
    except KeyboardInterrupt:
        pass
    finally:
        stop_process(playback_decoder)
        stop_process(monitor)
        stop_process(capture)
        try:
            set_device(old_out, "output")
            set_device(old_in, "input")
        except subprocess.CalledProcessError:
            pass
        instance_lock.close()
        print("已恢复原来的音频设备。")


if __name__ == "__main__":
    main()
