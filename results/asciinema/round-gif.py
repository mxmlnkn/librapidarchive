#!/usr/bin/env python3
"""Round the corners of an animated GIF and make the outside transparent.

The terminal demo usually uses a few dozen colors, so the script prefers one exact
global palette. Font anti-aliasing can produce thousands of colors; in that case
quantize each frame to 255 colors and reserve index 255 for transparency.
gifsicle restores the delta-frame encoding afterwards; Pillow alone would store
every frame in full.
"""

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageSequence

TRANSPARENT_INDEX = 255


def collectColors(image):
    colors = set()
    for frame in ImageSequence.Iterator(image):
        if frame.mode == "P":
            palette = frame.getpalette() or []
            frameColors = frame.getcolors(maxcolors=256)
            if frameColors is None:
                raise SystemExit("error: a GIF frame uses more than 256 colors")
            colors.update(
                tuple(palette[index * 3:index * 3 + 3])
                for _, index in frameColors
                if len(palette[index * 3:index * 3 + 3]) == 3
            )
        else:
            frameColors = frame.getcolors(maxcolors=1 << 20)
            if frameColors is None:
                raise SystemExit("error: the GIF has more than one million colors")
            colors.update(tuple(color[:3]) for _, color in frameColors)
    return sorted(colors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--radius", type=int, default=10,
        help="corner radius in pixels (default: 10)",
    )
    args = parser.parse_args()

    if args.radius < 0:
        raise SystemExit("error: --radius must not be negative")
    if not shutil.which("gifsicle"):
        raise SystemExit("error: gifsicle is required (apt-get install gifsicle)")

    image = Image.open(args.input)
    width, height = image.size
    colors = collectColors(image)
    useExactPalette = len(colors) <= TRANSPARENT_INDEX
    if useExactPalette:
        palette = [0] * (256 * 3)
        for index, color in enumerate(colors):
            palette[index * 3:index * 3 + 3] = color
        paletteImage = Image.new("P", (1, 1))
        paletteImage.putpalette(palette)
    else:
        print(f"Quantizing {len(colors)} colors to {TRANSPARENT_INDEX}")

    mask = Image.new("L", (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, width - 1, height - 1), radius=args.radius, fill=255
    )
    transparent = mask.point(lambda alpha: 255 if alpha == 0 else 0)

    frames = []
    durations = []
    for frame in ImageSequence.Iterator(image):
        durations.append(frame.info.get("duration", 100))
        if useExactPalette:
            quantized = frame.convert("RGB").quantize(
                palette=paletteImage, dither=Image.Dither.NONE
            )
        else:
            quantized = frame.convert("RGB").convert(
                "P", palette=Image.ADAPTIVE, colors=TRANSPARENT_INDEX
            )
        quantized.paste(TRANSPARENT_INDEX, mask=transparent)
        quantized.info["transparency"] = TRANSPARENT_INDEX
        frames.append(quantized)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="round-gif-") as temporaryDirectory:
        temporary = Path(temporaryDirectory) / "frames.gif"
        frames[0].save(
            temporary,
            save_all=True,
            append_images=frames[1:],
            duration=durations,
            loop=image.info.get("loop", 0),
            disposal=2,
            transparency=TRANSPARENT_INDEX,
            optimize=True,
        )
        subprocess.run(
            ["gifsicle", "-O3", "--careful", str(temporary), "-o", str(args.output)],
            check=True,
        )
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
