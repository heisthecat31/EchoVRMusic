"""Build an EchoVRMusic release: out/release/EchoVRMusicSetup.exe, for a GitHub release.

Usage: python tools/make_release.py

1. Raise VERSION (0.1.0, 0.1.1, 0.2.0, ...) before each release.
2. Run this. It builds the plugin and the setup app, and checks the exe carries that version.
3. On GitHub, make a release tagged v<VERSION> and attach out/release/EchoVRMusicSetup.exe,
   with exactly that file name: Spark looks for it on the latest release to offer the update.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def file_version(path):
    """The exe's FILEVERSION as (major, minor, patch), via the Windows version API."""
    import ctypes
    size = ctypes.windll.version.GetFileVersionInfoSizeW(path, None)
    buf = ctypes.create_string_buffer(size)
    ctypes.windll.version.GetFileVersionInfoW(path, 0, size, buf)
    ptr, length = ctypes.c_void_p(), ctypes.c_uint()
    ctypes.windll.version.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length))
    ms, ls = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint32))[2:4]
    return (ms >> 16, ms & 0xFFFF, ls >> 16)


def main():
    version = open(os.path.join(ROOT, "VERSION")).read().strip()
    want = tuple(int(p) for p in version.split("."))
    bat = os.path.join(ROOT, "setup", "build_setup.bat")
    if subprocess.call(["cmd", "/c", bat]) != 0:
        sys.exit("build failed")
    exe = os.path.join(ROOT, "out", "EchoVRMusicSetup.exe")
    got = file_version(exe)
    if got != want:
        sys.exit("the exe says %s, VERSION says %s" % (".".join(map(str, got)), version))
    rel = os.path.join(ROOT, "out", "release")
    os.makedirs(rel, exist_ok=True)
    shutil.copy2(exe, os.path.join(rel, "EchoVRMusicSetup.exe"))
    print("\nRelease ready: %s" % os.path.join(rel, "EchoVRMusicSetup.exe"))
    print("GitHub release tag: v%s  (asset name: EchoVRMusicSetup.exe)" % version)


if __name__ == "__main__":
    main()
