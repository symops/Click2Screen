#!/usr/bin/env python3
"""Draws the Click2Screen icon (a screen with a record dot and a pointer) into src/Click2Screen.ico."""
from PIL import Image, ImageDraw
import sys, os

def draw(size):
    k = 4  # supersampling
    S = size * k
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = S * 0.06
    d.rounded_rectangle([m, m, S - m, S - m], radius=S * 0.2, fill=(34, 40, 56, 255))
    # screen
    sx0, sy0, sx1, sy1 = S * 0.17, S * 0.22, S * 0.83, S * 0.66
    d.rounded_rectangle([sx0, sy0, sx1, sy1], radius=S * 0.05, fill=(70, 130, 220, 255), outline=(235, 240, 250, 255), width=max(1, int(S * 0.03)))
    # stand
    d.rectangle([S * 0.44, S * 0.66, S * 0.56, S * 0.74], fill=(235, 240, 250, 255))
    d.rounded_rectangle([S * 0.32, S * 0.74, S * 0.68, S * 0.79], radius=S * 0.02, fill=(235, 240, 250, 255))
    # record dot
    cx, cy, r = S * 0.36, S * 0.40, S * 0.085
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(235, 50, 50, 255))
    # pointer
    px, py = S * 0.52, S * 0.42
    pts = [(px, py), (px, py + S * 0.24), (px + S * 0.066, py + S * 0.18), (px + S * 0.115, py + S * 0.285),
           (px + S * 0.16, py + S * 0.265), (px + S * 0.112, py + S * 0.165), (px + S * 0.19, py + S * 0.165)]
    d.polygon(pts, fill=(255, 255, 255, 255), outline=(20, 24, 34, 255))
    return im.resize((size, size), Image.LANCZOS)

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "src", "Click2Screen.ico")
sizes = [16, 24, 32, 48, 64, 128, 256]
imgs = [draw(s) for s in sizes]
imgs[-1].save(out, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
print("wrote", out)
