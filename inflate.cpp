#include "inflate.h"

#include <cstring>

namespace wx {
namespace {

struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;   // pos in bytes
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    int need(int k) {    // read k bits (k <= 16), LSB first
        while (cnt < k) {
            if (pos >= n) { err = true; return 0; }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        int v = (int)(buf & ((1u << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
};

struct Huff {
    uint16_t count[16];
    uint16_t symbol[320];
    bool build(const uint8_t* len, int n) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) ++count[len[i]];
        if (count[0] == n) return true;                // no codes: allowed (e.g. unused distance table)
        int left = 1;
        for (int l = 1; l < 16; ++l) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false;                // over-subscribed
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + count[l]);
        for (int i = 0; i < n; ++i) if (len[i]) symbol[offs[len[i]]++] = (uint16_t)i;
        return true;
    }
    int decode(Bits& b) const {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= b.need(1);
            if (b.err) return -1;
            int c = count[l];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t kLenExtra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t kDistExtra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool codes(Bits& b, std::vector<uint8_t>& out, size_t maxOut, const Huff& lit, const Huff& dist) {
    for (;;) {
        int s = lit.decode(b);
        if (s < 0) return false;
        if (s < 256) {
            if (out.size() >= maxOut) return false;
            out.push_back((uint8_t)s);
        } else if (s == 256) {
            return true;
        } else {
            s -= 257;
            if (s >= 29) return false;
            int len = kLenBase[s] + b.need(kLenExtra[s]);
            int ds = dist.decode(b);
            if (ds < 0 || ds >= 30) return false;
            size_t d = (size_t)kDistBase[ds] + (size_t)b.need(kDistExtra[ds]);
            if (b.err || d > out.size() || out.size() + (size_t)len > maxOut) return false;
            size_t from = out.size() - d;
            for (int i = 0; i < len; ++i) out.push_back(out[from + (size_t)i]);
        }
    }
}

} // namespace

bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t maxOut, size_t* consumed) {
    Bits b;
    b.p = in; b.n = n;
    int last;
    do {
        last = b.need(1);
        int type = b.need(2);
        if (b.err) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;                       // drop to byte boundary
            if (b.pos + 4 > n) return false;
            unsigned len = in[b.pos] | (in[b.pos + 1] << 8), nlen = in[b.pos + 2] | (in[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || b.pos + len > n || out.size() + len > maxOut) return false;
            out.insert(out.end(), in + b.pos, in + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; ++i) l[i] = 8;
            for (; i < 256; ++i) l[i] = 9;
            for (; i < 280; ++i) l[i] = 7;
            for (; i < 288; ++i) l[i] = 8;
            Huff lit, dist;
            uint8_t dl[30];
            std::memset(dl, 5, sizeof dl);
            lit.build(l, 288); dist.build(dl, 30);
            if (!codes(b, out, maxOut, lit, dist)) return false;
        } else if (type == 2) {
            int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
            if (b.err || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t lengths[320];
            std::memset(lengths, 0, sizeof lengths);
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint8_t)b.need(3);
            Huff lc;
            if (!lc.build(lengths, 19)) return false;
            int idx = 0;
            uint8_t all[320];
            std::memset(all, 0, sizeof all);
            while (idx < nlen + ndist) {
                int sym = lc.decode(b);
                if (sym < 0) return false;
                if (sym < 16) { all[idx++] = (uint8_t)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (idx == 0) return false; val = all[idx - 1]; rep = 3 + b.need(2); }
                else if (sym == 17) rep = 3 + b.need(3);
                else rep = 11 + b.need(7);
                if (b.err || idx + rep > nlen + ndist) return false;
                while (rep--) all[idx++] = (uint8_t)val;
            }
            Huff lit, dist;
            if (!lit.build(all, nlen) || !dist.build(all + nlen, ndist)) return false;
            if (!codes(b, out, maxOut, lit, dist)) return false;
        } else {
            return false;
        }
    } while (!last);
    if (consumed) *consumed = b.pos - (size_t)(b.cnt / 8);
    return true;
}

bool gunzip(const std::string& s, std::vector<uint8_t>& out, size_t maxOut) {
    const uint8_t* p = (const uint8_t*)s.data();
    size_t n = s.size();
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return false;
    int flg = p[3];
    size_t i = 10;
    if (flg & 4) { if (i + 2 > n) return false; i += 2 + (p[i] | (p[i + 1] << 8)); }
    if (flg & 8) { while (i < n && p[i]) ++i; ++i; }
    if (flg & 16) { while (i < n && p[i]) ++i; ++i; }
    if (flg & 2) i += 2;
    if (i >= n) return false;
    out.clear();
    return inflateRaw(p + i, n - i, out, maxOut);
}

bool zlibDecompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    if (n < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return false;
    return inflateRaw(in + 2, n - 2, out, maxOut);
}

} // namespace wx
