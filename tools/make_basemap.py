#!/usr/bin/env python3
"""Builds third_party/basemap/basemap.bin, the radar map's background, from Natural Earth (public domain) GeoJSON:
coastlines, land borders, US/Canada/... state and province lines, lakes (1:50 million). Usage:
  make_basemap.py <natural-earth-vector/geojson> <out.bin>
Format (little endian): b"WXBM", u32 version 1, u32 line count; per line: u8 kind (0 coast, 1 border, 2 state, 3 lake),
varint point count, then each point as zigzag varint deltas of (lon, lat) in 1/2000 degree (the first from 0,0)."""
import json, struct, sys, os

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n: out.append(b | 0x80)
        else:
            out.append(b); return bytes(out)

def zz(n): return (n << 1) ^ (n >> 63)

def lines(path):
    g = json.load(open(path))
    for f in g["features"]:
        geo = f["geometry"]
        if geo is None: continue
        t, c = geo["type"], geo["coordinates"]
        if t == "LineString": yield c
        elif t == "MultiLineString": yield from c
        elif t == "Polygon": yield from c
        elif t == "MultiPolygon":
            for p in c: yield from p

src, out = sys.argv[1], sys.argv[2]
kinds = [(0, "ne_50m_coastline.geojson"), (1, "ne_50m_admin_0_boundary_lines_land.geojson"),
         (2, "ne_50m_admin_1_states_provinces_lines.geojson"), (3, "ne_50m_lakes.geojson")]
body = bytearray(); n = 0; pts = 0
for kind, name in kinds:
    for ln in lines(os.path.join(src, name)):
        q = []
        for lon, lat in ln:
            p = (int(round(lon * 2000)), int(round(lat * 2000)))
            if not q or p != q[-1]: q.append(p)
        if len(q) < 2: continue
        body.append(kind); body += varint(len(q))
        px = py = 0
        for x, y in q:
            body += varint(zz(x - px)) + varint(zz(y - py)); px, py = x, y
        n += 1; pts += len(q)
open(out, "wb").write(b"WXBM" + struct.pack("<II", 1, n) + body)
print(f"{n} lines, {pts} points, {len(body) + 12} bytes")
