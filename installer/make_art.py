"""Draws the setup program's neon icon.
Usage: python3 make_art.py <outdir>"""
import sys, os, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build")
os.makedirs(OUT, exist_ok=True)
FONT = os.path.join(HERE, "art", "Audiowide-Regular.ttf")

BG_TOP = (10, 4, 24)
BG_BOT = (40, 6, 52)
PINK = (255, 46, 160)
CYAN = (0, 234, 255)
SUN_A = (255, 220, 60)
SUN_B = (255, 40, 140)


def gradient(w, h, a, b):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)))
    return im


def glow(base, layer, radius, strength=2):
    """Adds a blurred copy of an RGBA layer (the glow) and then the layer itself."""
    g = layer.filter(ImageFilter.GaussianBlur(radius))
    for _ in range(strength):
        base.alpha_composite(g)
    base.alpha_composite(layer)


def sun(base, cx, cy, r, horizon):
    w, h = base.size
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for y in range(cy - r, min(cy + r, horizon)):
        t = (y - (cy - r)) / (2 * r)
        col = tuple(int(SUN_A[i] + (SUN_B[i] - SUN_A[i]) * t) for i in range(3))
        # stripes cut through the lower half of the sun
        if t > 0.5:
            band = (y - cy) / r
            period = max(3, int(r * 0.16))
            gap = int(period * (band * 0.9))
            if (y - cy) % period < gap:
                continue
        dx = int(math.sqrt(max(0, r * r - (y - cy) ** 2)))
        d.line([(cx - dx, y), (cx + dx, y)], fill=col + (255,))
    glow(base, layer, r * 0.25, 2)


def grid(base, horizon, scale):
    w, h = base.size
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    lw = max(1, int(1.2 * scale))
    # floor
    floor = Image.new("RGBA", (w, h - horizon), (8, 0, 20, 235))
    base.alpha_composite(floor, (0, horizon))
    vx = w // 2
    for i in range(-14, 15):
        x0 = vx + i * w * 0.18
        d.line([(vx + i * w * 0.02, horizon), (x0, h)], fill=PINK + (230,), width=lw)
    y = 0.0
    step = 2.0 * scale
    while horizon + y < h:
        d.line([(0, horizon + y), (w, horizon + y)], fill=PINK + (230,), width=lw)
        step *= 1.35
        y += step
    d.line([(0, horizon), (w, horizon)], fill=CYAN + (255,), width=lw + 1)
    glow(base, layer, 3 * scale, 2)


def text(base, xy, s, size, col, font=FONT, anchor="mm", blur=None):
    w, h = base.size
    f = ImageFont.truetype(font, size)
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text(xy, s, font=f, fill=col + (255,), anchor=anchor)
    glow(base, layer, blur or size * 0.22, 2)
    # white-hot core
    core = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(core).text(xy, s, font=f, fill=(255, 255, 255, 140), anchor=anchor)
    base.alpha_composite(core)


def icon():
    S = 4
    n = 256 * S
    base = gradient(n, n, BG_TOP, BG_BOT).convert("RGBA")
    horizon = int(n * 0.70)
    sun(base, n // 2, int(n * 0.50), int(n * 0.34), horizon)
    grid(base, horizon, S * 2)
    text(base, (n // 2, int(n * 0.84)), "VR", 70 * S, CYAN)
    im = base.resize((256, 256), Image.LANCZOS)
    mask = Image.new("L", (256, 256), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, 255, 255], 44, fill=255)
    im.putalpha(mask)
    return im


ic = icon()
ic.save(os.path.join(OUT, "icon.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
ic.save(os.path.join(OUT, "icon.png"))
print("icon written to", OUT)
