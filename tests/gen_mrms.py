#!/usr/bin/env python3
"""Writes mock MRMS products (gzipped GRIB2) with a storm near 37.9N 97.8W for the plugin harness.
Usage: gen_mrms.py <outdir>   -> mrms_refl.grib2.gz (simple packing), mrms_top.grib2.gz (PNG packing)"""
import gzip, math, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_grib as g

g.NI, g.NJ = 1000, 700
g.LA1, g.LO1 = 41.995, 360 - 102.495
g.DX = g.DY = 0.01

def dist(lat, lon, la0, lo0):
    return math.hypot(lat - la0, (lon - lo0) * math.cos(math.radians(lat)))

def build(kind):
    vals = []
    for j in range(g.NJ):
        lat = g.LA1 - j * g.DY
        for i in range(g.NI):
            lon = g.LO1 + i * g.DX - 360
            d = dist(lat, lon, 37.9, -97.8)
            d2 = dist(lat, lon, 38.9, -96.5)
            if kind == 'refl':
                v = -99.0
                if d < 0.40: v = round(58 - 100 * d, 1)
                elif d2 < 0.25: v = 24.0
                vals.append(v)
            else:
                v = -3.0
                if d < 0.40: v = round(max(3.0, 12.0 - 20 * d), 1)
                elif d2 < 0.25: v = 4.5
                vals.append(v)
    return vals

if __name__ == '__main__':
    out = sys.argv[1]
    la2 = g.LA1 - (g.NJ - 1) * g.DY; lo2 = g.LO1 + (g.NI - 1) * g.DX
    refl = g.message(0, g.LA1, g.LO1, la2, lo2, build('refl'), 'simple')
    top = g.message(0, g.LA1, g.LO1, la2, lo2, build('top'), 'png16')
    gzip.open(out + '/mrms_refl.grib2.gz', 'wb').write(refl)
    gzip.open(out + '/mrms_top.grib2.gz', 'wb').write(top)
    print('refl', len(refl), 'top', len(top))
