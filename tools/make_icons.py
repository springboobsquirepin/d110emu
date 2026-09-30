#!/usr/bin/env python3
"""D110Emu's icons from the D-110 logo (res/d110logo2ac.png): the light blue wordmark on a dark tile, as

    res/D110Emu.ico   Windows: 16 to 256 pixels (256 as PNG, the others as 32-bit bitmaps), for D110Emu.exe and
                      D110EmuTUI.exe (res/D110Emu.rc)
    res/D110Emu.icns  macOS: 16 to 512 points (32 to 1024 pixels) as PNG, for D110Emu.app (Contents/Resources,
                      CFBundleIconFile)

The builds only copy these files, so building needs no Python. To make them again after changing the logo or the look
below, with Pillow and numpy (a temporary virtual environment will do):

    python3 -m venv /tmp/icons && /tmp/icons/bin/pip install pillow numpy
    /tmp/icons/bin/python tools/make_icons.py [--preview DIR]

The Mac icon follows Apple's app icon template: an 824 x 824 rounded square with continuous corners (radius 185.4) in
the middle of a 1024 x 1024 canvas, and nothing outside it (no shadow). macOS 26 puts an icon whose artwork leaves that
shape on a grey tile of its own. The Windows icon is the same tile with smaller margins.
"""

import argparse
import io
import os
import struct
import sys

import numpy
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO = os.path.join(ROOT, "res", "d110logo2ac.png")

TILE_TOP = (0x2C, 0x2F, 0x34)     # The tile's gradient: the D-110's dark panel, lit from above
TILE_BOTTOM = (0x15, 0x17, 0x1A)
RIM_TOP = (0x4E, 0x53, 0x5B)      # A thin lighter edge, so the tile shows on dark backgrounds (64 pixels and up)
RIM_BOTTOM = (0x26, 0x29, 0x2E)
RIM_WIDTH = 0.007                 # Of the tile's side
SUPERSAMPLING = 4                 # The tile's edge is drawn this much larger, then averaged down

# Apple's continuous corner (as UIBezierPath draws it for iOS 7 and later, and the macOS app icon template's shape):
# three cubic curves from 1.528665 radii along one edge to as far along the next, in units of the radius, from the
# corner. Each point is (along the first edge, along the second).
CORNER = [
    ((1.52866483, 0.0), (1.08849323, 0.0), (0.86840689, 0.0), (0.66993427, 0.06549600)),
    ((0.63149399, 0.07491100), (0.37282392, 0.16905899), (0.16906013, 0.37282401), (0.07491176, 0.63149399)),
    ((0.06549600, 0.66993427), (0.0, 0.86840701), (0.0, 1.08849299), (0.0, 1.52866483)),
]
RADIUS = 185.4 / 824.0  # Of the tile's side


def cubic(p0, p1, p2, p3, steps):
    points = []
    for i in range(steps + 1):
        t = i / steps
        u = 1.0 - t
        points.append((
            u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
            u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1],
        ))
    return points


def tile_outline(left, top, size):
    """The rounded square's outline, clockwise from the top edge, as polygon points."""
    radius = min(RADIUS * size, size / 2.0 / 1.52866483)
    right = left + size
    bottom = top + size
    # (corner x, corner y, direction of the first edge from the corner, direction of the second)
    corners = [
        (right, top, (-1, 0), (0, 1)),     # Top right: from the top edge down to the right edge
        (right, bottom, (0, -1), (-1, 0)),  # Bottom right
        (left, bottom, (1, 0), (0, -1)),   # Bottom left
        (left, top, (0, 1), (1, 0)),       # Top left
    ]
    points = []
    for cx, cy, first, second in corners:
        for segment in CORNER:
            mapped = [(cx + radius * (a * first[0] + b * second[0]), cy + radius * (a * first[1] + b * second[1])) for a, b in segment]
            points.extend(cubic(*mapped, steps=24))
    return points


def logo_mask(logo, width):
    """The wordmark's alpha at `width` pixels wide (its colour is added when it is placed)."""
    height = max(1, round(logo.height * width / logo.width))
    return logo.getchannel("A").resize((width, height), Image.LANCZOS)


def tile_mask(size, left, tile_size):
    """The rounded square's coverage (0-255) on a size x size canvas, its edge anti-aliased."""
    big = size * SUPERSAMPLING
    points = numpy.array(tile_outline(left * SUPERSAMPLING, left * SUPERSAMPLING, tile_size * SUPERSAMPLING))
    x0, y0 = points[:, 0], points[:, 1]
    x1, y1 = numpy.roll(x0, -1), numpy.roll(y0, -1)
    # Each row of the large mask: the pixels whose centres are between the outline's crossings of the row's centre (the
    # shape is convex). Pillow's polygon() counts both edge pixels, which would make the tile a quarter pixel wider.
    mask = numpy.zeros((big, big), numpy.uint8)
    centres = numpy.arange(big) + 0.5
    for row in range(big):
        y = row + 0.5
        crossing = ((y0 <= y) & (y < y1)) | ((y1 <= y) & (y < y0))
        if numpy.count_nonzero(crossing) < 2:
            continue
        xs = x0[crossing] + (y - y0[crossing]) * (x1[crossing] - x0[crossing]) / (y1[crossing] - y0[crossing])
        mask[row, (centres >= xs.min()) & (centres <= xs.max())] = 255
    return Image.fromarray(mask, "L").resize((size, size), Image.BOX)


def gradient(size, top, bottom):
    column = Image.new("RGB", (1, size))
    for y in range(size):
        t = (y + 0.5) / size
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize((size, size), Image.NEAREST)


def render(size, tile_size, logo_width, logo, colour):
    """An icon of size x size: the tile (tile_size wide, centred) with the wordmark (logo_width wide) in its middle."""
    left = (size - tile_size) / 2.0
    mask = tile_mask(size, left, tile_size)
    icon = gradient(size, TILE_TOP, TILE_BOTTOM)
    if size >= 64:
        # The rim: the tile less a slightly smaller one inside it.
        rim = max(1.0, tile_size * RIM_WIDTH)
        inner = tile_mask(size, left + rim, tile_size - 2 * rim)
        icon = Image.composite(icon, gradient(size, RIM_TOP, RIM_BOTTOM), inner)
    icon = icon.convert("RGBA")
    icon.putalpha(mask)

    alpha = logo_mask(logo, logo_width)
    wordmark = Image.new("RGBA", alpha.size, colour + (255,))
    wordmark.putalpha(alpha)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    layer.paste(wordmark, ((size - alpha.width) // 2, (size - alpha.height) // 2))
    return Image.alpha_composite(icon, layer)


def mac_icon(size, logo, colour):
    tile = round(size * 824 / 1024)
    return render(size, tile, round(tile * 0.80), logo, colour)


def windows_icon(size, logo, colour):
    # Small icons fill their square (a margin would leave the wordmark a pixel or two tall); larger ones keep a little.
    tile = size if size <= 32 else size - 2 * round(size * 0.04)
    fraction = 0.92 if size <= 32 else 0.84
    return render(size, tile, round(tile * fraction), logo, colour)


def png_bytes(image):
    out = io.BytesIO()
    image.save(out, "PNG", optimize=True)
    return out.getvalue()


def dib_bytes(image):
    """A 32-bit icon bitmap: BITMAPINFOHEADER (twice the height, for the AND mask), BGRA rows bottom-up, the AND mask."""
    width, height = image.size
    pixels = image.load()
    rows = []
    for y in reversed(range(height)):
        row = bytearray()
        for x in range(width):
            r, g, b, a = pixels[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_stride = ((width + 31) // 32) * 4
    mask = bytearray()
    for y in reversed(range(height)):
        row = bytearray(mask_stride)
        for x in range(width):
            if pixels[x, y][3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    image_size = width * height * 4 + len(mask)
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, image_size, 0, 0, 0, 0)
    return header + b"".join(rows) + bytes(mask)


def write_ico(path, images):
    entries = []
    data = []
    offset = 6 + 16 * len(images)
    for image in images:
        size = image.width
        blob = png_bytes(image) if size >= 256 else dib_bytes(image)
        entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset))
        data.append(blob)
        offset += len(blob)
    with open(path, "wb") as out:
        out.write(struct.pack("<HHH", 0, 1, len(images)))
        out.write(b"".join(entries))
        out.write(b"".join(data))


# The icns slots that hold PNG data, with their pixel sizes (@2x slots hold twice the points), as Apple's iconutil and
# Pillow write them.
ICNS_SLOTS = [(b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512)]


def write_icns(path, images):
    blobs = {size: png_bytes(image) for size, image in images.items()}
    entries = [(kind, blobs[size]) for kind, size in ICNS_SLOTS]
    toc = b"TOC " + struct.pack(">I", 8 + 8 * len(entries)) + b"".join(kind + struct.pack(">I", 8 + len(blob)) for kind, blob in entries)
    body = toc + b"".join(kind + struct.pack(">I", 8 + len(blob)) + blob for kind, blob in entries)
    with open(path, "wb") as out:
        out.write(b"icns" + struct.pack(">I", 8 + len(body)))
        out.write(body)


def main():
    parser = argparse.ArgumentParser(description="Makes res/D110Emu.ico and res/D110Emu.icns from res/d110logo2ac.png")
    parser.add_argument("--preview", help="also save each size as a PNG in this folder")
    args = parser.parse_args()

    logo = Image.open(LOGO).convert("RGBA")
    # The wordmark's colour: its opaque pixels' (the file's is #BBDFFF).
    opaque = [logo.getpixel((x, y))[:3] for y in range(logo.height) for x in range(logo.width) if logo.getpixel((x, y))[3] == 255]
    colour = max(set(opaque), key=opaque.count) if opaque else (0xBB, 0xDF, 0xFF)

    windows = [windows_icon(size, logo, colour) for size in (16, 20, 24, 32, 40, 48, 64, 256)]
    write_ico(os.path.join(ROOT, "res", "D110Emu.ico"), windows)
    mac = {size: mac_icon(size, logo, colour) for size in (32, 64, 128, 256, 512, 1024)}
    write_icns(os.path.join(ROOT, "res", "D110Emu.icns"), mac)

    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        for image in windows:
            image.save(os.path.join(args.preview, "windows-%d.png" % image.width))
        for size, image in mac.items():
            image.save(os.path.join(args.preview, "mac-%d.png" % size))
    print("res/D110Emu.ico: %s" % ", ".join(str(image.width) for image in windows))
    print("res/D110Emu.icns: %s" % ", ".join(str(size) for size in sorted(mac)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
