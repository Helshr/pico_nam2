#!/usr/bin/env python3
"""离线测试：测试音送入 Pico NAM，录下返回 WAV，再用 Mac 扬声器播放。"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def tool(name):
    return shutil.which(name) or next(
        (str(Path(p) / name) for p in ("/opt/homebrew/bin", "/usr/local/bin")
         if (Path(p) / name).exists()), None)


SWITCH = tool("SwitchAudioSource")
FFMPEG = tool("ffmpeg")
FFPLAY = tool("ffplay")


def run(*args):
    return subprocess.run(args, check=False)


def current(kind):
    return subprocess.check_output([SWITCH, "-c", "-t", kind], text=True).strip()


def set_device(name, kind):
    run(SWITCH, "-s", name, "-t", kind)


def main():
    if not all((SWITCH, FFMPEG, FFPLAY)):
        sys.exit("需要安装：brew install switchaudio-osx ffmpeg")

    old_out, old_in = current("output"), current("input")
    result = Path(tempfile.mktemp(prefix="pico_nam_", suffix=".wav"))
    try:
        set_device("Pico NAM", "output")
        set_device("Pico NAM", "input")
        print("正在录制板子处理后的音频……")
        capture = subprocess.Popen([
            FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "avfoundation", "-i", ":0", "-t", "4",
            "-ar", "48000", "-ac", "2", "-c:a", "pcm_s16le", str(result),
        ])
        # 板子接收并处理 2 秒 440 Hz 测试音。
        run(FFPLAY, "-hide_banner", "-loglevel", "error", "-nodisp",
            "-autoexit", "-f", "lavfi",
            "-i", "sine=frequency=440:sample_rate=48000:duration=2")
        capture.wait()
    finally:
        set_device(old_out, "output")
        set_device(old_in, "input")

    print(f"处理完成，正在播放：{result}")
    run(FFPLAY, "-hide_banner", "-loglevel", "error", "-nodisp",
        "-autoexit", str(result))
    print("测试完成；录音文件保留在：", result)


if __name__ == "__main__":
    main()
