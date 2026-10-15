"""Reproduce original PNG fixtures with Python 3 standard library only (CC0-1.0)."""
import hashlib
import json
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[3]
SIG = b"\x89PNG\r\n\x1a\n"
PIXELS = bytes([
    255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
    0, 255, 255, 255, 255, 0, 255, 255, 128, 128, 128, 255, 255, 255, 255, 255,
    255, 128, 0, 255, 128, 0, 255, 128, 17, 34, 51, 0, 0, 0, 0, 255,
])


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(width, height, depth, color, raw_rows, extra=b"", interlace=0):
    ihdr = struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, interlace)
    return SIG + chunk(b"IHDR", ihdr) + extra + chunk(b"IDAT", zlib.compress(raw_rows, 0)) + chunk(b"IEND", b"")


rgba = png(4, 3, 8, 6, b"".join(b"\0" + PIXELS[y * 16:(y + 1) * 16] for y in range(3)))
cases = {
    "gray8": png(2, 1, 8, 0, bytes([0, 0, 128])),
    "gray_alpha8": png(2, 1, 8, 4, bytes([0, 128, 64, 255, 0])),
    "rgb8": png(2, 1, 8, 2, bytes([0, 255, 0, 0, 128, 128, 128])),
    "palette8": png(2, 1, 8, 3, bytes([0, 0, 1]), chunk(b"PLTE", bytes([255, 0, 0, 0, 255, 0]))),
    "rgb16": png(1, 1, 16, 2, bytes([0, 255, 255, 0, 0, 128, 128])),
    "rgba16": png(1, 1, 16, 6, bytes([0, 255, 255, 0, 0, 128, 128, 255, 255])),
    # A 1x1 Adam7 image has exactly one pixel in the first pass; other passes are empty.
    "adam7": png(1, 1, 8, 6, bytes([0, 255, 0, 0, 255]), interlace=1),
    "max_width": png(4096, 1, 8, 0, b"\0" + bytes([128]) * 4096),
    "max_height": png(1, 4096, 8, 0, bytes([0, 128]) * 4096),
    # One black pixel remains a valid packed sample at depths 1, 2 and 4.
    # Tests derive gray2/gray4 by changing this IHDR and recomputing its CRC.
    "gray1": png(1, 1, 1, 0, bytes([0, 0])),
}
# ICC bytes are ancillary to this decoder; no ICC color conversion is offered.
cases["icc_metadata"] = rgba[:33] + chunk(b"iCCP", b"fixture\0\0" + zlib.compress(b"original test profile")) + rgba[33:]
compressor = zlib.compressobj(level=0, wbits=-15)
raw = bytes([0, 0, 0, 255, 255])  # filter=0, BGRA red for Apple's raw-deflate variant
cases["cgbi"] = (SIG + chunk(b"CgBI", bytes([64, 160, 96, 130]))
                 + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
                 + chunk(b"IDAT", compressor.compress(raw) + compressor.flush()) + chunk(b"IEND", b""))

manifest = {}
for name, data in cases.items():
    (ROOT / f"{name}.png").write_bytes(data)
    manifest[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
(ROOT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
asset = PROJECT / "assets/textures/reference_rgba.png"
asset.parent.mkdir(parents=True, exist_ok=True)
asset.write_bytes(rgba)
assert len(rgba) == 119
assert hashlib.sha256(rgba).hexdigest() == "a609d42b2ac0a5fab2e9ffac52387d4c479018e729e46d9536598e213baa10b3"
print(f"Generated {len(cases)} fixtures and {asset}")
