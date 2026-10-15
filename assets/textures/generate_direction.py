"""Generate the original 3x4 direction sample using Python 3 stdlib (CC0-1.0)."""
from pathlib import Path
import struct
import zlib


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


# Rows top-to-bottom, pixels left-to-right; RGB is sRGB, alpha is straight.
ROWS = [
    [(255, 255, 255, 255), (255, 128, 0, 255), (0, 0, 0, 255)],
    [(0, 0, 255, 255), (128, 128, 128, 255), (255, 0, 0, 255)],
    [(0, 255, 0, 255), (255, 0, 255, 128), (0, 255, 255, 255)],
    [(17, 34, 51, 0), (255, 255, 0, 255), (128, 0, 255, 255)],
]
raw = b"".join(b"\0" + bytes(channel for pixel in row for channel in pixel) for row in ROWS)
header = struct.pack(">IIBBBBB", 3, 4, 8, 6, 0, 0, 0)
data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 0)) + chunk(b"IEND", b""))
Path(__file__).with_name("direction_rgba.png").write_bytes(data)
