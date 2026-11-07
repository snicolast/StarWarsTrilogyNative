"""Saves the largest picture of a .ico (the ones holding PNGs, as tools/make_icon.py
writes them) as a .png: macOS builds the application's icon from it.
Usage: python tools/ico_to_png.py res/app.ico out.png
"""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    count = struct.unpack("<H", data[4:6])[0]
    best = (0, 0, 0)
    for i in range(count):
        e = data[6 + 16 * i:22 + 16 * i]
        size, offset = struct.unpack("<II", e[8:16])
        best = max(best, (e[0] or 256, size, offset))
    _, size, offset = best
    open(sys.argv[2], "wb").write(data[offset:offset + size])
    return 0


if __name__ == "__main__":
    sys.exit(main())
