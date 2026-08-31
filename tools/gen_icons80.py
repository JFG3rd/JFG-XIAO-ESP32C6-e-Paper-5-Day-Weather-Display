#!/usr/bin/env python3
"""Generate the 80x80 weather icon set for the BWRY e-paper panel.

Style: white body, black outline, and a lit edge - yellow where the sun would
catch it, red on severe cloud. Rain is black; red is reserved for heavy
precipitation, storms and hail so it keeps meaning "take notice" the same way
the warning strip and low-battery marker do.

Emits both a preview PNG and the packed 4bpp header the firmware consumes.
Palette indices match the panel: white 0x00, red 0x06, yellow 0x0B, black 0x0F.
"""
import math, struct, zlib

WHITE, RED, YELLOW, BLACK = 0x00, 0x06, 0x0B, 0x0F
RGB = {WHITE: (255, 255, 255), RED: (192, 32, 32), YELLOW: (232, 192, 32), BLACK: (0, 0, 0)}

# Every icon is authored in an 80-unit design space. The primitives convert to
# pixels through K, so the same drawing code renders the 80x80 hero art and the
# 40x40 day-column art natively - rather than downsampling the large set, which
# on a palette with no intermediate tones either eats the outlines or doubles
# their weight, depending on which way the block vote falls.
UNITS = 80.0
S = 80
K = 1.0


def set_size(size):
    global S, K
    S, K = size, size / UNITS


def canvas():
    return [[WHITE] * S for _ in range(S)]


# Anything drawn outside the tile is silently truncated, which is how the cloud
# crowns ended up flat-topped: the lit-rim copy is offset up-left and the outline
# grows outward, so a cloud that looks safely inside the tile in the source
# coordinates can still lose its top row. Record every out-of-bounds write so a
# clipped icon is a reported failure rather than something to spot by eye.
_clip = [0, 0, 0, 0]   # left, top, right, bottom overshoot, in pixels


def _note(x0, y0, x1, y1):
    _clip[0] = max(_clip[0], -int(math.floor(x0)))
    _clip[1] = max(_clip[1], -int(math.floor(y0)))
    _clip[2] = max(_clip[2], int(math.ceil(x1)) - (S - 1))
    _clip[3] = max(_clip[3], int(math.ceil(y1)) - (S - 1))


def disc(c, cx, cy, r, col):
    cx, cy, r = cx * K, cy * K, max(r * K, 0.5)
    _note(cx - r, cy - r, cx + r, cy + r)
    for y in range(max(0, int(cy - r)), min(S, int(cy + r) + 1)):
        for x in range(max(0, int(cx - r)), min(S, int(cx + r) + 1)):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                c[y][x] = col


def rect(c, x0, y0, x1, y1, col):
    x0, y0, x1, y1 = x0 * K, y0 * K, x1 * K, y1 * K
    _note(x0, y0, x1, y1)
    for y in range(max(0, int(y0)), min(S, int(y1) + 1)):
        for x in range(max(0, int(x0)), min(S, int(x1) + 1)):
            c[y][x] = col


def poly(c, pts, col):
    pts = [(x * K, y * K) for x, y in pts]
    ys = [p[1] for p in pts]
    xs_all = [p[0] for p in pts]
    _note(min(xs_all), min(ys), max(xs_all), max(ys))
    for y in range(max(0, int(min(ys))), min(S, int(max(ys)) + 1)):
        xs = []
        for i in range(len(pts)):
            x1, y1 = pts[i]
            x2, y2 = pts[(i + 1) % len(pts)]
            if (y1 <= y < y2) or (y2 <= y < y1):
                xs.append(x1 + (y - y1) * (x2 - x1) / (y2 - y1))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(max(0, int(xs[i])), min(S, int(xs[i + 1]) + 1)):
                c[y][x] = col


def thick_line(c, x0, y0, x1, y1, w, col):
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) * K) * 2 + 1
    for i in range(steps + 1):
        t = i / steps
        disc(c, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, w / 2, col)


# ------------------------------------------------------------------ parts ---
LOBES = [(-17, 3, 14), (-1, -7, 18), (16, 3, 13)]


def cloud_mass(c, cx, cy, col, grow=0, s=1.0):
    for dx, dy, r in LOBES:
        disc(c, cx + dx * s, cy + dy * s, r * s + grow, col)
    rect(c, cx - 31 * s - grow, cy + 3 * s, cx + 29 * s + grow, cy + 16 * s + grow, col)


def cloud(c, cx, cy, lit=YELLOW, s=1.0):
    """White body, black outline, and a rim lit from the upper left.

    The rim is a second copy of the mass offset up-left: the white body drawn
    on top of it leaves a crescent, which is the only way to suggest a light
    source with no intermediate tones to shade with.
    """
    cloud_mass(c, cx - 3, cy - 3, BLACK, grow=3, s=s)
    cloud_mass(c, cx, cy, BLACK, grow=3, s=s)
    if lit is not None:
        cloud_mass(c, cx - 3, cy - 3, lit, s=s)
    cloud_mass(c, cx, cy, WHITE, s=s)


def sun(c, cx, cy, r, ray_len=0):
    """Same lighting logic as the cloud: a white crescent, not a white hole."""
    if ray_len:
        for i in range(8):
            a = i * math.pi / 4
            x0, y0 = cx + math.cos(a) * (r + 4), cy + math.sin(a) * (r + 4)
            x1, y1 = cx + math.cos(a) * (r + 4 + ray_len), cy + math.sin(a) * (r + 4 + ray_len)
            thick_line(c, x0, y0, x1, y1, 6, BLACK)
            thick_line(c, x0, y0, x1, y1, 4, YELLOW)
    disc(c, cx, cy, r + 3, BLACK)
    disc(c, cx - r * 0.16, cy - r * 0.16, r, WHITE)
    disc(c, cx, cy, r, YELLOW)


def drop(c, x, y, col, size=1.0):
    h, w = 11 * size, 5 * size
    poly(c, [(x, y), (x + w, y + h), (x - w, y + h)], BLACK)
    disc(c, x, y + h, w, BLACK)
    poly(c, [(x, y + 2.5 * size), (x + w - 2, y + h), (x - w + 2, y + h)], col)
    disc(c, x, y + h, w - 2, col)


def flake(c, x, y, col, r=7):
    for i in range(3):
        a = i * math.pi / 3
        thick_line(c, x - math.cos(a) * r, y - math.sin(a) * r,
                   x + math.cos(a) * r, y + math.sin(a) * r, 3, col)


def bolt(c, x, y):
    pts = [(x - 3, y - 18), (x + 13, y - 18), (x + 3, y - 3),
           (x + 12, y - 3), (x - 7, y + 20), (x + 1, y + 1), (x - 10, y + 1)]
    for dx in (-3, -2, 2, 3):
        poly(c, [(px + dx, py) for px, py in pts], BLACK)
    for dy in (-3, 3):
        poly(c, [(px, py + dy) for px, py in pts], BLACK)
    poly(c, pts, YELLOW)


def arc(c, cx, cy, r, a0, a1, w, col):
    steps = max(8, int(abs(a1 - a0) * r))
    for i in range(steps + 1):
        a = a0 + (a1 - a0) * i / steps
        disc(c, cx + math.cos(a) * r, cy + math.sin(a) * r, w / 2, col)


def gust(c, x0, y, length, r, w, col):
    """A stroke that ends in a curl - the standard wind glyph, and the only
    shape that still reads as moving air once the drops are gone."""
    thick_line(c, x0, y, x0 + length, y, w, col)
    arc(c, x0 + length, y - r, r, math.pi / 2, -math.pi * 0.85, w, col)


# ------------------------------------------------------------------ icons ---
# Positions are constrained by the cloud's true extent, which is larger than its
# nominal size: the outline grows 3 units outward and the lit rim sits 3 further
# up-left, so a full-size cloud spans cy-31..cy+19 and cx-37..cx+32.
def i_clear(c):      sun(c, 40, 40, 21, 10)
def i_mostly(c):     sun(c, 44, 32, 15, 7); cloud(c, 30, 63, s=0.60)
def i_partly(c):     sun(c, 54, 26, 12, 5); cloud(c, 38, 54, s=0.85)
def i_cloudy(c):
    # A smaller cloud set back and to the right, then the main one in front of
    # it. The front cloud's outline does the occluding, so the two read as two
    # clouds rather than as one doubled contour. Both keep a lit rim - passing
    # lit=None leaves the offset black mass uncovered, which shows as a fat
    # black crescent rather than as no highlight.
    cloud(c, 54, 22, YELLOW, s=0.55)
    cloud(c, 40, 52, YELLOW, s=0.95)
def i_fog(c):
    cloud(c, 42, 36, s=0.9)
    for x0, x1, y in ((10, 58, 60), (20, 70, 68), (14, 52, 75)):
        thick_line(c, x0, y, x1, y, 5, BLACK)
def i_light_rain(c):
    cloud(c, 42, 34, s=0.9)
    for x in (24, 42, 60): drop(c, x, 56, BLACK, 0.9)
def i_heavy_rain(c):
    cloud(c, 42, 32, RED, s=0.9)
    for x, y in ((22, 52), (38, 58), (54, 52), (68, 58)): drop(c, x, y, RED)
def i_showers(c):
    sun(c, 54, 26, 11, 5); cloud(c, 36, 46, s=0.78)
    for x in (26, 44): drop(c, x, 64, BLACK, 0.8)
def i_storm(c):      cloud(c, 42, 32, RED, s=0.9); bolt(c, 38, 55)
def i_drizzle(c):
    cloud(c, 42, 36, s=0.9)
    for x in (26, 42, 58):
        for y in (58, 70): thick_line(c, x, y, x - 3, y + 6, 3, BLACK)
def i_snow(c):
    cloud(c, 42, 34, s=0.9)
    for x, y in ((24, 60), (42, 70), (60, 60)): flake(c, x, y, BLACK)
def i_mixed(c):
    cloud(c, 42, 34, s=0.9); drop(c, 28, 56, BLACK, 0.9); flake(c, 56, 64, BLACK)
def i_sleet(c):
    cloud(c, 42, 34, s=0.9)
    drop(c, 24, 54, BLACK, 0.85); drop(c, 60, 54, BLACK, 0.85)
    disc(c, 42, 68, 8, BLACK); disc(c, 42, 68, 4, WHITE)   # the ice pellet
def i_freezing(c):
    cloud(c, 42, 30, RED, s=0.9)
    for x in (26, 42, 58): drop(c, x, 50, RED, 0.9)
    thick_line(c, 14, 74, 66, 74, 5, BLACK)
def i_hail(c):
    cloud(c, 42, 30, RED, s=0.9)
    for x, y in ((26, 56), (42, 68), (58, 56)):
        disc(c, x, y, 7, BLACK); disc(c, x, y, 4, RED)
def i_wind(c):
    # The strongest gust in red: this icon only appears when the wind is worth
    # mentioning, so red is carrying the same "take notice" meaning as elsewhere.
    gust(c, 8, 22, 34, 8, 6, BLACK)
    gust(c, 8, 44, 44, 9, 6, RED)
    gust(c, 8, 66, 28, 7, 6, BLACK)
def i_wind_rain(c):
    cloud(c, 40, 30, s=0.85)
    gust(c, 6, 58, 26, 7, 5, BLACK)
    gust(c, 6, 72, 20, 6, 5, BLACK)
    drop(c, 62, 48, BLACK, 0.85); drop(c, 62, 62, BLACK, 0.85)


ICONS = [
    ('Clear', i_clear), ('MostlyClear', i_mostly), ('PartlyCloudy', i_partly),
    ('Cloudy', i_cloudy), ('Fog', i_fog), ('LightRain', i_light_rain),
    ('HeavyRain', i_heavy_rain), ('Showers', i_showers), ('Thunderstorm', i_storm),
    ('Drizzle', i_drizzle), ('Snow', i_snow), ('MixedRainSnow', i_mixed),
    ('Sleet', i_sleet), ('FreezingRain', i_freezing), ('Hail', i_hail),
    ('Wind', i_wind), ('WindRain', i_wind_rain),
]


def render_all():
    out, bad = [], []
    for name, fn in ICONS:
        c = canvas()
        _clip[:] = [0, 0, 0, 0]
        fn(c)
        if any(v > 0 for v in _clip):
            bad.append((name, tuple(_clip)))
        out.append((name, c))
    if bad:
        print(f'  CLIPPED at {S}x{S} (left, top, right, bottom overshoot in px):')
        for name, cl in bad:
            print(f'    {name:<14} {cl}')
    else:
        print(f'  no clipping at {S}x{S}')
    return out


def write_png(path, icons, cols=6, scale=2):
    rows = (len(icons) + cols - 1) // cols
    gap = 4
    w, h = cols * (S + gap) + gap, rows * (S + gap) + gap
    img = [[WHITE] * w for _ in range(h)]
    for i, (_, c) in enumerate(icons):
        ox, oy = gap + (i % cols) * (S + gap), gap + (i // cols) * (S + gap)
        for y in range(S):
            for x in range(S):
                img[oy + y][ox + x] = c[y][x]
        for x in range(-1, S + 1):
            img[oy - 1][ox + x] = BLACK
            img[oy + S][ox + x] = BLACK
        for y in range(-1, S + 1):
            img[oy + y][ox - 1] = BLACK
            img[oy + y][ox + S] = BLACK
    raw = b''
    for row in img:
        line = b'\x00'
        for v in row:
            line += bytes(RGB[v]) * scale
        raw += line * scale
    def ch(t, d):
        b = t + d
        return struct.pack('>I', len(d)) + b + struct.pack('>I', zlib.crc32(b) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + ch(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
           + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))
    open(path, 'wb').write(png)
    print(f'{path}: {w * scale}x{h * scale}')


def write_header(path, icons):
    p = f'kWeatherIcon{S}'
    lines = ['#pragma once', '',
             '#include <Arduino.h>', '',
             f'// {S}x{S} weather icons, 4bpp packed (two pixels per byte, high nibble first).',
             '// Palette indices match the panel: white 0x00, red 0x06, yellow 0x0B, black 0x0F.',
             '//',
             '// Generated by tools/gen_icons80.py - edit the generator, not this file.',
             '// Style: white body with a black outline and a lit rim (yellow, or red on severe',
             '// cloud). Rain is black; red marks heavy precipitation, storms and hail so it',
             '// keeps the same "take notice" meaning it has elsewhere on the panel.',
             '//',
             '// The two sizes are the same drawing code at two scales, not one resampled into',
             '// the other, so the hero and the day columns are one piece of design throughout.', '',
             f'constexpr int16_t {p}Width = {S};',
             f'constexpr int16_t {p}Height = {S};',
             f'constexpr uint16_t {p}Bytes = ({p}Width * {p}Height) / 2;', '',
             '// Palette indices for JD79667 COLOR_GET (output 0=black,1=white,2=yellow,3=red)',
             f'constexpr uint8_t {p}IndexBlack = 0x0F;',
             f'constexpr uint8_t {p}IndexWhite = 0x00;',
             f'constexpr uint8_t {p}IndexYellow = 0x0B;',
             f'constexpr uint8_t {p}IndexRed = 0x06;', '']
    for name, c in icons:
        data = []
        for y in range(S):
            for x in range(0, S, 2):
                data.append((c[y][x] << 4) | c[y][x + 1])
        lines.append(f'static const uint8_t {p}{name}[{p}Bytes] PROGMEM = {{')
        for i in range(0, len(data), 16):
            lines.append('    ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 16]) + ',')
        lines.append('};')
        lines.append('')
    open(path, 'w').write('\n'.join(lines))
    print(f'{path}: {len(icons)} icons, {len(icons) * S * S // 2} bytes of pixel data')


if __name__ == '__main__':
    set_size(80)
    icons80 = render_all()
    write_png('docs/images/icons-80x80.png', icons80)
    write_header('include/weather_icons_80x80.h', icons80)

    # The day columns are drawn from the same source at half scale rather than
    # resampled from the 80x80 art: on a palette with no intermediate tones, a
    # 2:1 downsample either eats the outlines or doubles their weight depending
    # on which way each block vote falls. Redrawing keeps a true 1px outline.
    set_size(40)
    icons40 = render_all()
    write_png('docs/images/icons-40x40.png', icons40, scale=4)
    write_header('include/weather_icons_40x40.h', icons40)
