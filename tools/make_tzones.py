#!/usr/bin/env python3
"""Builds third_party/basemap/tzones.bin, the map's "Time Zone" layer and the local-time lookup, from Natural Earth's
ne_10m_time_zones.geojson (public domain). Usage:
  make_tzones.py <ne_10m_time_zones.geojson> <out.bin>
Format (little endian): b"WXTZ", u32 version 1;
  u32 line count, per line: varint point count, then zigzag varint deltas of (lon, lat) in 1/2000 degree (the first from 0,0)
    (the zone boundaries, each shared edge once, simplified; for drawing);
  u32 zone count, per zone: i8 standard UTC offset in quarter hours, varint ring count, per ring the same point list as a line
    (simplified polygons; a point is in the zone when it is inside an odd number of its rings)."""
import json, struct, sys

Q = 2000          # points in 1/2000 degree
TOL_LINE = 0.01   # simplification (degrees) of the drawn lines
TOL_POLY = 0.02   # and of the lookup polygons

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n: out.append(b | 0x80)
        else:
            out.append(b); return bytes(out)

def zz(n): return (n << 1) ^ (n >> 63)

def dp(pts, tol):   # Douglas-Peucker on (x, y) in degrees, iterative
    if len(pts) < 3: return pts
    keep = [False] * len(pts); keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]; bx, by = pts[b]
        dx, dy = bx - ax, by - ay; L = (dx * dx + dy * dy) ** 0.5
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            d = abs(dx * (ay - py) - dy * (ax - px)) / L if L > 0 else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > best: best, bi = d, i
        if best > tol and bi > 0:
            keep[bi] = True; stack.append((a, bi)); stack.append((bi, b))
    return [p for p, k in zip(pts, keep) if k]

def encode(pts):
    out = bytearray(varint(len(pts)))
    x = y = 0
    for lon, lat in pts:
        X, Y = int(round(lon * Q)), int(round(lat * Q))
        out += varint(zz(X - x)) + varint(zz(Y - y)); x, y = X, Y
    return out

g = json.load(open(sys.argv[1]))
zones = []      # (quarter hours, [rings])
edges = {}      # undirected quantized edge -> count
for f in g["features"]:
    geo = f["geometry"]
    if geo is None: continue
    polys = [geo["coordinates"]] if geo["type"] == "Polygon" else geo["coordinates"]
    qh = int(round(float(f["properties"]["zone"]) * 4))
    rings = []
    for poly in polys:
        for ring in poly:
            q = []
            for lon, lat in ring:
                p = (int(round(lon * Q)), int(round(lat * Q)))
                if not q or q[-1] != p: q.append(p)
            for a, b in zip(q, q[1:]):
                if a != b: k = (a, b) if a < b else (b, a); edges[k] = edges.get(k, 0) + 1
            rings.append(q)
    zones.append((qh, rings))

# boundary lines: every edge once, minus the world's frame (lon +-180, lat +-90); chained into polylines
frame = lambda a, b: (abs(a[0]) == 180 * Q and a[0] == b[0]) or (abs(a[1]) >= 90 * Q - 2 and abs(b[1]) >= 90 * Q - 2)
adj = {}
for a, b in edges:
    if frame(a, b): continue
    adj.setdefault(a, []).append(b); adj.setdefault(b, []).append(a)
used = set()
def take(a, b): used.add((a, b) if a < b else (b, a))
def isused(a, b): return ((a, b) if a < b else (b, a)) in used
lines = []
starts = [p for p, n in adj.items() if len(n) != 2] + list(adj.keys())   # chain from junctions/ends first, then loops
for s in starts:
    for nb in adj[s]:
        if isused(s, nb): continue
        line = [s, nb]; take(s, nb); cur, prev = nb, s
        while len(adj[cur]) == 2:
            nxt = adj[cur][0] if adj[cur][1] == prev else adj[cur][1]
            if isused(cur, nxt): break
            take(cur, nxt); line.append(nxt); prev, cur = cur, nxt
        lines.append(line)

body = bytearray(b"WXTZ") + struct.pack("<I", 1)
outl = [dp([(x / Q, y / Q) for x, y in L], TOL_LINE) for L in lines]
outl = [L for L in outl if len(L) >= 2]
body += struct.pack("<I", len(outl))
for L in outl: body += encode(L)
body += struct.pack("<I", len(zones))
for qh, rings in zones:
    rs = [dp([(x / Q, y / Q) for x, y in r], TOL_POLY) for r in rings]
    rs = [r for r in rs if len(r) >= 4]
    body += struct.pack("<b", qh) + varint(len(rs))
    for r in rs: body += encode(r)
open(sys.argv[2], "wb").write(body)
print(f"{len(outl)} lines, {len(zones)} zones, {len(body)} bytes")
