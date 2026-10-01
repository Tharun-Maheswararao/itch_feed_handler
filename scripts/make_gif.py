#!/usr/bin/env python3
"""Render live-view frames into an animated GIF (no screen recorder needed).

    python3 scripts/make_gif.py results/logs/view_frames.txt docs/live_book.gif

`feed_handler bench ... --view SYMBOL --record FILE` writes one frame per
refresh, separated by form feeds. Each frame is drawn as a terminal screen.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BG = (26, 26, 25)
FG = (232, 231, 226)
DIM = (150, 149, 140)
BID = (57, 135, 229)   # blue
ASK = (230, 103, 103)  # red
FONTS = [
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
]


def load_font(size):
    for f in FONTS:
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def draw_frame(text, font, cw, ch, cols, rows, pad=18):
    img = Image.new("RGB", (cols * cw + 2 * pad, rows * ch + 2 * pad + 10), BG)
    d = ImageDraw.Draw(img)
    for r, line in enumerate(text.splitlines()):
        y = pad + r * ch
        if "|" in line and r >= 3 and not line.strip().startswith("orders"):
            left, right = line.split("|", 1)
            d.text((pad, y), left, font=font, fill=BID)
            d.text((pad + len(left) * cw, y), "|", font=font, fill=DIM)
            d.text((pad + (len(left) + 1) * cw, y), right, font=font, fill=ASK)
        else:
            d.text((pad, y), line, font=font, fill=DIM if r in (1, 2) or line.startswith(" spread") else FG)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--ms", type=int, default=500, help="milliseconds per frame")
    ap.add_argument("--max-frames", type=int, default=120)
    ap.add_argument("--font-size", type=int, default=18)
    args = ap.parse_args()

    frames = [f.strip("\n") for f in args.frames.read_text().split("\f") if f.strip()]
    if len(frames) > args.max_frames:  # keep an even sample across the run
        step = len(frames) / args.max_frames
        frames = [frames[int(i * step)] for i in range(args.max_frames)]
    font = load_font(args.font_size)
    box = font.getbbox("M")
    cw, ch = box[2] - box[0], int((box[3] - box[1]) * 1.55)
    cols = max(len(l) for f in frames for l in f.splitlines())
    rows = max(len(f.splitlines()) for f in frames)
    images = [draw_frame(f, font, cw, ch, cols, rows) for f in frames]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    images[0].save(args.out, save_all=True, append_images=images[1:], duration=args.ms, loop=0, optimize=True)
    print(f"wrote {args.out} ({len(images)} frames)")


if __name__ == "__main__":
    main()
