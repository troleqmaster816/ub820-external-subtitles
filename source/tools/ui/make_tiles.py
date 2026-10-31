#!/usr/bin/env python3
"""Makes modules/ui/tiles_png.h: the OPTIONS root tiles of our categories.

Each tile is the empty stock tile (modules/ui/tile_HD.png) with the caption
drawn in the player's own font, white for the normal state and in the stock
focus colour for the selected one, as the stock icons (smngbr_0N_N/F_HD.png)
are. The player's font is not part of this repository, so the header is
committed and this script is only run when a caption changes:

    python tools/ui/make_tiles.py <firmware>/datafs/local/fonts/f015003t.ttf
"""
import io
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

UI = Path(__file__).resolve().parents[2] / "modules" / "ui"
NORMAL = (255, 255, 255, 255)
FOCUS = (255, 194, 57, 255)   # icon colour of smngbr_0N_F_HD.png
CENTRE = (80, 60)             # middle of the stock icons
MAX_WIDTH = 124
TILES = (("subs", ("External", "Subtitles")), ("helper", ("UB820", "Helper")))


def fit(font_path, lines):
    for size in range(34, 12, -1):
        font = ImageFont.truetype(font_path, size)
        if max(font.getbbox(line)[2] - font.getbbox(line)[0] for line in lines) <= MAX_WIDTH:
            return font
    raise SystemExit("caption does not fit")


def tile(base, font, lines, colour):
    im = base.copy()
    draw = ImageDraw.Draw(im)
    ascent, descent = font.getmetrics()
    step = ascent + descent
    top = CENTRE[1] - step * len(lines) // 2
    for i, line in enumerate(lines):
        draw.text((CENTRE[0], top + i * step + ascent), line, font=font, fill=colour, anchor="ms")
    out = io.BytesIO()
    im.save(out, "PNG", optimize=True)
    return out.getvalue()


def array(name, data):
    rows = [",".join("0x%02x" % b for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return "static const unsigned char %s[]={\n%s\n};\n" % (name, ",\n".join(rows))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    base = Image.open(UI / "tile_HD.png").convert("RGBA")
    parts = ["/* Made by tools/ui/make_tiles.py from tile_HD.png; see there. */\n",
             "#ifndef UB_TILES_PNG_H\n#define UB_TILES_PNG_H\n"]
    font = fit(sys.argv[1], [line for _, lines in TILES for line in lines])
    for key, lines in TILES:
        parts.append(array("ub_tile_%s_png" % key, tile(base, font, lines, NORMAL)))
        parts.append(array("ub_tile_%s_focus_png" % key, tile(base, font, lines, FOCUS)))
    parts.append("#endif\n")
    (UI / "tiles_png.h").write_text("".join(parts), newline="\n")


if __name__ == "__main__":
    main()
