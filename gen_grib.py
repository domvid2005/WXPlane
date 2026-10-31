#!/usr/bin/env python3
"""Writes small test GRIB2 messages (a CONUS-like 0.01 degree grid) in the layouts MRMS files might use.
Usage: gen_grib.py <outdir>"""
import struct, sys, zlib, math

def sm32(v):  # sign-magnitude
    return struct.pack('>I', (abs(v) | (0x80000000 if v < 0 else 0)))
def sm16(v):
    return struct.pack('>H', (abs(v) | (0x8000 if v < 0 else 0)))

NI, NJ = 700, 450            # 7 x 4.5 degrees
LA1, LO1 = 37.495, 360 - 102.495   # NW corner, longitudes in 0..360 like MRMS
DX = DY = 0.01

def value(lat, lon):         # lon -180..180
    return round(lat * 3 + (lon + 180) * 0.5, 1)

def field(scan='nw', bitmap_hole=False):
    vals = []
    for j in range(NJ):
        for i in range(NI):
            lat = LA1 - j * DY
            lon = (LO1 + i * DX) - 360
            vals.append(value(lat, lon))
    return vals

def sect3(scan_byte, la1, lo1):
    b = bytearray()
    b += struct.pack('>IBBIBBH', 72, 3, 0, NI * NJ, 0, 0, 0)       # len,num,src,npts,optlen,interp,template
    b += bytes([6]) + bytes([0]) + struct.pack('>I', 0) + bytes([0]) + struct.pack('>I', 0) + bytes([0]) + struct.pack('>I', 0)  # shape, radius scale/val
    b += struct.pack('>I', 0) * 0
    b += struct.pack('>II', 0, 0)                                 # major axis, minor axis (scale+value omitted below)
    return bytes(b)

def grid_section(scan_byte, la1, lo1, la2, lo2):
    # Section 3 layout (template 3.0), offsets per GRIB2 spec
    s = bytearray(72)
    s[0:4] = struct.pack('>I', 72); s[4] = 3; s[5] = 0
    s[6:10] = struct.pack('>I', NI * NJ); s[10] = 0; s[11] = 0
    s[12:14] = struct.pack('>H', 0)
    s[14] = 6                                            # shape of earth
    # 15..29: radius/axis scale+values (zeros)
    s[30:34] = struct.pack('>I', NI); s[34:38] = struct.pack('>I', NJ)
    s[38:42] = struct.pack('>I', 0); s[42:46] = struct.pack('>I', 0xFFFFFFFF)
    s[46:50] = sm32(round(la1 * 1e6)); s[50:54] = sm32(round(lo1 * 1e6))
    s[54] = 0x30
    s[55:59] = sm32(round(la2 * 1e6)); s[59:63] = sm32(round(lo2 * 1e6))
    s[63:67] = struct.pack('>I', round(DX * 1e6)); s[67:71] = struct.pack('>I', round(DY * 1e6))
    s[71] = scan_byte
    return bytes(s)

def sect5_simple(npts, vals, dec=1):
    ints = [round(v * 10 ** dec) for v in vals]
    ref = min(ints)
    rng = max(ints) - ref
    nbits = max(1, rng.bit_length())
    s = bytearray(21)
    s[0:4] = struct.pack('>I', 21); s[4] = 5
    s[5:9] = struct.pack('>I', npts); s[9:11] = struct.pack('>H', 0)
    s[11:15] = struct.pack('>f', float(ref)); s[15:17] = sm16(0); s[17:19] = sm16(dec)
    s[19] = nbits; s[20] = 0
    # pack bits MSB first
    acc = 0; nb = 0; out = bytearray()
    for x in ints:
        acc = (acc << nbits) | (x - ref); nb += nbits
        while nb >= 8:
            out.append((acc >> (nb - 8)) & 255); nb -= 8
        acc &= (1 << nb) - 1
    if nb: out.append((acc << (8 - nb)) & 255)
    return bytes(s), bytes(out)

def sect5_png(npts, vals, dec=1, depth=16):
    ints = [round(v * 10 ** dec) for v in vals]
    ref = min(ints)
    nbits = depth
    s = bytearray(21)
    s[0:4] = struct.pack('>I', 21); s[4] = 5
    s[5:9] = struct.pack('>I', npts); s[9:11] = struct.pack('>H', 41)
    s[11:15] = struct.pack('>f', float(ref)); s[15:17] = sm16(0); s[17:19] = sm16(dec)
    s[19] = nbits; s[20] = 0
    w = NI; h = NJ
    rows = b''
    prev = bytes(w * depth // 8)
    for y in range(h):
        r = b''.join(struct.pack('>H' if depth == 16 else '>B', x - ref) for x in ints[y * w:(y + 1) * w])
        # filter 1 (sub), bytewise with bpp = depth/8
        bpp = depth // 8
        fr = bytearray(len(r))
        for i in range(len(r)):
            a = r[i - bpp] if i >= bpp else 0
            fr[i] = (r[i] - a) & 255
        rows += b'\x01' + bytes(fr)
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
    return bytes(s), png

def message(scan_byte, la1, lo1, la2, lo2, vals, packing, bitmap=None):
    s1 = bytearray(21); s1[0:4] = struct.pack('>I', 21); s1[4] = 1
    s3 = grid_section(scan_byte, la1, lo1, la2, lo2)
    s4 = struct.pack('>IBHHH', 9, 4, 0, 0, 0)[:9]
    if bitmap is not None:
        present = [v for v, b in zip(vals, bitmap) if b]
    else:
        present = vals
    if packing == 'simple': s5, data = sect5_simple(len(present), present)
    elif packing == 'png16': s5, data = sect5_png(len(present), present, depth=16)
    elif packing == 'png8':  s5, data = sect5_png(len(present), present, depth=8)
    if bitmap is not None:
        bm = bytearray((len(bitmap) + 7) // 8)
        for k, b in enumerate(bitmap):
            if b: bm[k // 8] |= 0x80 >> (k % 8)
        s6 = struct.pack('>IBB', 6 + len(bm), 6, 0) + bytes(bm)
    else:
        s6 = struct.pack('>IBB', 6, 6, 255)
    s7 = struct.pack('>IB', 5 + len(data), 7) + data
    body = bytes(s1) + s3 + s4 + s5 + s6 + s7
    total = 16 + len(body) + 4
    s0 = b'GRIB' + b'\x00\x00' + bytes([209, 2]) + struct.pack('>Q', total)
    return s0 + body + b'7777'

if __name__ == '__main__':
    out = sys.argv[1]
    vals = field()
    la2 = LA1 - (NJ - 1) * DY; lo2 = LO1 + (NI - 1) * DX
    open(out + '/simple.grib2', 'wb').write(message(0, LA1, LO1, la2, lo2, vals, 'simple'))
    open(out + '/png16.grib2', 'wb').write(message(0, LA1, LO1, la2, lo2, vals, 'png16'))
    # south-to-north storage (scan flag 0x40): first row is the southern edge
    rows = [vals[j * NI:(j + 1) * NI] for j in range(NJ)]
    flipped = [v for r in reversed(rows) for v in r]
    open(out + '/south_up.grib2', 'wb').write(message(0x40, la2, LO1, LA1, lo2, flipped, 'simple'))
    # bitmap with holes: every 7th point missing (values at those points are not stored)
    bm = [(k % 7) != 0 for k in range(NI * NJ)]
    open(out + '/bitmap.grib2', 'wb').write(message(0, LA1, LO1, la2, lo2, vals, 'simple', bm))
