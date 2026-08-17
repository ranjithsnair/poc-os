#!/usr/bin/env python3
# Generates the raw pixel assets gui/libgui/gfx.c's gfx_load_raw()/
# gfx_load_raw_rgba() read at runtime (usr/share/wallpaper.raw, usr/
# share/icons/terminal.raw) - poc-os has no PNG/JPEG decoder of its
# own, so images are converted once, host-side, into a trivial format
# it can read with a handful of open()/read() calls: 4-byte magic
# ("PRAW" for opaque RGB, "PRGA" for RGBA), u32 width, u32 height, then
# width*height*3 (or *4) raw bytes, row-major, no row padding.
#
# Host-side tool only, like tools/genfont.py - not part of the poc-os
# build itself, and the generated files are committed so building
# poc-os never needs Python/PIL. Re-run this and commit the result if
# the source wallpaper/icon ever changes.
#
# Usage:
#   python3 tools/genraw.py wallpaper <src.jpg> <out.raw> <width> <height>
#   python3 tools/genraw.py icon <src.png> <out.raw> <width> <height>
import struct
import sys

from PIL import Image

MAGIC_RGB = b"PRAW"
MAGIC_RGBA = b"PRGA"


def cover_resize(im, w, h):
    # Scale to fully cover w x h (preserving aspect ratio), then
    # center-crop the overflow - the usual "wallpaper fill" behavior,
    # avoiding the stretch distortion a plain resize(w,h) would cause
    # when the source's aspect ratio doesn't match the target's.
    src_w, src_h = im.size
    scale = max(w / src_w, h / src_h)
    new_w, new_h = round(src_w * scale), round(src_h * scale)
    im = im.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - w) // 2
    top = (new_h - h) // 2
    return im.crop((left, top, left + w, top + h))


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        sys.exit(1)
    kind, src, out, w, h = sys.argv[1:]
    w, h = int(w), int(h)

    im = Image.open(src)
    if kind == "wallpaper":
        im = cover_resize(im.convert("RGB"), w, h)
        magic, mode, nbytes = MAGIC_RGB, "RGB", 3
    elif kind == "icon":
        im = im.convert("RGBA").resize((w, h), Image.LANCZOS)
        magic, mode, nbytes = MAGIC_RGBA, "RGBA", 4
    else:
        print(f"unknown kind {kind!r} (expected wallpaper or icon)")
        sys.exit(1)

    data = im.tobytes()
    assert len(data) == w * h * nbytes
    with open(out, "wb") as f:
        f.write(magic)
        f.write(struct.pack("<II", w, h))
        f.write(data)
    print(f"wrote {out}: {w}x{h} {mode}, {len(data)} pixel bytes")


if __name__ == "__main__":
    main()
