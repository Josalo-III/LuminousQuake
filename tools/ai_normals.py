#!/usr/bin/env python3
"""AI generated normals: an experiment on E1M1's tech08_1 wall texture.

Infers a height field from the 128x128 original, separates its paint from the
lighting painted into it, rebuilds both at 1024x1024, and relights the result
with cast shadows, ambient occlusion and specular from the height field. See
docs/AI_GENERATED_NORMALS.md for the method and the results.

The geometry is fitted by hand to tech08_1 (outline, strip spacing, rivet
size); it is not a general converter. Outputs go to an ignored directory
because they are derived from id's textures.

Needs numpy, scipy and Pillow.
"""
import argparse
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as nd

T = 128            # texels across tech08_1
S = 8              # samples per texel at 1024


# --- reading the texture -------------------------------------------------------

def read_texture(pak, bsp_name, tex_name):
    data = Path(pak).read_bytes()
    _, off, ln = struct.unpack_from('<4sii', data, 0)
    files = {}
    for i in range(ln // 64):
        n, o, l = struct.unpack_from('<56sii', data, off + i * 64)
        files[n.split(b'\0')[0].decode()] = data[o:o + l]
    pal = np.frombuffer(files['gfx/palette.lmp'], np.uint8).reshape(256, 3)
    bsp = files[bsp_name]
    toff = struct.unpack_from('<i', bsp, 4 + 2 * 8)[0]
    count = struct.unpack_from('<i', bsp, toff)[0]
    for o in struct.unpack_from('<%di' % count, bsp, toff + 4):
        if o < 0:
            continue
        b = toff + o
        name, w, h, m0 = struct.unpack_from('<16sIIi', bsp, b)
        if name.split(b'\0')[0].decode() == tex_name:
            idx = np.frombuffer(bsp[b + m0:b + m0 + w * h], np.uint8).reshape(h, w)
            return pal[idx].astype(float)
    raise SystemExit('%s not found in %s' % (tex_name, bsp_name))


def smooth(e0, e1, v):
    t = np.clip((v - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# --- height field ------------------------------------------------------------------

# Olive/plate outline, fitted to the dark edge line: x = X0 + SLOPE*y, mirrored
# about x = 64.5; the plates' bottom corners are rounded by FILLET.
SLOPE, X0 = 0.3068, 19.76
TOP, BOT, FILLET = 19.5, 120.0, 22.0
STRIPS = list(range(23, 120, 13))        # first row of each raised strip; 5 rows tall


def left_plate(x, y):
    """Signed distance to the left plate, negative inside."""
    a = -(X0 + SLOPE * y - x) / np.hypot(1, SLOPE)
    b = -(BOT - y)
    u = np.hypot(np.maximum(FILLET + a, 0), np.maximum(FILLET + b, 0))
    rounded = np.minimum(-FILLET, np.maximum(a, b)) + u
    return np.maximum(rounded, -(y - TOP))


class Features:
    """What the depth model reads from the original's pixels."""

    def __init__(self, rgb):
        self.rgb = rgb
        self.lum = lum = rgb @ [0.3, 0.59, 0.11]
        # strip rivets: a bright pixel over a dark one on row y0+2, every third column
        self.strip_rivets = [(x, y0) for y0 in STRIPS for x in range(T)
                             if lum[y0 + 2, x] - lum[y0 + 3, x] > 35]
        # olive rivets: dark minima; the head sits half a texel above
        s = nd.gaussian_filter(lum, 1.0, mode='wrap')
        pk = (s == nd.minimum_filter(s, 7, mode='wrap')) & (s < 38)
        self.rivets = [(x + 0.5, y - 0.5) for y, x in zip(*np.where(pk))
                       if 30 < x < 98 and 25 < y < 122]

    def rivet_keepout(self):
        k = np.zeros((T, T), bool)
        for cx, cy in self.rivets:
            k[int(cy) - 3:int(cy) + 9, int(cx) - 3:int(cx) + 3] = True   # head + shadow trail
        return k

    def damage(self, olive_t):
        """Speckles that stand out from the olive's local median, 0..1 per texel."""
        o = nd.binary_erosion(olive_t, iterations=1, border_value=1) & ~self.rivet_keepout()
        res = np.abs(self.lum - nd.median_filter(self.lum, 5, mode='wrap'))
        return smooth(8, 22, res) * o


def build_height(f, s):
    """Height in texels and material layers, at s samples per texel."""
    n = T * s
    yc, xc = (np.mgrid[0:n, 0:n] + 0.5) / s
    plate_sdf = np.minimum(left_plate(xc, yc), left_plate(129 - xc, yc))
    olive_w = smooth(-1.1, 1.1, plate_sdf)

    strips = np.zeros((n, n)); strip_rivets = np.zeros((n, n))
    for y0 in STRIPS:
        strips = np.maximum(strips, smooth(y0, y0 + 1.0, yc) * (1 - smooth(y0 + 4.6, y0 + 5.2, yc)))
    for x, y0 in f.strip_rivets:
        dist = np.hypot(xc - (x + 0.5), yc - (y0 + 3.0))
        strip_rivets = np.maximum(strip_rivets, np.clip(1 - (dist / 0.8) ** 2, 0, 1) ** 0.6)
    plate_h = 0.8 * strips + 0.5 * strip_rivets * strips

    olive_rivets = np.zeros((n, n))
    for cx, cy in f.rivets:
        dist = np.hypot(xc - cx, yc - cy)
        olive_rivets = np.maximum(olive_rivets, np.clip(1 - (dist / 2.4) ** 2, 0, 1) ** 0.6)

    c = np.arange(T) + 0.5
    olive_t = smooth(-1.1, 1.1, np.minimum(left_plate(*np.meshgrid(c, c)),
                                           left_plate(*np.meshgrid(129 - c, c)))) > 0.99
    dmg = nd.zoom(f.damage(olive_t), s, order=1, mode='grid-wrap', grid_mode=True)
    dmg = nd.gaussian_filter(dmg, 0.6 * s, mode='wrap')
    dmg /= max(dmg.max(), 1e-6)
    olive_h = 2.5 + 1.2 * olive_rivets - 0.7 * dmg

    height = (1 - olive_w) * plate_h + olive_w * olive_h
    return dict(height=height, olive_w=olive_w, strips=strips, olive_rivets=olive_rivets,
                dmg=dmg, plate_sdf=plate_sdf)


# --- paint without lighting --------------------------------------------------------

def fill(img, ok, sy, sx):
    """Normalized convolution, widening until every texel has a value. Tiles wrap."""
    out = np.where(ok[..., None], img, 0.0); have = ok.copy(); k = 1.0
    while not have.all() or k == 1.0:
        w = nd.gaussian_filter(ok.astype(float), (sy * k, sx * k), mode='wrap')
        v = np.stack([nd.gaussian_filter(img[..., c] * ok, (sy * k, sx * k), mode='wrap')
                      for c in range(3)], -1)
        new = ~have & (w > 1e-3)
        out[new] = v[new] / w[new, None]; have |= new; k *= 1.5
    return out


def grain(img, ok, base, seed=7):
    """Fill the holes' fine detail by copying it from valid texels at a fixed list of
    offsets, so neighbouring holes take the same offset and get coherent patches."""
    hf = img - base; out = np.where(ok[..., None], hf, 0.0); todo = ~ok
    rng = np.random.default_rng(seed)
    for dy, dx in rng.integers(-14, 15, (64, 2)):
        src_ok = np.roll(np.roll(ok, -dy, 0), -dx, 1); src = np.roll(np.roll(hf, -dy, 0), -dx, 1)
        take = todo & src_ok
        out[take] = src[take]; todo &= ~take
    return base + out


def upscale(img, s):
    """Lanczos on a 3x3 tiling so the edges wrap."""
    n = T * s; big = np.tile(img, (3, 3, 1)); out = np.empty((n, n, 3))
    for c in range(3):
        im = Image.fromarray(big[..., c].astype(np.float32)).resize((3 * n, 3 * n), Image.LANCZOS)
        out[..., c] = np.asarray(im)[n:2 * n, n:2 * n]
    return out


def delight(f, l1, l8):
    """Inpaint the texels whose colour is painted-in lighting, per material, then
    rebuild each material's paint at 1024 and join them on the 1024 outlines."""
    rgb = f.rgb
    olive = l1['olive_w'] > 0.5; plate = ~olive
    edge = np.abs(l1['plate_sdf']) < 1.6                # bevel and the shading either side
    rows = np.arange(T)[:, None] * np.ones((1, T), int)
    strip_body = np.zeros((T, T), bool); strip_lit = np.zeros((T, T), bool)
    plate_lit = np.zeros((T, T), bool)
    for y0 in STRIPS:
        strip_body |= (rows == y0 + 1) | (rows == y0 + 3)
        strip_lit |= (rows == y0) | (rows == y0 + 2) | (rows == y0 + 4)   # top, rivets, bottom
        plate_lit |= (rows == y0 + 5) | (rows == y0 + 6)                  # shadow under the strip
    plate_lit |= (rows >= 19) & (rows <= 22)                              # under the olive's top lip
    rivet_px = np.zeros((T, T), bool)
    for x, y0 in f.strip_rivets:
        rivet_px[y0 + 2:y0 + 4, x] = True
    dmg = f.damage(olive & ~edge)

    in_strip = plate & (strip_body | strip_lit)
    valid = {
        'plate': plate & ~in_strip & ~plate_lit & ~edge,
        'strip': plate & strip_body & ~rivet_px & ~edge,
        'olive': olive & ~edge & ~nd.binary_dilation(f.rivet_keepout(), np.ones((5, 1), bool),
                                                     origin=(-2, 0)) & (dmg < 0.1),
    }
    olive_low = nd.gaussian_filter(fill(rgb, valid['olive'], 2.0, 2.0), (2.0, 2.0, 0), mode='wrap')
    paint = {
        'plate': fill(rgb, valid['plate'], 3.0, 0.6),   # vertical streaks carry across the gaps
        'strip': fill(rgb, valid['strip'], 0.6, 2.5),   # strips run horizontally
        'olive': np.where(valid['olive'][..., None], rgb, grain(rgb, valid['olive'], olive_low)),
    }
    paint['strip'] = np.where(valid['strip'][..., None],
                              nd.uniform_filter1d(paint['strip'], 3, axis=1, mode='wrap'), paint['strip'])

    lit_mask = ~(valid['plate'] | valid['strip'] | valid['olive'])
    zones = rgb.copy(); zones[lit_mask] = zones[lit_mask] * 0.3 + np.array([255, 0, 255]) * 0.7

    p = {k: upscale(v, S) for k, v in paint.items()}
    # bare metal where the damage cuts through the olive paint
    metal = np.median(paint['plate'][valid['plate']], axis=0) * 0.7
    ow = l8['olive_w'][..., None]; sw = l8['strips'][..., None]; dw = l8['dmg'][..., None]
    plate_paint = p['plate'] * (1 - sw) + p['strip'] * sw
    olive_paint = p['olive'] * (1 - 0.6 * dw) + metal * 0.6 * dw
    return np.clip(plate_paint * (1 - ow) + olive_paint * ow, 0, 255), zones, int(lit_mask.sum())


# --- lighting --------------------------------------------------------------------------

LIGHTS = {
    'top':     (0.0, -0.77, 0.64),     # from above, about 40 degrees up: the original's direction
    'topleft': (-0.6, -0.6, 0.53),
    'left':    (-0.9, -0.1, 0.42),     # grazing
    'right':   (0.9, -0.1, 0.42),
    'below':   (0.0, 0.8, 0.6),
}


class Lighter:
    """Diffuse, soft cast shadows marched through the height field, height-based
    ambient occlusion and per-material specular, in linear light."""

    def __init__(self, l8, albedo):
        h = self.h = l8['height']; n = h.shape[0]
        a = albedo / 255.0
        self.lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
        gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * S / 2
        gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * S / 2
        nrm = np.dstack([-gx, -gy, np.ones_like(h)])
        self.n = nrm / np.linalg.norm(nrm, axis=2, keepdims=True)
        self.yy, self.xx = np.mgrid[0:n, 0:n].astype(float)
        # plates: semi-gloss metal; olive: nearly matte paint, glossier rivets; cuts: dull metal
        ow = l8['olive_w']; dmg = l8['dmg'] * ow; riv = l8['olive_rivets'] * ow
        self.ks = (1 - ow) * 0.10 + ow * (0.004 + 0.04 * riv) + dmg * 0.02
        self.pw = (1 - ow) * 40 + ow * (20 + 20 * riv) + dmg * 10
        self.metal_tint = (1 - ow)[..., None] * 0.6        # metal specular takes some of its colour
        self.ao = self.occlusion()

    def sample(self, img, dx, dy):
        return nd.map_coordinates(img, [self.yy + dy, self.xx + dx], order=1, mode='grid-wrap')

    def soft_shadow(self, L, k=10.0):
        h = self.h
        d = np.hypot(L[0], L[1]); tan_e = L[2] / d; ux, uy = L[0] / d, L[1] / d
        res = np.ones_like(h); t = 0.25
        while t * tan_e < h.max() + 0.1:                     # beyond this the ray clears everything
            gap = (h + t * tan_e) - self.sample(h, ux * t * S, uy * t * S)
            res = np.minimum(res, np.clip(k * gap / t + 0.5, 0, 1))
            t += 0.25
        return res

    def occlusion(self, radii=(0.5, 1, 2, 4, 7)):
        h = self.h; ao = np.zeros_like(h)
        for a in np.linspace(0, 2 * np.pi, 12, endpoint=False):
            horizon = np.zeros_like(h)
            for r in radii:
                rise = self.sample(h, np.cos(a) * r * S, np.sin(a) * r * S) - h
                horizon = np.maximum(horizon, np.arctan2(rise, r))
            ao += 1 - np.sin(horizon)
        return ao / 12

    def light(self, L, amb=0.22):
        L = np.array(L, float); L /= np.linalg.norm(L)
        sh = self.soft_shadow(L)
        ndl = np.clip(self.n @ L, 0, None)
        i = (1 - amb) / max(L[2], 0.35)                      # flat, unshadowed paint keeps its colour
        hv = L + [0, 0, 1]; hv /= np.linalg.norm(hv)
        spec = self.ks * (np.clip(self.n @ hv, 0, None) ** self.pw) * (self.pw + 8) / 8 * (ndl > 0) * sh * i
        spec_col = spec[..., None] * (self.metal_tint * self.lin + (1 - self.metal_tint))
        col = self.lin * (amb * self.ao[..., None] + i * (ndl * sh)[..., None]) + spec_col
        col = np.where(col < 0.8, col, 0.8 + 0.2 * np.tanh((col - 0.8) / 0.2))   # soft shoulder
        srgb = np.where(col <= 0.0031308, col * 12.92, 1.055 * np.clip(col, 0, None) ** (1 / 2.4) - 0.055)
        return (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)


def norm_map(height):
    """DarkPlaces _norm: tangent normal in RGB with green up the image, height in alpha."""
    gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * S / 2
    gy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * S / 2
    nrm = np.dstack([-gx, gy, np.ones_like(height)])
    nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)
    rgba = np.dstack([nrm * 0.5 + 0.5, height / height.max()])
    return (rgba * 255 + 0.5).astype(np.uint8)


def sheet(panels, path, w=512):
    g = 10; cols = 4; rws = (len(panels) + cols - 1) // cols
    out = Image.new('RGB', (cols * w + (cols + 1) * g, rws * (w + 22) + g), (24, 24, 24))
    dr = ImageDraw.Draw(out)
    for i, (name, img) in enumerate(panels):
        im = Image.fromarray(img).convert('RGB')
        im = im.resize((w, w), Image.NEAREST if im.size[0] < w else Image.LANCZOS)
        x = g + (i % cols) * (w + g); y = g // 2 + (i // cols) * (w + 22)
        dr.text((x, y + 4), name, fill=(220, 220, 220)); out.paste(im, (x, y + 20))
    out.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--pak', default='quake-complete/Quake/id1/pak0.pak')
    ap.add_argument('--out', default='build/ai_normals')
    args = ap.parse_args()
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)

    rgb = read_texture(args.pak, 'maps/e1m1.bsp', 'tech08_1')
    f = Features(rgb)
    l1 = build_height(f, 1); l8 = build_height(f, S)
    albedo, zones, inpainted = delight(f, l1, l8)
    lighter = Lighter(l8, albedo)

    u8 = lambda a: np.clip(a, 0, 255).astype(np.uint8)
    Image.fromarray(u8(rgb)).save(out / 'original.png')
    Image.fromarray(u8(albedo)).save(out / 'tech08_1.png')
    Image.fromarray(norm_map(l8['height'])).save(out / 'tech08_1_norm.png')
    Image.fromarray(u8(l8['height'] / l8['height'].max() * 255)).save(out / 'height.png')
    Image.fromarray(u8(lighter.ao * 255)).save(out / 'ao.png')
    lit = {}
    for name, L in LIGHTS.items():
        lit[name] = lighter.light(L)
        Image.fromarray(lit[name]).save(out / ('lit_%s.png' % name))
    sheet([('original 128 (nearest)', u8(rgb)), ('texels inpainted (magenta)', u8(zones)),
           ('inferred paint, no lighting', u8(albedo)), ('height', u8(l8['height'] / l8['height'].max() * 255)),
           ('lit from above (original direction)', lit['top']), ('lit from top-left', lit['topleft']),
           ('grazing from the left', lit['left']), ('grazing from the right', lit['right']),
           ('lit from below', lit['below']), ('ambient occlusion', u8(lighter.ao * 255))],
          out / 'sheet.png')

    flat = l8['olive_w'] > 0.99
    orig = np.repeat(np.repeat(rgb, S, 0), S, 1)
    print('olive rivets %d, strip rivets %d, texels inpainted %d' % (len(f.rivets), len(f.strip_rivets), inpainted))
    for name, m in (('olive', flat), ('plate', ~flat)):
        print('%s mean colour: original %s, lit from above %s'
              % (name, orig[m].mean(0).round(1), lit['top'][m].astype(float).mean(0).round(1)))
    print('wrote', out)


if __name__ == '__main__':
    main()
