"""Rider-view mock of the gear hints (src/coachgear.h), for the PR and for Sean to look at.

    coachgear_test --dump <dir>
    python tools/gear_preview.py <dir> <out.png>

Reads the scenes the test wrote (<dir>/gear-up.txt, gear-down.txt: the ribbon rows, the glyph as
triangles, the HUD badge's quads and texts) and draws each from the rider's seat on a straight
stretch: 30 m before Coach's shift to fourth, once from second (up) and once from sixth (down).
Offline only; nothing here ships.
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 960, 540
FOCAL = 640.0  # the game's lens: 16:9, about 47 degrees from top to bottom
EYE = 1.45
PITCH = math.radians(4.0)
CAM_BACK = 3.0


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def load(path):
    rows, marks, hq, ht, meta = [], [], [], [], ""
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "#":
            meta = line[2:].strip()
        elif p[0] == "row":
            rows.append((float(p[1]), float(p[2])))
        elif p[0] == "mark":
            marks.append([float(v) for v in p[1:]])
        elif p[0] == "hq":
            hq.append(([float(v) for v in p[1:9]], int(p[9], 16)))
        elif p[0] == "ht":
            ht.append((float(p[1]), float(p[2]), float(p[3]), int(p[4]), int(p[5], 16), " ".join(p[6:])))
    return rows, marks, hq, ht, meta


def abgr(c):
    return ((c >> 0) & 255, (c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255)


def camera():
    cx, cy, cz = -CAM_BACK, EYE, 0.0
    fwd = (math.cos(PITCH), -math.sin(PITCH), 0.0)
    up = (math.sin(PITCH), math.cos(PITCH), 0.0)

    def proj(x, y, z):  # world: x along the line, y up, z to the left
        d = (x - cx, y - cy, z - cz)
        zc = d[0] * fwd[0] + d[1] * fwd[1]
        if zc < 0.3:
            return None
        xc = -d[2]
        yc = d[0] * up[0] + d[1] * up[1]
        return (W / 2 + FOCAL * xc / zc, H * 0.47 - FOCAL * yc / zc)

    return proj


def poly(img, pts, rgba):
    if any(p is None for p in pts):
        return
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(over).polygon(pts, fill=tuple(int(max(0, min(1, c)) * 255) for c in rgba))
    img.alpha_composite(over)


def scene(src, title):
    rows, marks, hq, ht, meta = load(src)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(120 + 80 * t), int(165 + 60 * t), int(215 + 30 * t)))
    proj = camera()
    s = 140.0
    while s > -4:
        s2 = s - 1.0
        for z0, z1, col in ((-40, 40, (86, 118, 60)), (-3.5, 3.5, (128, 96, 66))):
            shade = 1.0 - 0.15 * ((int(s) // 3) % 2) if col[0] == 128 else 1.0
            y = -0.05 if col[0] != 128 else 0.0
            pts = [proj(s, y, z0), proj(s, y, z1), proj(s2, y, z1), proj(s2, y, z0)]
            if all(p is not None for p in pts):
                d.polygon(pts, fill=tuple(int(c * shade) for c in col) + (255,))
        s = s2
    # the line: Coach's blue, on the gas
    for k in range(len(rows) - 1, 0, -1):
        (a, ya), (b, yb) = rows[k - 1], rows[k]
        fade = max(0.0, min(1.0, 1.0 - (b - 25.0) / 35.0)) if b > 25 else 1.0
        poly(img, [proj(a, ya + 0.02, 0.35), proj(b, yb + 0.02, 0.35), proj(b, yb + 0.02, -0.35), proj(a, ya + 0.02, -0.35)],
             (0.36, 0.84, 0.36, 0.85 * fade))
    for t in range(0, len(marks) - 2, 3):
        poly(img, [proj(m[0], m[1], m[2]) for m in marks[t : t + 3]], marks[t][3:7])
    d = ImageDraw.Draw(img)
    # the bike
    d.polygon([(W * 0.30, H), (W * 0.36, H * 0.86), (W * 0.64, H * 0.86), (W * 0.70, H)], fill=(25, 25, 28, 255))
    d.line([(W * 0.18, H * 0.84), (W * 0.82, H * 0.84)], fill=(40, 40, 44, 255), width=9)
    d.rectangle([W * 0.44, H * 0.875, W * 0.56, H * 0.97], fill=(235, 235, 235, 255))
    # the HUD, as the plugin builds it (screen fractions, 16:9)
    for q, color in hq:
        poly(img, [(q[i] * W, q[i + 1] * H) for i in range(0, 8, 2)], [c / 255 for c in abgr(color)])
    for x, y, size, just, color, text in ht:
        f = font(int(size * H * 0.95))
        tw = d.textlength(text, font=f)
        px = x * W - (tw / 2 if just == 1 else tw if just == 2 else 0)
        d.text((px, y * H), text, font=f, fill=abgr(color))
    d.text((14, 12), title, font=font(24), fill=(20, 20, 20))
    return img, meta


def main(src, out):
    left, m1 = scene(f"{src}/gear-up.txt", "In second, Coach goes to fourth in 30 m")
    right, m2 = scene(f"{src}/gear-down.txt", "In sixth, Coach goes to fourth in 30 m")
    img = Image.new("RGB", (W * 2 + 12, H + 92), (18, 18, 20))
    img.paste(left.convert("RGB"), (0, 92))
    img.paste(right.convert("RGB"), (W + 12, 92))
    d = ImageDraw.Draw(img)
    d.text((14, 10), "GEAR HINTS on the line (hud.ini gear=1)", font=font(28), fill=(240, 240, 240))
    d.text((14, 52), "A sign with an arrow and the target gear stands on the line 6 m before Coach's shift, and a badge sits beside the cue box. "
                     "Only when your gear is neither of Coach's two there.", font=font(16), fill=(185, 185, 190))
    img.save(out)
    print(out, m1, m2)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
