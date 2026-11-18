"""Generates assets/stepthumb.ico (A3D brand colours) and a 512 px PNG preview.

Concept: an orange isometric part (cube) inside viewfinder corners = "thumbnail",
on a graphite rounded tile. Small sizes drop the viewfinder for clarity.
"""
from PIL import Image, ImageDraw
import math, os, sys

ORANGE = (232, 109, 49)      # #E86D31 A3D logo orange
ORANGE_LT = (255, 154, 92)   # #FF9A5C light accent
ORANGE_DK = (176, 74, 26)    # shaded side
GRAPHITE = (38, 38, 38)      # #262626
GRAPHITE_LT = (58, 58, 58)
WHITE = (244, 244, 244)      # #F4F4F4

def draw(size, detailed):
    S = 8                       # supersampling
    W = size * S
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = W * (0.03 if size >= 32 else 0.0)
    rad = W * 0.22
    # tile with subtle top highlight
    d.rounded_rectangle([pad, pad, W - pad, W - pad], rad, fill=GRAPHITE)
    if size >= 32:
        hl = Image.new('RGBA', (W, W), (0, 0, 0, 0)); hd = ImageDraw.Draw(hl)
        hd.rounded_rectangle([pad, pad, W - pad, W - pad], rad, fill=GRAPHITE_LT)
        mask = Image.linear_gradient('L').resize((W, W)).point(lambda v: max(0, 140 - v))
        im.paste(hl, (0, 0), Image.composite(mask, Image.new('L', (W, W), 0), hl.getchannel('A')))
        d = ImageDraw.Draw(im)

    cx, cy = W / 2, W / 2 + W * 0.02
    r = W * (0.27 if detailed else 0.36)     # cube "radius"
    c30, s30 = math.cos(math.pi / 6), 0.5
    top = (cx, cy - r)
    ul, ur = (cx - r * c30, cy - r * s30), (cx + r * c30, cy - r * s30)
    ctr = (cx, cy)
    ll, lr = (cx - r * c30, cy + r * s30), (cx + r * c30, cy + r * s30)
    bot = (cx, cy + r)
    d.polygon([top, ur, ctr, ul], fill=ORANGE_LT)       # top face
    d.polygon([ul, ctr, bot, ll], fill=ORANGE)          # left face
    d.polygon([ctr, ur, lr, bot], fill=ORANGE_DK)       # right face
    if detailed:
        # a bored hole on the top face (reads as a machined part)
        hr = r * 0.30
        pts = [(cx + hr * c30 * math.cos(t) - hr * c30 * math.sin(t) * 0 + 0,
                0) for t in []]
        ell = []
        for k in range(64):
            t = 2 * math.pi * k / 64
            u, v = math.cos(t) * hr, math.sin(t) * hr          # circle in face plane
            ell.append((cx + (u - v) * c30, cy - r * 0.5 + (u + v) * s30 - r * 0.0))
        d.polygon(ell, fill=(150, 60, 20))
        # edge lines like the renderer
        lw = max(1, int(W * 0.012))
        for a, b in [(top, ur), (ur, lr), (lr, bot), (bot, ll), (ll, ul), (ul, top), (ul, ctr), (ur, ctr), (ctr, bot)]:
            d.line([a, b], fill=(60, 26, 8), width=lw)
        # viewfinder corners
        m, L, t = W * 0.17, W * 0.15, max(2, int(W * 0.035))
        for sx in (0, 1):
            for sy in (0, 1):
                x = m if sx == 0 else W - m
                y = m if sy == 0 else W - m
                dx = L if sx == 0 else -L
                dy = L if sy == 0 else -L
                d.line([(x, y), (x + dx, y)], fill=WHITE, width=t)
                d.line([(x, y), (x, y + dy)], fill=WHITE, width=t)
                d.ellipse([x - t / 2, y - t / 2, x + t / 2, y + t / 2], fill=WHITE)
    return im.resize((size, size), Image.LANCZOS)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'assets'
    os.makedirs(out, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    imgs = [draw(s, s >= 40) for s in sizes]
    imgs[-1].save(os.path.join(out, 'stepthumb.ico'), sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
    draw(512, True).save(os.path.join(out, 'stepthumb-512.png'))
    # preview sheet on light and dark
    sheet = Image.new('RGBA', (900, 300), (255, 255, 255, 255))
    dark = Image.new('RGBA', (900, 150), (32, 32, 32, 255)); sheet.paste(dark, (0, 150))
    x = 10
    for s in [16, 24, 32, 48, 64, 128]:
        im = draw(s, s >= 40)
        sheet.alpha_composite(im, (x, 75 - s // 2)); sheet.alpha_composite(im, (x, 225 - s // 2)); x += s + 30
    sheet.save(os.path.join(out, 'icon-sizes-preview.png'))
