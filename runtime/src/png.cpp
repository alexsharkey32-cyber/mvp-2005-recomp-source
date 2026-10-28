// Minimal PNG reader for texture replacements: 8-bit grey / grey+alpha / RGB / RGBA / palette (+tRNS),
// non-interlaced. Includes a small zlib inflater (stored, fixed and dynamic Huffman blocks).
#include "ee_context.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct Bits {
    const u8* p; size_t n, pos = 0; u32 buf = 0; int cnt = 0;
    bool fail = false;
    u32 get(int k) {
        while (cnt < k) { if (pos >= n) { fail = true; return 0; } buf |= (u32)p[pos++] << cnt; cnt += 8; }
        u32 v = buf & ((1u << k) - 1); buf >>= k; cnt -= k; return v;
    }
    void align() { buf = 0; cnt = 0; }
};
struct Huff { u16 count[16]; u16 sym[288]; };
static void build(Huff& h, const u8* len, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    h.count[0] = 0;
    u16 offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++) if (len[i]) h.sym[offs[len[i]]++] = (u16)i;
}
static int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)b.get(1);
        int count = h.count[len];
        if (code - count < first) return h.sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (b.fail) return -1;
    }
    return -1;
}
static const u16 kLenBase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const u8 kLenExtra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const u16 kDistBase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const u8 kDistExtra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool inflate(const u8* src, size_t n, std::vector<u8>& out) {
    if (n < 2) return false;
    Bits b{src + 2, n - 2};                      // skip the zlib header
    for (;;) {
        u32 last = b.get(1), type = b.get(2);
        if (b.fail) return false;
        if (type == 0) {
            b.align();
            if (b.pos + 4 > b.n) return false;
            u32 len = b.p[b.pos] | (b.p[b.pos + 1] << 8); b.pos += 4;
            if (b.pos + len > b.n) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len); b.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            u8 lens[320];
            if (type == 1) {
                for (int i = 0; i < 144; i++) lens[i] = 8;
                for (int i = 144; i < 256; i++) lens[i] = 9;
                for (int i = 256; i < 280; i++) lens[i] = 7;
                for (int i = 280; i < 288; i++) lens[i] = 8;
                build(lit, lens, 288);
                for (int i = 0; i < 30; i++) lens[i] = 5;
                build(dist, lens, 30);
            } else {
                int hlit = (int)b.get(5) + 257, hdist = (int)b.get(5) + 1, hclen = (int)b.get(4) + 4;
                static const u8 order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                u8 cl[19] = {};
                for (int i = 0; i < hclen; i++) cl[order[i]] = (u8)b.get(3);
                Huff ch; build(ch, cl, 19);
                int i = 0;
                while (i < hlit + hdist) {
                    int sym = decode(b, ch);
                    if (sym < 0) return false;
                    if (sym < 16) lens[i++] = (u8)sym;
                    else {
                        int rep, val = 0;
                        if (sym == 16) { if (!i) return false; val = lens[i - 1]; rep = 3 + (int)b.get(2); }
                        else if (sym == 17) rep = 3 + (int)b.get(3);
                        else rep = 11 + (int)b.get(7);
                        if (i + rep > hlit + hdist) return false;
                        while (rep--) lens[i++] = (u8)val;
                    }
                }
                build(lit, lens, hlit);
                build(dist, lens + hlit, hdist);
            }
            for (;;) {
                int sym = decode(b, lit);
                if (sym < 0) return false;
                if (sym < 256) out.push_back((u8)sym);
                else if (sym == 256) break;
                else {
                    sym -= 257; if (sym >= 29) return false;
                    int len = kLenBase[sym] + (int)b.get(kLenExtra[sym]);
                    int ds = decode(b, dist); if (ds < 0 || ds >= 30) return false;
                    size_t d = kDistBase[ds] + b.get(kDistExtra[ds]);
                    if (d > out.size()) return false;
                    size_t from = out.size() - d;
                    for (int k = 0; k < len; k++) out.push_back(out[from + k]);
                }
                if (b.fail) return false;
            }
        } else return false;
        if (last) return true;
    }
}
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
}  // namespace

bool png_decode(const std::vector<u8>& f, std::vector<u32>& rgba, int& W, int& H) {
    if (f.size() < 33 || std::memcmp(f.data(), "\x89PNG\r\n\x1a\n", 8)) return false;
    size_t pos = 8;
    int bitdepth = 0, ctype = 0, interlace = 0;
    std::vector<u8> idat; u32 pal[256]; int npal = 0;
    for (int i = 0; i < 256; i++) pal[i] = 0xff000000u;
    W = H = 0;
    while (pos + 12 <= f.size()) {
        u32 len = be32(&f[pos]);
        const u8* type = &f[pos + 4];
        const u8* d = &f[pos + 8];
        if (pos + 12 + len > f.size()) return false;
        if (!std::memcmp(type, "IHDR", 4)) { W = (int)be32(d); H = (int)be32(d + 4); bitdepth = d[8]; ctype = d[9]; interlace = d[12]; }
        else if (!std::memcmp(type, "PLTE", 4)) { npal = (int)(len / 3); for (int i = 0; i < npal && i < 256; i++) pal[i] = 0xff000000u | d[i * 3] | (d[i * 3 + 1] << 8) | (d[i * 3 + 2] << 16); }
        else if (!std::memcmp(type, "tRNS", 4) && ctype == 3) { for (u32 i = 0; i < len && i < 256; i++) pal[i] = (pal[i] & 0xffffff) | ((u32)d[i] << 24); }
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
        else if (!std::memcmp(type, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (W <= 0 || H <= 0 || W > 16384 || H > 16384 || bitdepth != 8 || interlace) return false;
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch) return false;
    std::vector<u8> raw; raw.reserve((size_t)(W * ch + 1) * H);
    if (!inflate(idat.data(), idat.size(), raw)) return false;
    size_t stride = (size_t)W * ch;
    if (raw.size() < (stride + 1) * H) return false;
    std::vector<u8> prev(stride, 0), cur(stride);
    rgba.resize((size_t)W * H);
    for (int y = 0; y < H; y++) {
        const u8* r = &raw[(stride + 1) * y];
        int ft = r[0]; r++;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)ch ? cur[x - ch] : 0, b = prev[x], c = x >= (size_t)ch ? prev[x - ch] : 0, v = r[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: { int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                      v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
            }
            cur[x] = (u8)v;
        }
        u32* o = &rgba[(size_t)y * W];
        for (int x = 0; x < W; x++) {
            const u8* p = &cur[(size_t)x * ch];
            switch (ctype) {
            case 0: o[x] = 0xff000000u | p[0] * 0x010101u; break;
            case 2: o[x] = 0xff000000u | p[0] | (p[1] << 8) | (p[2] << 16); break;
            case 3: o[x] = pal[p[0]]; break;
            case 4: o[x] = ((u32)p[1] << 24) | p[0] * 0x010101u; break;
            case 6: o[x] = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); break;
            }
        }
        std::swap(prev, cur);
    }
    return true;
}

bool png_load(const std::string& path, std::vector<u32>& rgba, int& w, int& h) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::vector<u8> f;
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    if (n > 0) { f.resize((size_t)n); if (std::fread(f.data(), 1, (size_t)n, fp) != (size_t)n) f.clear(); }
    std::fclose(fp);
    return png_decode(f, rgba, w, h);
}
