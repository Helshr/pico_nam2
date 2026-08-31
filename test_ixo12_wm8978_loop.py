#!/usr/bin/env python3
"""IXO12 + WM8978 回环测试。

把仓库里的 DI 吉他 WAV 播到 Steinberg IXO12 输出，
同时从 Steinberg IXO12 输入录回音频，便于确认整条链路是否通。
"""

import argparse
import fcntl
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
DEFAULT_AUDIO = REPO_ROOT / "test_audio" / "ui_public_inputs_Mayer - Guitar.wav"
DEFAULT_DEVICE = "Steinberg IXO12"
LOCK_PATH = Path("/tmp/ixo12-wm8978-loop.lock")


def find_tool(name):
    return shutil.which(name) or next(
        (str(Path(p) / name) for p in ("/opt/homebrew/bin", "/usr/local/bin")
         if (Path(p) / name).exists()),
        None,
    )


SWITCH = find_tool("SwitchAudioSource")
FFPLAY = find_tool("ffplay")
FFMPEG = find_tool("ffmpeg")


def acquire_single_instance_lock():
    lock = LOCK_PATH.open("a+")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.close()
        sys.exit("回环测试已经在运行；请先在原来的终端按 Ctrl+C。")
    lock.seek(0)
    lock.truncate()
    lock.write(f"{os.getpid()}\n")
    lock.flush()
    return lock


def run(*args, capture=False, check=True):
    return subprocess.run(
        args,
        check=check,
        text=True,
        capture_output=capture,
    )


def current(kind):
    return run(SWITCH, "-c", "-t", kind, capture=True).stdout.strip()


def set_device(name, kind):
    run(SWITCH, "-s", name, "-t", kind)


def audiotoolbox_output_index(device_name):
    # AudioToolbox 在切换默认输出后可能需要数百毫秒重新枚举设备。
    # 轮询而不是固定一次探测，避免把临时的 macOS 重枚举误判为设备不存在。
    deadline = time.monotonic() + 4.0
    while time.monotonic() < deadline:
        probe = subprocess.Popen(
            [
                FFMPEG, "-hide_banner", "-loglevel", "info", "-f", "lavfi",
                "-i", "anullsrc=r=48000:cl=stereo", "-list_devices", "true",
                "-f", "audiotoolbox", "dummy",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        time.sleep(0.35)
        probe.kill()
        _, stderr = probe.communicate()
        for line in stderr.splitlines():
            match = re.search(r"\[(\d+)\]", line)
            if match and device_name in line:
                return int(match.group(1))
        time.sleep(0.15)
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


def analyze_volume(path):
    probe = run(
        FFMPEG,
        "-hide_banner",
        "-loglevel",
        "info",
        "-i",
        str(path),
        "-af",
        "volumedetect",
        "-f",
        "null",
        "-",
        capture=True,
        check=False,
    )
    mean = None
    peak = None
    for line in (probe.stderr or "").splitlines():
        mean_match = re.search(r"mean_volume:\s*(-?\d+(?:\.\d+)?) dB", line)
        peak_match = re.search(r"max_volume:\s*(-?\d+(?:\.\d+)?) dB", line)
        if mean_match:
            mean = float(mean_match.group(1))
        if peak_match:
            peak = float(peak_match.group(1))
    return mean, peak


def classify_level(mean, peak):
    """给出简单的回环诊断，避免把纯底噪误判成 NAM 输出。"""
    if mean is None or peak is None:
        return "无法读取电平"
    if peak >= -1.0:
        return "削波：请降低 IXO12 输入增益或输出电平"
    if peak <= -45.0 or mean <= -70.0:
        return "近似静音/只有底噪：检查 WM8978 输入、I2S 链路和固件"
    if peak - mean < 6.0:
        return "动态范围很小：疑似持续噪声或直流偏置"
    return "有效音频电平"


def main():
    lock = acquire_single_instance_lock()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGHUP, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))

    parser = argparse.ArgumentParser(description="IXO12 + WM8978 回环测试")
    parser.add_argument(
        "--audio",
        default=str(DEFAULT_AUDIO),
        help="要播放的 DI WAV 文件",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=12.0,
        help="录音时长，默认 12 秒",
    )
    parser.add_argument(
        "--playback-gain",
        type=float,
        default=0.01,
        help="播放到 WM8978 前的数字增益，默认 0.01（NAM 高增益链路更安全）",
    )
    parser.add_argument(
        "--output-device",
        default=DEFAULT_DEVICE,
        help="macOS 输出设备名",
    )
    parser.add_argument(
        "--input-device",
        default=DEFAULT_DEVICE,
        help="macOS 输入设备名",
    )
    args = parser.parse_args()

    if not all((SWITCH, FFPLAY, FFMPEG)):
        sys.exit("需要先安装 SwitchAudioSource、ffplay 和 ffmpeg")

    audio_path = Path(args.audio).expanduser().resolve()
    if not audio_path.is_file():
        sys.exit(f"找不到音频文件：{audio_path}")

    old_out, old_in = current("output"), current("input")
    capture = None
    playback = None
    recorded = Path(tempfile.mktemp(prefix="ixo12_wm8978_", suffix=".wav"))
    try:
        # 某些 macOS 音频设备在切换输入时会自动重置输出；最后再设置输出，
        # 避免 IXO12 被系统悄悄切回耳机。
        set_device(args.input_device, "input")
        set_device(args.output_device, "output")

        output_index = audiotoolbox_output_index(args.output_device)
        # ffmpeg 的设备探测在 macOS 上可能把系统默认输出切回耳机；
        # 播放前再次设置，确保 DI 信号确实送入 IXO12/WM8978。
        set_device(args.output_device, "output")
        time.sleep(0.3)
        print(f"输出设备：{args.output_device} (AudioToolbox index {output_index})")
        print(f"输入设备：{args.input_device}")
        print(f"播放文件：{audio_path}")
        print(f"录音文件：{recorded}")
        print("先把 IXO12 的输入增益放低，再开始。")

        capture = subprocess.Popen(
            [
                FFMPEG,
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-f",
                "avfoundation",
                "-i",
                ":0",
                "-t",
                str(args.duration),
                "-ar",
                "48000",
                "-ac",
                "2",
                "-c:a",
                "pcm_s16le",
                str(recorded),
            ],
            start_new_session=True,
        )

        time.sleep(0.3)
        playback = subprocess.Popen(
            [
                FFMPEG,
                "-hide_banner",
                "-loglevel",
                "error",
                "-stream_loop",
                "-1",
                "-i",
                str(audio_path),
                "-t",
                str(args.duration),
                "-af",
                f"volume={args.playback_gain}",
                "-ar",
                "48000",
                "-ac",
                "2",
                "-audio_device_index",
                str(output_index),
                "-f",
                "audiotoolbox",
                "ixo12-wm8978-playback",
            ],
            start_new_session=True,
        )

        playback.wait()
        capture.wait()
    except KeyboardInterrupt:
        pass
    finally:
        stop_process(playback)
        stop_process(capture)
        try:
            set_device(old_out, "output")
            set_device(old_in, "input")
        except subprocess.CalledProcessError:
            pass
        lock.close()

    if recorded.exists():
        mean, peak = analyze_volume(recorded)
        print("回环完成。")
        print(f"录音文件：{recorded}")
        if mean is not None or peak is not None:
            print(
                "电平统计："
                f"mean={mean if mean is not None else 'n/a'} dB, "
                f"peak={peak if peak is not None else 'n/a'} dB"
            )
            print(f"诊断：{classify_level(mean, peak)}")
    else:
        print("没有生成录音文件，链路可能没有起来。")


if __name__ == "__main__":
    main()
