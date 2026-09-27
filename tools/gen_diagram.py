#!/usr/bin/env python3
"""Generates the architecture diagram in docs/images/, in a light and a dark variant.

Two files and one source, for the same reason everything else in this repository is
generated: two pictures that have to say the same thing are one picture that is wrong. The
README references them with <picture> and a prefers-color-scheme source, which is how GitHub
serves a diagram that is legible on both themes -- CSS inside an SVG is stripped by GitHub's
sanitiser, so the theme cannot be handled inside the file.

    python tools/gen_diagram.py

What the diagram is for: the boundaries. Which process is which, what crosses between them,
and which of the three stores is safe to delete. The modules inside the browser process are
named and not drawn, because a box per module is a diagram nobody reads -- README.md has the
layout, and the ADRs have the reasons.
"""

import pathlib
import sys

W, H = 1000, 580

# GitHub's own palette, so the picture looks native in a README on either theme rather than
# like something pasted in from a drawing tool.
THEMES = {
    "light": {
        "bg": "#ffffff",
        "panel": "#f6f8fa",
        "inner": "#eaeef2",
        "border": "#d0d7de",
        "text": "#1f2328",
        "muted": "#59636e",
        "wire": "#8c959f",
        "accent": "#0969da",
        "warn": "#bc4c00",
    },
    "dark": {
        "bg": "#0d1117",
        "panel": "#161b22",
        "inner": "#21262d",
        "border": "#30363d",
        "text": "#e6edf3",
        "muted": "#9198a1",
        "wire": "#6e7681",
        "accent": "#4493f8",
        "warn": "#db6d28",
    },
}

FONT = "ui-sans-serif, -apple-system, Segoe UI, Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, Consolas, monospace"


def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Drawing:
    def __init__(self, theme):
        self.c = THEMES[theme]
        self.parts = []

    def box(self, x, y, w, h, *, fill="panel", stroke="border", dash=None, radius=8, width=1):
        dashes = f' stroke-dasharray="{dash}"' if dash else ""
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" '
            f'fill="{self.c[fill]}" stroke="{self.c[stroke]}" stroke-width="{width}"{dashes}/>'
        )

    def text(self, x, y, s, *, size=13, fill="text", weight="normal", anchor="start",
             mono=False, spacing=0.0):
        family = MONO if mono else FONT
        letter = f' letter-spacing="{spacing}"' if spacing else ""
        self.parts.append(
            f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" '
            f'font-weight="{weight}" fill="{self.c[fill]}" text-anchor="{anchor}"{letter}>'
            f"{esc(s)}</text>"
        )

    def lines(self, x, y, rows, *, size=12, fill="muted", step=16, **kw):
        for i, row in enumerate(rows):
            self.text(x, y + i * step, row, size=size, fill=fill, **kw)

    def rows(self, x, y, pairs, *, gutter=74, size=12, step=17):
        """A module name and what it is, in two columns.

        Two columns and not one sentence, because in one line "library SQLite + FTS5 index"
        reads as a phrase rather than as a name and a description, and a diagram whose labels
        have to be parsed is a diagram that has failed at the only thing it does.
        """
        for i, (name, what) in enumerate(pairs):
            self.text(x, y + i * step, name, size=size, weight="600", mono=True)
            self.text(x + gutter, y + i * step, what, size=size, fill="muted")

    def arrow(self, x1, y1, x2, y2, *, both=False, stroke="wire", dash=None):
        dashes = f' stroke-dasharray="{dash}"' if dash else ""
        start = ' marker-start="url(#back)"' if both else ""
        self.parts.append(
            f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{self.c[stroke]}" '
            f'stroke-width="1.5"{dashes} marker-end="url(#head)"{start}/>'
        )

    def label(self, x, y, s, *, size=11, anchor="middle"):
        # A wire label sits on the wire, so it gets the background colour behind it rather
        # than a box: fewer lines on the picture, and it cannot be mistaken for a node.
        width = len(s) * size * 0.56 + 10
        self.parts.append(
            f'<rect x="{x - width / 2:.0f}" y="{y - size:.0f}" width="{width:.0f}" '
            f'height="{size + 6}" fill="{self.c["bg"]}" stroke="none"/>'
        )
        self.text(x, y, s, size=size, fill="muted", anchor=anchor)

    def render(self):
        markers = (
            f'<marker id="head" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
            f'markerHeight="7" orient="auto-start-reverse">'
            f'<path d="M 0 0 L 10 5 L 0 10 z" fill="{self.c["wire"]}"/></marker>'
            f'<marker id="back" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="7" '
            f'markerHeight="7" orient="auto-start-reverse">'
            f'<path d="M 10 0 L 0 5 L 10 10 z" fill="{self.c["wire"]}"/></marker>'
        )
        body = "\n  ".join(self.parts)
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" '
            f'height="{H}" role="img" aria-label="Sonora process architecture">\n'
            f"  <defs>{markers}</defs>\n"
            f'  <rect width="{W}" height="{H}" fill="{self.c["bg"]}"/>\n'
            f"  {body}\n</svg>\n"
        )


def draw(theme):
    d = Drawing(theme)

    # ---- the browser process ------------------------------------------------
    d.box(24, 52, 476, 404)
    d.text(44, 80, "Sonora.exe", size=16, weight="600", mono=True)
    d.lines(44, 100, [
        "the browser process, and the one that owns the message loop:",
        "CEF runs on an external pump, so there is one loop and not two",
    ])

    d.box(44, 128, 436, 96, fill="inner")
    d.text(60, 148, "portable — built and tested on Windows, macOS and Linux",
           size=11, fill="muted")
    d.rows(60, 170, [
        ("core", "playback state machine, deep links, window placement"),
        ("audio", "decoder \u2192 lock-free SPSC ring \u2192 device callback"),
        ("bridge", "the native \u2194 web protocol, from one schema"),
    ])

    d.box(44, 236, 436, 78, fill="inner")
    d.rows(60, 258, [
        ("library", "SQLite + FTS5 index, and the scanner that fills it"),
        ("state", "the durable store: play history and the stable ids"),
        ("assets", "the web interface, served over sonora://"),
    ])

    d.box(44, 326, 436, 52, fill="inner")
    d.rows(60, 348, [("platform", "the only place #ifdef on the OS is allowed")])
    d.text(60, 366, "window, event loop, displays, tray, media panel, single instance",
           size=11, fill="muted")

    d.box(44, 390, 436, 54, fill="inner", stroke="warn")
    d.rows(60, 412, [("libcef.dll", "Chromium, and the Crashpad already in it")])
    d.text(60, 430, "someone else's code, and its build flags are not this project's",
           size=11, fill="muted")

    # ---- the other processes ------------------------------------------------
    d.box(560, 52, 416, 104)
    d.text(580, 80, "sonora_helper.exe", size=15, weight="600", mono=True)
    d.text(742, 80, "x N", size=12, fill="muted", mono=True)
    d.lines(580, 102, [
        "renderer, GPU and utility processes. The web interface",
        "runs here, and never sees a file path — only ids.",
    ])

    d.box(560, 180, 416, 92, stroke="warn")
    d.text(580, 208, "Sonora.exe --type=crashpad-handler", size=13, weight="600", mono=True)
    d.lines(580, 230, [
        "Started by Crashpad, from this same executable — which is",
        "why main() asks CEF whether it is a sub-process, first.",
    ])

    d.box(560, 296, 416, 122)
    d.text(580, 324, "sonora-updater.exe", size=15, weight="600", mono=True)
    d.lines(580, 346, [
        "The only process allowed to move the installation, and it",
        "copies itself to %TEMP% before it does. Waits for the",
        "application to exit, writes a journal before every step,",
        "and rolls back a version that never reported starting.",
    ])

    # ---- wires --------------------------------------------------------------
    d.arrow(500, 104, 556, 104, both=True, stroke="accent")
    d.label(528, 98, "bridge")

    d.arrow(482, 404, 556, 240, stroke="warn")
    d.label(522, 302, "starts")

    d.arrow(500, 356, 556, 348, stroke="wire", dash="4 3")
    d.label(528, 342, "at exit")

    # ---- what is on disk ----------------------------------------------------
    d.box(24, 486, 276, 72, fill="inner")
    d.text(40, 510, "the user's music folder", size=12, weight="600")
    d.lines(40, 528, [
        "the source of truth, and the reason",
        "nothing below it is precious",
    ], size=11)

    d.arrow(300, 522, 344, 522)
    d.label(322, 514, "scan")

    d.box(348, 486, 276, 72, fill="inner")
    d.text(364, 510, "library.sqlite", size=12, weight="600", mono=True)
    d.lines(364, 528, [
        "the index: a cache of that folder,",
        "and safe to delete (ADR 0007)",
    ], size=11)

    d.box(672, 486, 304, 72, fill="inner")
    d.text(688, 510, "state.sqlite", size=12, weight="600", mono=True)
    d.lines(688, 528, [
        "play history and the ids links are made",
        "of: NOT safe to delete (ADR 0008)",
    ], size=11)

    d.text(W - 24, 32, "Sonora  ·  processes, boundaries and what is on disk", size=12,
           fill="muted", anchor="end")
    return d.render()


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "docs" / "images"
    out.mkdir(parents=True, exist_ok=True)
    for theme in THEMES:
        path = out / f"architecture-{theme}.svg"
        path.write_text(draw(theme), encoding="utf-8")
        print(f"gen_diagram: {path.relative_to(path.parents[2])} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
