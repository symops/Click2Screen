#!/usr/bin/env python3
"""Draws the Click2Screen icons on the same gradient tile as Speak2Mic / Show2Cam (a screen with a record dot and a pointer):
    src/Click2Screen.ico         the program
    src/Click2Screen-Setup.ico   the installer: the same glyph with a download badge
Small sizes (16-24 px) use a simplified glyph so they stay readable.
    python3 tools/make_icon.py
"""
import os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'src')
W = 1024
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def gradient_tile(size, radius):
    """Rounded square with a diagonal violet -> cyan gradient and a soft top highlight (as Speak2Mic / Show2Cam)."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / (size - 1)
    t = np.clip(0.65 * y + 0.35 * x, 0, 1)[..., None]
    c0 = np.array([108, 59, 255], np.float32)
    c1 = np.array([0, 194, 255], np.float32)
    rgb = c0 * (1 - t) + c1 * t
    hl = np.clip(1 - y * 2.2, 0, 1)[..., None] * 38
    rgb = np.clip(rgb + hl, 0, 255).astype(np.uint8)
    img = Image.fromarray(np.dstack([rgb, np.full((size, size), 255, np.uint8)]), 'RGBA')
    mask = Image.new('L', (size, size), 0)
    m = int(size * 0.035)
    ImageDraw.Draw(mask).rounded_rectangle((m, m, size - m, size - m), radius=radius, fill=255)
    img.putalpha(mask)
    return img


def draw_screen(d, small):
    """A monitor: white frame, dark screen with a red record dot and a white pointer, a stand."""
    white = (255, 255, 255, 255)
    dark = (40, 30, 110, 255)
    red = (255, 70, 80, 255)
    if small:
        d.rounded_rectangle((150, 230, 874, 690), radius=90, fill=white)
        d.rectangle((200, 280, 824, 640), fill=dark)
        d.ellipse((260, 330, 420, 490), fill=red)
        d.polygon([(520, 380), (520, 600), (590, 540), (640, 640), (700, 615), (650, 520), (740, 520)], fill=white)
        d.rectangle((452, 690, 572, 800), fill=white)
        d.rounded_rectangle((300, 790, 724, 870), radius=40, fill=white)
        return
    d.rounded_rectangle((150, 200, 874, 690), radius=70, fill=white)
    d.rounded_rectangle((200, 250, 824, 640), radius=30, fill=dark)
    d.ellipse((262, 312, 400, 450), fill=red)                      # record dot
    d.ellipse((294, 344, 340, 390), fill=(255, 170, 175, 255))     # its highlight
    d.polygon([(520, 360), (520, 590), (581, 535), (627, 630), (672, 609), (626, 515), (714, 515)],
              fill=white, outline=(20, 14, 70, 255))                # pointer
    d.rectangle((467, 690, 557, 790), fill=white)                   # stand
    d.rounded_rectangle((320, 780, 704, 850), radius=35, fill=white)


def draw_badge(d):
    """Download badge in the bottom right corner (installer), as Show2Cam."""
    d.ellipse((610, 610, 960, 960), fill=(30, 200, 90, 255), outline=(255, 255, 255, 255), width=28)
    d.rectangle((755, 680, 815, 820), fill=(255, 255, 255, 255))
    d.polygon([(700, 800), (870, 800), (785, 890)], fill=(255, 255, 255, 255))


def render(size, badge):
    img = gradient_tile(W, 180)
    d = ImageDraw.Draw(img)
    draw_screen(d, size <= 24)
    if badge:
        draw_badge(d)
    return img.resize((size, size), Image.LANCZOS)


def save(name, badge):
    imgs = [render(s, badge) for s in SIZES]
    imgs[-1].save(os.path.join(OUT, name), format='ICO', sizes=[(s, s) for s in SIZES], append_images=imgs[:-1])


if __name__ == '__main__':
    save('Click2Screen.ico', False)
    save('Click2Screen-Setup.ico', True)
    print('Click2Screen.ico, Click2Screen-Setup.ico written')
