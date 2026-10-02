"""Rider-view preview of the pace hints (src/coachpace.h), for the PR and for Sean to look at.

    coachpace_test --dump <dir>
    python tools/pace_preview.py <dir> <out_dir>

Reads the scenes the test wrote (<dir>/pace-too-fast.txt, pace-too-slow.txt: the ribbon rows in
both colour layers, the chevrons and gate as triangles, the ground) and draws each from the
rider's seat twice, side by side: the line as it is today, and with the pace hints over it.
The synthetic lap is straight in the file; here the corner after the braking zone is bent to the
right so it reads as one. Offline only; nothing here ships.
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 960, 540
FOCAL = 1150.0  # a narrow lens, so the 10-50 m that matter are big enough to see
EYE = 1.45  # metres over the ground
PITCH = math.radians(4.0)


def bend(s):
    """Heading (radians) of the synthetic line at s: straight, then a right-hander after 215 m."""
    if s < 215:
        return 0.0
    if s < 265:
        return -math.pi / 2 * (s - 215) / 50
    return -math.pi / 2


class Path:
    def __init__(self, s0, s1):
        self.s0, self.step = s0, 0.25
        self.pts = []
        x = z = 0.0
        s = s0
        while s <= s1 + 1:
            self.pts.append((x, z, bend(s)))
            h = bend(s)
            x += math.cos(h) * self.step
            z += math.sin(h) * self.step
            s += self.step

    def at(self, s, lat):
        i = max(0, min(len(self.pts) - 1, int((s - self.s0) / self.step)))
        x, z, h = self.pts[i]
        # left of the heading is +z when heading +x
        return x - math.sin(h) * lat, z + math.cos(h) * lat


def load(path):
    rows, marks, ground, meta = [], [], {}, ""
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "#":
            meta = line[2:].strip()
        elif p[0] == "row":
            rows.append([float(v) for v in p[1:]])
        elif p[0] == "mark":
            marks.append([float(v) for v in p[1:]])
        elif p[0] == "ground":
            ground[round(float(p[1]) * 2) / 2] = float(p[2])
        elif p[0] == "speed":
            speed = (float(p[1]), float(p[2]))
    return rows, marks, ground, meta, speed


def make_cam(path, at_s, ground):
    cx, cz = path.at(at_s - 3.0, 0.0)
    cy = ground.get(round((at_s - 3.0) * 2) / 2, 0.0) + EYE
    h = bend(at_s)
    fwd = (math.cos(h) * math.cos(PITCH), -math.sin(PITCH), math.sin(h) * math.cos(PITCH))
    up = (math.cos(h) * math.sin(PITCH), math.cos(PITCH), math.sin(h) * math.sin(PITCH))
    right = (math.sin(h), 0.0, -math.cos(h))  # screen right: the line's right

    def proj(x, y, z):
        d = (x - cx, y - cy, z - cz)
        zc = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]
        if zc < 0.3:
            return None
        xc = d[0] * right[0] + d[1] * right[1] + d[2] * right[2]
        yc = d[0] * up[0] + d[1] * up[1] + d[2] * up[2]
        return (W / 2 + FOCAL * xc / zc, H * 0.30 - FOCAL * yc / zc)

    return proj


def blend_poly(img, pts, rgba):
    if any(p is None for p in pts):
        return
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(over).polygon(pts, fill=tuple(int(max(0, min(1, c)) * 255) for c in rgba))
    img.alpha_composite(over)


def scene(rows, marks, ground, at_s, with_pace, hud_more):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    for y in range(H):  # sky
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(120 + 80 * t), int(165 + 60 * t), int(215 + 30 * t)))
    path = Path(at_s - 10, at_s + 130)
    proj = make_cam(path, at_s, ground)
    gh = lambda s: ground.get(round(s * 2) / 2, 0.0)
    # grass, far to near, then the dirt track (7 m wide), in strips
    s = at_s + 125
    while s > at_s - 4:
        s2 = s - 1.0
        for lat0, lat1, col in ((-40, 40, (86, 118, 60, 255)), (-3.5, 3.5, (128, 96, 66, 255))):
            a = path.at(s, lat0), path.at(s, lat1), path.at(s2, lat1), path.at(s2, lat0)
            ys = (gh(s), gh(s), gh(s2), gh(s2)) if col[0] == 128 else (gh(s) - 0.05,) * 2 + (gh(s2) - 0.05,) * 2
            pts = [proj(p[0], y, p[1]) for p, y in zip(a, ys)]
            if all(p is not None for p in pts):
                shade = 1.0 - 0.15 * ((int(s) // 3) % 2) if col[0] == 128 else 1.0
                d.polygon(pts, fill=tuple(int(c * shade) for c in col[:3]) + (255,))
        s = s2
    # the ribbon, far to near
    for k in range(len(rows) - 1, 0, -1):
        r0, r1 = rows[k - 1], rows[k]
        c = r1[3:7] if with_pace else r1[7:11]
        quad = []
        for r, lat in ((r0, 0.35), (r1, 0.35), (r1, -0.35), (r0, -0.35)):
            x, z = path.at(r[1], lat)
            quad.append(proj(x, r[2], z))
        blend_poly(img, quad, c)
    if with_pace:
        for t in range(0, len(marks) - 2, 3):
            tri = []
            for m in marks[t : t + 3]:
                x, z = path.at(m[0], m[2])
                tri.append(proj(x, m[1] + 0.01, z))
            blend_poly(img, tri, marks[t][3:7])
    d = ImageDraw.Draw(img)
    # a hint of the bike: bars and number plate at the bottom
    d.polygon([(W * 0.30, H), (W * 0.36, H * 0.86), (W * 0.64, H * 0.86), (W * 0.70, H)], fill=(25, 25, 28, 255))
    d.line([(W * 0.18, H * 0.84), (W * 0.82, H * 0.84)], fill=(40, 40, 44, 255), width=9)
    d.rectangle([W * 0.44, H * 0.875, W * 0.56, H * 0.97], fill=(235, 235, 235, 255))
    if hud_more:
        f = font(int(H * 0.032))
        tw = d.textlength("MORE SPEED", font=f)
        x0, y0 = W / 2 - tw / 2 - 10, H * 0.396
        over = Image.new("RGBA", img.size, (0, 0, 0, 0))
        ImageDraw.Draw(over).rectangle([x0, y0, W / 2 + tw / 2 + 10, y0 + H * 0.045], fill=(0, 0, 0, 160))
        img.alpha_composite(over)
        d = ImageDraw.Draw(img)
        d.text((W / 2 - tw / 2, y0 + H * 0.006), "MORE SPEED", font=f, fill=(51, 217, 255, 255))
    return img


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def render(src, out, at_s, title, sub):
    rows, marks, ground, meta, speed = load(src)
    more = "more=1" in meta
    left = scene(rows, marks, ground, at_s, False, False)
    right = scene(rows, marks, ground, at_s, True, more)
    img = Image.new("RGB", (W * 2 + 12, H + 92), (18, 18, 20))
    img.paste(left.convert("RGB"), (0, 92))
    img.paste(right.convert("RGB"), (W + 12, 92))
    d = ImageDraw.Draw(img)
    d.text((14, 10), title, font=font(26), fill=(240, 240, 240))
    d.text((14, 46), sub, font=font(17), fill=(185, 185, 190))
    d.text((14, 100), "line today (what to do)", font=font(18), fill=(255, 255, 255))
    d.text((W + 26, 100), "with pace hints over it", font=font(18), fill=(255, 255, 255))
    d.text((W * 2 - 330, 46), f"you {speed[0] * 3.6:.0f} km/h   Coach {speed[1] * 3.6:.0f} km/h",
           font=font(17), fill=(185, 185, 190))
    img.save(out)
    print(out, meta)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    render(f"{src}/pace-too-fast.txt", f"{dst}/pace-too-fast.png", 160.0,
           "TOO FAST before a corner",
           "14% over Coach 40 m out: chevrons point back, yellow/red brought ~11 m sooner (braking-distance model)")
    render(f"{src}/pace-too-slow.txt", f"{dst}/pace-too-slow.png", 415.0,
           "TOO SLOW before a jump",
           "29% under Coach 35 m from the lip: chevrons point on, gate at the lip, MORE SPEED on the HUD")
