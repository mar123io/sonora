#!/usr/bin/env python3
"""Draw src/shell/sonora.ico.

The application had no icon at all, which Windows notices in more places than a
title bar: Explorer, the taskbar, Task Manager, and the system's media panel,
which until now called Sonora "Unknown app".

It is a script rather than a checked-in drawing so that the icon is
reproducible and so that changing it is a diff of intent rather than a binary
blob nobody can review. Run it when the shape changes; the .ico it produces is
committed, because the build must not need Python to make an icon.

Usage:
    python tools/make_icon.py [--out src/shell/sonora.ico]
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

# The interface's own palette, so the icon and the window agree.
BACKGROUND = (24, 24, 24, 255)
ACCENT = (110, 168, 254, 255)
MUTED = (154, 160, 166, 255)

# Four bars of a level meter: the one picture that says "sound" without being a
# note, a speaker or anything anybody else's icon already is. The values are
# fractions of the canvas, so every size is the same drawing rather than a
# resampled one -- at 16 pixels a resampled icon is mud.
BARS = [(0.26, 0.42), (0.42, 0.68), (0.58, 0.86), (0.74, 0.54)]


def draw(size: int) -> Image.Image:
    # Drawn at four times the size and reduced: the rounded corners and the bar
    # ends are the parts that show the jaggedness otherwise.
    scale = 4
    canvas = size * scale
    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    painter = ImageDraw.Draw(image)

    radius = canvas * 0.22
    painter.rounded_rectangle([0, 0, canvas - 1, canvas - 1], radius=radius, fill=BACKGROUND)

    width = canvas * 0.10
    for index, (centre, height) in enumerate(BARS):
        x = canvas * centre
        top = canvas * (0.5 - height / 2)
        bottom = canvas * (0.5 + height / 2)
        colour = ACCENT if index != 3 else MUTED
        painter.rounded_rectangle(
            [x - width / 2, top, x + width / 2, bottom],
            radius=width / 2,
            fill=colour,
        )

    return image.resize((size, size), Image.LANCZOS)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("src/shell/sonora.ico"))
    arguments = parser.parse_args()

    # Every size Windows asks for. Leaving one out means Windows scales the
    # nearest, and the one it picks is never the one you would have picked.
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [draw(size) for size in sizes]

    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    # Saved from the largest, with every drawing offered: Pillow only ever
    # scales an icon down, so saving from the 16-pixel one produces a file with
    # a 16-pixel icon in it and nothing else. append_images is what makes each
    # size the drawing made for it rather than a resample of the big one.
    images[-1].save(arguments.out, format="ICO", sizes=[(s, s) for s in sizes],
                    append_images=images)

    from PIL import Image as _Image  # local: only to verify what was written

    written = sorted(_Image.open(arguments.out).info["sizes"])
    if len(written) != len(sizes):
        raise SystemExit(f"make_icon: asked for {len(sizes)} sizes, got {written}")
    print(f"make_icon: wrote {arguments.out} ({len(written)} sizes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
