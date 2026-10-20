# Original LiveArea artwork for the Zero Mission Vita port (no game assets):
# a night sky with stars, a rocky alien planet with an orange rim light.
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def sky(w, h, seed):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = (int(10 + 14 * t), int(12 + 10 * t), int(30 + 26 * t))
        for x in range(w):
            px[x, y] = c
    rnd = random.Random(seed)
    d = ImageDraw.Draw(img)
    for _ in range(w * h // 900):
        x, y = rnd.randrange(w), rnd.randrange(h)
        b = rnd.randrange(110, 255)
        r = rnd.choice((0, 0, 0, 1))
        col = (b, b, min(255, b + 20))
        d.ellipse((x - r, y - r, x + r, y + r), fill=col)
    return img

def planet(size, seed):
    """RGBA planet, lit from the upper right, orange rim."""
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    px = img.load()
    rnd = random.Random(seed)
    craters = [(rnd.uniform(-.8, .8), rnd.uniform(-.8, .8), rnd.uniform(.05, .16)) for _ in range(14)]
    R = s / 2 - 2
    for y in range(s):
        for x in range(s):
            nx, ny = (x - s / 2) / R, (y - s / 2) / R
            d2 = nx * nx + ny * ny
            if d2 > 1:
                continue
            nz = math.sqrt(1 - d2)
            lx, ly, lz = .55, -.55, .63
            lam = max(0, nx * lx + ny * ly + nz * lz)
            base = [70, 46, 92]
            for cx, cy, cr in craters:
                dd = math.hypot(nx - cx, ny - cy)
                if dd < cr:
                    k = dd / cr
                    shade = -22 if k < .8 else 16
                    base = [c + shade for c in base]
            band = math.sin(ny * 9 + math.sin(nx * 3) * 1.5) * 8
            col = [c + band for c in base]
            col = [c * (.25 + .9 * lam) for c in col]
            rim = max(0, 1 - nz) ** 3
            col = [col[0] + 255 * rim * .9, col[1] + 140 * rim * .9, col[2] + 40 * rim * .6]
            a = 255 if d2 < .985 else int(255 * (1 - (d2 - .985) / .015))
            px[x, y] = tuple(max(0, min(255, int(c))) for c in col) + (a,)
    return img

def glow(base, box, color, radius):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).ellipse(box, fill=color)
    layer = layer.filter(ImageFilter.GaussianBlur(radius))
    base.alpha_composite(layer)

def text(img, xy, s, size, fill, anchor="mm"):
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, size)
    x, y = xy
    for dx, dy in ((2, 2), (1, 2), (2, 1)):
        d.text((x + dx, y + dy), s, font=f, fill=(0, 0, 0, 200), anchor=anchor)
    d.text(xy, s, font=f, fill=fill, anchor=anchor)

ORANGE = (255, 156, 52, 255)
PALE = (205, 220, 255, 255)

def scene(w, h, pr, pc, seed):
    img = sky(w, h, seed).convert("RGBA")
    glow(img, (pc[0] - pr - 30, pc[1] - pr - 30, pc[0] + pr + 30, pc[1] + pr + 30), (255, 120, 40, 90), pr // 3)
    p = planet(pr * 2, seed)
    img.alpha_composite(p, (pc[0] - pr, pc[1] - pr))
    return img

out = sys.argv[1]
bg = scene(840, 500, 150, (420, 190), 7)
text(bg, (420, 395), "Zero Mission", 64, ORANGE)
text(bg, (420, 450), "PS Vita Port", 28, PALE)
bg.convert("RGB").quantize(256).save(f"{out}/bg.png", optimize=True)

icon = scene(128, 128, 46, (64, 54), 3)
text(icon, (64, 112), "ZM", 26, ORANGE)
icon.convert("RGB").quantize(256).save(f"{out}/icon0.png", optimize=True)

st = scene(280, 158, 46, (58, 79), 5)
text(st, (196, 66), "Zero Mission", 19, ORANGE)
text(st, (196, 96), "Start", 17, PALE)
st.convert("RGB").quantize(256).save(f"{out}/startup.png", optimize=True)
