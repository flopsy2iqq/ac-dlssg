"""Draws the ac-dlssg logo and writes every image made from it.

The mark: three forward-leaning frames. The solid racing-red one in front is
the real frame; the two thinner, darker copies behind it are the generated
frames, and together they read as speed. The geometry lives in mark_shapes()
only, in a 512 x 512 unit box, and every output is drawn from it:

  docs/logo/ac-dlssg-mark.svg       the mark as shapes, transparent
  docs/logo/ac-dlssg-mark-512.png   the mark, 512 x 512, transparent
  docs/logo/ac-dlssg-banner.png     1280 x 320, dark, for the README header
  docs/logo/social-preview.png      1280 x 640, dark, GitHub's social preview
  apps/lua/AcDlssg/icon.png         64 x 64, transparent, the CSP app icon
  apps/lua/AcDlssg/logo.png         96 x 96, transparent, the window header

Raster images are drawn at 4x their size and scaled down with premultiplied
alpha, so the slanted edges are smooth. The wordmark and the taglines are
Bahnschrift (a Windows font) rasterised into the PNGs; the SVG has no text.

Run from anywhere: python tools/brand/make_logo.py
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FONT = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'bahnschrift.ttf')
SS = 4  # supersampling factor

RED = (0xE1, 0x06, 0x00)
GHOST = (0x7A, 0x04, 0x00)
GHOST_FAR = (0x4A, 0x03, 0x00)
INK = (0x0B, 0x0B, 0x0D)
WHITE = (0xFF, 0xFF, 0xFF)
GREY = (0x9C, 0x9C, 0xA6)

WORDMARK = 'ac-dlssg'
BANNER_LINE = 'DLSS Frame Generation for Assetto Corsa'
SOCIAL_LINE = 'DLSS-G 2X / 3X / 4X for Assetto Corsa + CSP'


def mark_shapes():
    """The mark in a 512 x 512 box: (points, rgb) from back to front.

    Every frame is the same parallelogram leaning 20 degrees forward; the
    ghosts are narrower and darker and sit behind with a gap, the farther one
    thinner still, and the gaps close up towards the back like a trail.
    """
    top, bottom = 84.0, 428.0
    lean = (bottom - top) * math.tan(math.radians(20.0))
    widths = (30.0, 56.0, 178.0)  # far ghost, near ghost, real frame
    gaps = (20.0, 24.0)           # far-near, near-real
    total = sum(widths) + sum(gaps) + lean
    x = (512.0 - total) / 2.0
    colours = (GHOST_FAR, GHOST, RED)
    shapes = []
    for i, w in enumerate(widths):
        shapes.append(([(x, bottom), (x + w, bottom), (x + w + lean, top), (x + lean, top)], colours[i]))
        x += w + (gaps[i] if i < len(gaps) else 0.0)
    return shapes


def draw_mark(layer, left, top, size):
    """Draws the mark into an RGBA layer, its 512-unit box at (left, top) with
    the given side in pixels of that layer."""
    d = ImageDraw.Draw(layer)
    k = size / 512.0
    for points, rgb in mark_shapes():
        d.polygon([(left + px * k, top + py * k) for px, py in points], fill=rgb + (255,))


def downscale(img, width, height):
    """Scales a 4x RGBA image down with premultiplied alpha (no dark fringes)."""
    return img.convert('RGBa').resize((width, height), Image.LANCZOS).convert('RGBA')


def font(size, weight=b'Bold'):
    f = ImageFont.truetype(FONT, size)
    f.set_variation_by_name(weight)
    return f


def text_box(f, text):
    """Ink bounds of the text relative to its drawing origin."""
    return f.getbbox(text)


def save(img, *parts):
    path = os.path.join(ROOT, *parts)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, optimize=True)
    print('wrote', os.path.relpath(path, ROOT))


def transparent_mark(size, padding=0.0):
    """The mark alone on transparency; padding is the share of the side left
    empty around the 512-unit box."""
    big = size * SS
    layer = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    inner = big * (1.0 - 2.0 * padding)
    draw_mark(layer, big * padding, big * padding, inner)
    return downscale(layer, size, size)


def lockup(width, height, mark_size, word_size, line, line_size, stacked_line_gap, mark_gap):
    """Dark background, the mark and the wordmark side by side, the line under
    the wordmark; the whole group centred."""
    W, H = width * SS, height * SS
    img = Image.new('RGBA', (W, H), INK + (255,))
    fw = font(word_size * SS, b'Bold')
    fl = font(line_size * SS, b'Regular')
    wb = text_box(fw, WORDMARK)
    lb = text_box(fl, line)
    word_w, word_h = wb[2] - wb[0], wb[3] - wb[1]
    line_w, line_h = lb[2] - lb[0], lb[3] - lb[1]
    # The mark's ink spans y 84..428 of its box: size the box so that the ink
    # is as tall as the wordmark and the line together.
    text_h = word_h + stacked_line_gap * SS + line_h
    ink = 344.0 / 512.0
    box = mark_size * SS if mark_size else text_h / ink
    shapes = mark_shapes()
    ink_left = min(p[0] for s in shapes for p in s[0]) / 512.0 * box
    ink_right = max(p[0] for s in shapes for p in s[0]) / 512.0 * box
    mark_w = ink_right - ink_left
    text_w = max(word_w, line_w)
    group_w = mark_w + mark_gap * SS + text_w
    gx = (W - group_w) / 2.0
    group_h = max(text_h, box * ink)
    gy = (H - group_h) / 2.0
    # Mark: ink top at gy.
    draw_mark(img, gx - ink_left, gy - 84.0 / 512.0 * box, box)
    d = ImageDraw.Draw(img)
    tx = gx + mark_w + mark_gap * SS
    ty = gy + (group_h - text_h) / 2.0
    d.text((tx - wb[0], ty - wb[1]), WORDMARK, font=fw, fill=WHITE + (255,))
    d.text((tx - lb[0] + 2 * SS, ty + word_h + stacked_line_gap * SS - lb[1]), line, font=fl, fill=GREY + (255,))
    # Opaque: stored without an alpha channel.
    return downscale(img, width, height).convert('RGB')


def svg():
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">',
             '  <title>ac-dlssg</title>']
    for points, rgb in mark_shapes():
        pts = ' '.join('%.2f,%.2f' % p for p in points)
        parts.append('  <polygon points="%s" fill="#%02X%02X%02X"/>' % ((pts,) + rgb))
    parts.append('</svg>')
    return '\n'.join(parts) + '\n'


def main():
    path = os.path.join(ROOT, 'docs', 'logo', 'ac-dlssg-mark.svg')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(svg())
    print('wrote', os.path.relpath(path, ROOT))
    save(transparent_mark(512), 'docs', 'logo', 'ac-dlssg-mark-512.png')
    save(lockup(1280, 320, 0, 112, BANNER_LINE, 34, 18, 44), 'docs', 'logo', 'ac-dlssg-banner.png')
    save(lockup(1280, 640, 0, 150, SOCIAL_LINE, 40, 24, 60), 'docs', 'logo', 'social-preview.png')
    # The app icon and the header logo keep a small margin, since CSP draws
    # the icon edge to edge in its taskbar.
    save(transparent_mark(64, 0.02), 'apps', 'lua', 'AcDlssg', 'icon.png')
    save(transparent_mark(96, 0.0), 'apps', 'lua', 'AcDlssg', 'logo.png')


if __name__ == '__main__':
    main()
