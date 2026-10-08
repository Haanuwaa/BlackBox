#!/usr/bin/env python3
"""Generate BlackBox's vector mark and native icons; Python standard library only.

Run from any directory. Generated assets are committed, so builds need no renderer.
The rounded recorder enclosure, signal trace and recording light share one geometry.
"""
from pathlib import Path
import argparse
import math
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
INK, BLUE, WHITE = '#101923', '#2d9cff', '#f4f8fc'
TRACE = [(60, 144), (88, 144), (108, 98), (136, 180), (157, 126), (172, 144), (196, 144)]


def shapes(state='app'):
    result = [('rect', (8, 8, 240, 240, 48), INK),
              ('rect', (27, 27, 202, 202, 32), BLUE),
              ('rect', (38, 38, 180, 180, 22), INK),
              ('line', (TRACE, 13), WHITE), ('circle', (184, 74, 11), BLUE)]
    if state != 'app':
        result += [('circle', (202, 202, 40), INK)]
        color = {'recording': '#55ddab', 'capturing': BLUE, 'paused': '#ffcc66',
                 'retrying': '#ffcc66', 'error': '#ff6978'}[state]
        result += [('circle', (202, 202, 32), color)]
        if state == 'paused':
            result += [('rect', (189, 185, 8, 34, 2), INK), ('rect', (207, 185, 8, 34, 2), INK)]
        elif state in ('retrying', 'error'):
            result += [('rect', (198, 182, 8, 24, 3), INK), ('circle', (202, 216, 4), INK)]
        elif state == 'capturing':
            result += [('circle', (202, 202, 13), INK)]
    return result


def svg():
    elements = []
    for kind, p, color in shapes():
        if kind == 'rect':
            x, y, w, h, r = p
            elements.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{color}"/>')
        elif kind == 'circle':
            x, y, r = p
            elements.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color}"/>')
        else:
            points, width = p
            coords = ' '.join(f'{x},{y}' for x, y in points)
            elements.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>')
    return '\n'.join(elements)


def distance(kind, p, x, y):
    if kind == 'circle':
        cx, cy, r = p
        return math.hypot(x-cx, y-cy)-r
    if kind == 'rect':
        px, py, w, h, r = p
        qx, qy = abs(x-px-w/2)-w/2+r, abs(y-py-h/2)-h/2+r
        return math.hypot(max(qx, 0), max(qy, 0))+min(max(qx, qy), 0)-r
    points, width = p
    best = 1e9
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        dx, dy = bx-ax, by-ay
        t = max(0, min(1, ((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy)))
        best = min(best, math.hypot(x-ax-t*dx, y-ay-t*dy))
    return best-width/2


def png(size, state='app'):
    # Signed-distance antialiasing, composited in premultiplied alpha.
    layers = [(k, p, tuple(int(c[i:i+2], 16) for i in (1, 3, 5))) for k, p, c in shapes(state)]
    raw = bytearray()
    scale = size/256
    for row in range(size):
        raw.append(0)
        y = (row+0.5)/scale
        for col in range(size):
            x = (col+0.5)/scale
            rgb, alpha = [0., 0., 0.], 0.
            for kind, p, color in layers:
                a = max(0., min(1., 0.5-distance(kind, p, x, y)*scale))
                if a:
                    rgb = [color[i]*a+rgb[i]*(1-a) for i in range(3)]
                    alpha = a+alpha*(1-a)
            raw.extend([round(c/alpha) if alpha else 0 for c in rgb]+[round(alpha*255)])
    def chunk(name, payload):
        return struct.pack('>I', len(payload))+name+payload+struct.pack('>I', zlib.crc32(name+payload))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))+chunk(b'IDAT', zlib.compress(raw, 9))+chunk(b'IEND', b'')


def ico(images):
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6+16*len(images)
    for size, data in images:
        header += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return header+b''.join(data for _, data in images)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    outputs = {}
    mark = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" role="img" aria-label="BlackBox">\n'+svg()+'\n</svg>\n'
    outputs['packaging/branding/blackbox.svg'] = mark.encode()
    outputs['packaging/linux/io.github.Haanuwaa.BlackBox.svg'] = mark.encode()
    logo = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 256" role="img" aria-label="BlackBox Computer Flight Recorder">\n'+svg()+'\n<text x="290" y="139" font-family="Segoe UI, sans-serif" font-size="86" font-weight="600" fill="#2d9cff">BlackBox</text>\n<text x="295" y="184" font-family="Segoe UI, sans-serif" font-size="25" fill="#91a6b9">COMPUTER FLIGHT RECORDER</text>\n</svg>\n'
    outputs['packaging/branding/blackbox-logo.svg'] = logo.encode()
    images = {size: png(size) for size in (16, 20, 24, 32, 40, 48, 64, 128, 256, 512)}
    outputs['packaging/branding/blackbox.png'] = images[256]
    outputs['packaging/branding/blackbox.ico'] = ico([(s, b) for s, b in images.items() if s <= 256])
    for state in ('recording', 'capturing', 'paused', 'retrying', 'error'):
        outputs[f'packaging/branding/tray-{state}.ico'] = ico([(s, png(s, state)) for s in (16, 20, 24, 32, 40, 48)])
    chunks = b''.join(kind+struct.pack('>I', len(images[size])+8)+images[size]
                      for kind, size in [(b'icp4', 16), (b'icp5', 32), (b'icp6', 64), (b'ic07', 128), (b'ic08', 256), (b'ic09', 512)])
    outputs['packaging/branding/blackbox.icns'] = b'icns'+struct.pack('>I', len(chunks)+8)+chunks
    data = images[64]
    lines = ['    '+', '.join(f'0x{v:02x}' for v in data[i:i+16])+',' for i in range(0, len(data), 16)]
    outputs['src/core/brand_icon_data.hpp'] = ('// Generated by scripts/generate-brand-assets.py. Do not edit.\n#pragma once\n#include <array>\nnamespace blackbox::core {\ninline constexpr std::array<unsigned char, '+str(len(data))+'> brand_icon_png{\n'+'\n'.join(lines)+'\n};\n} // namespace blackbox::core\n').encode()
    for name, data in outputs.items():
        path = ROOT/name
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                raise SystemExit(f'Brand asset is stale: {name}')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    print(f'{len(outputs)} brand assets {"verified" if args.check else "generated"}')


if __name__ == '__main__':
    main()
