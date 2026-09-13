#!/usr/bin/env python3
#
# icon-prep.py - normalize a dropped-in toolbar icon for icons/.
#
# Icon exports routinely arrive oversized and with the background painted
# a flat white instead of left transparent (PNG colortype 2, no alpha).
# On a themed toolbar that renders as a white tile around the artwork.
# This resizes to the 512x512 the rest of icons/ uses and keys the
# background out to alpha.
#
# The key is a flood fill INWARD FROM THE BORDER, not a global "white is
# transparent" threshold: the artwork's own paper-white interior must
# survive, and it does because the outline walls the fill off.  Tolerance
# is deliberately tight for the same reason - the document interiors in
# this set sit at ~233, and a loose tolerance swallows them.
#
# Inputs:
#   paths - PNG files, rewritten IN PLACE
#   --size  N    square size to fit within, aspect kept (default 512)
#   --tol   N    how far from white still counts as background (default 12)
#
# Output:
#   0 on success, 1 if a file could not be read or written.
#
# Requires Pillow:  /opt/local/bin/python3 -m pip install --user Pillow
#
import argparse
import sys
from collections import deque

from PIL import Image


def key_background(im, tol):
    """
    Make the border-connected near-white region transparent.

    Inputs:
      im  - RGBA Image, modified in place
      tol - a pixel counts as background when min(r,g,b) >= 255 - tol

    Output:
      number of pixels keyed.
    """
    w, h = im.size
    px = im.load()
    seen = bytearray(w * h)
    queue = deque([(x, y) for x in range(w) for y in (0, h - 1)] +
                  [(x, y) for y in range(h) for x in (0, w - 1)])
    keyed = 0

    while queue:
        x, y = queue.popleft()
        i = y * w + x
        if seen[i]:
            continue
        r, g, b, _ = px[x, y]
        v = min(r, g, b)
        if v < 255 - tol:
            continue
        seen[i] = 1
        # Pure white goes fully clear; an antialiased edge pixel keeps a
        # matching sliver of alpha, so the outline stays smooth.
        px[x, y] = (r, g, b, 255 - v)
        keyed += 1
        if x:
            queue.append((x - 1, y))
        if x < w - 1:
            queue.append((x + 1, y))
        if y:
            queue.append((x, y - 1))
        if y < h - 1:
            queue.append((x, y + 1))

    return keyed


def prep(path, size, tol):
    """
    Resize and key one icon in place.  Returns True on success.
    """
    try:
        im = Image.open(path)
    except OSError as exc:
        print(f"{path}: {exc}", file=sys.stderr)
        return False

    before = im.size
    # Drop any existing alpha first: a re-run must recompute the key from
    # the colour planes, never compound a previous pass's alpha.
    im = im.convert('RGB')
    im.thumbnail((size, size), Image.LANCZOS)
    im = im.convert('RGBA')
    keyed = key_background(im, tol)

    try:
        im.save(path)
    except OSError as exc:
        print(f"{path}: {exc}", file=sys.stderr)
        return False

    print(f"{path}: {before[0]}x{before[1]} -> {im.size[0]}x{im.size[1]}, "
          f"{100 * keyed / (im.size[0] * im.size[1]):.1f}% keyed")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('paths', nargs='+', help="PNG files, rewritten in place")
    ap.add_argument('--size', type=int, default=512)
    ap.add_argument('--tol', type=int, default=12)
    args = ap.parse_args()

    ok = all(prep(p, args.size, args.tol) for p in args.paths)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
