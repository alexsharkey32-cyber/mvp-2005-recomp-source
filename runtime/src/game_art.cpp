// Art from the user's own disc for the launcher and the pause menu: the game's logo, team wordmarks and colours,
// ballpark panoramas and menu photos. Nothing is shipped with the recomp; the pictures are decoded from the ISO
// when the launcher starts.
//
// The files are EA shape files (.ssh, "SHPS") inside BIGF archives, most of them RefPack-compressed:
//   type 2   8-bit pixels + 256-colour palette (PS2 CLUT order)
//   type 5   32-bit RGBA
//   type 14  the game's GS-side compression, 3 bits per pixel: the PS2 decompresses it on the graphics chip.
//            Layer A (w/8 x h/4, 32-bit) holds four base palette indices per 8x4 block; layer B holds a 2-bit
//            offset per pixel. The game draws A stretched into a 32-bit target, adds B through a lookup table
//            (alpha byte: low 2 bits only) and then uses the target's memory as the 8-bit texture. Because the
//            32-bit and 8-bit layouts of GS memory differ, decoding has to go through the same memory layout.
#include "game_art.h"
#include "settings.h"
#include "iop.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <string>

static bool refpack_decompress(const u8* s, size_t n, std::vector<u8>& out) {
    if (n < 5 || s[1] != 0xfb) return false;
    size_t i = 2; u8 flags = s[0];
    if (flags & 0x01) i += 3;
    int nb = (flags & 0x80) ? 4 : 3; u32 size = 0;
    for (int k = 0; k < nb; k++) size = size << 8 | s[i++];
    out.clear(); out.reserve(size);
    auto lit = [&](size_t l) { if (i + l > n) return false; out.insert(out.end(), s + i, s + i + l); i += l; return true; };
    while (i < n) {
        u8 c0 = s[i]; size_t ln = 0, off = 0;
        if (c0 < 0x80) { if (i + 2 > n) return false; u8 c1 = s[i + 1]; i += 2; if (!lit(c0 & 3)) return false; ln = ((c0 >> 2) & 7) + 3; off = ((c0 & 0x60) << 3) + c1 + 1; }
        else if (c0 < 0xc0) { if (i + 3 > n) return false; u8 c1 = s[i + 1], c2 = s[i + 2]; i += 3; if (!lit(c1 >> 6)) return false; ln = (c0 & 0x3f) + 4; off = ((c1 & 0x3f) << 8) + c2 + 1; }
        else if (c0 < 0xe0) { if (i + 4 > n) return false; u8 c1 = s[i + 1], c2 = s[i + 2], c3 = s[i + 3]; i += 4; if (!lit(c0 & 3)) return false;
                              ln = ((c0 & 0x0c) << 6) + c3 + 5; off = ((c0 & 0x10) << 12) + (c1 << 8) + c2 + 1; }
        else if (c0 < 0xfc) { i += 1; if (!lit(((c0 & 0x1f) + 1) * 4)) return false; continue; }
        else { i += 1; lit(c0 & 3); break; }
        if (off > out.size()) return false;
        for (size_t k = 0; k < ln; k++) out.push_back(out[out.size() - off]);
    }
    return out.size() == size;
}
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 le32(const u8* p) { return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; }

// ---------------------------------------------------------------- GS memory layout (32-bit <-> 8-bit view)
static const u8 kBlk[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const u8 kCol32[8][8] = {
    {0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15},{16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},
    {32,33,36,37,40,41,44,45},{34,35,38,39,42,43,46,47},{48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
static const u8 kCol8[16][16] = {
    {0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54},{8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62},
    {33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23},{41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31},
    {96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86},{104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94},
    {65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119},{73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127},
    {128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182},{136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190},
    {161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151},{169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159},
    {224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214},{232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222},
    {193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247},{201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255}};
static inline u32 addr32(u32 bw, u32 x, u32 y) { return (((y >> 5) * bw + (x >> 6)) * 32 + kBlk[(y >> 3) & 3][(x >> 3) & 7]) * 64 + kCol32[y & 7][x & 7]; }
static inline u32 addr8(u32 bw, u32 x, u32 y) { return (((y >> 6) * ((bw + 1) >> 1) + (x >> 7)) * 32 + kBlk[(y >> 4) & 3][(x >> 4) & 7]) * 256 + kCol8[y & 15][x & 15]; }
// A w32 x h32 image written as 32-bit pixels, read back as an 8-bit image of twice the width and height
static void ct32_to_t8(const u32* words, int w32, int h32, std::vector<u8>& out) {
    const u32 bw = (u32)std::max(1, (w32 + 63) / 64);
    std::vector<u32> mem((size_t)((h32 + 31) / 32 + 1) * bw * 32 * 64, 0);
    for (int y = 0; y < h32; y++) for (int x = 0; x < w32; x++) mem[addr32(bw, x, y)] = words[(size_t)y * w32 + x];
    const u8* b = (const u8*)mem.data();
    out.resize((size_t)w32 * 2 * h32 * 2);
    for (int y = 0; y < h32 * 2; y++) for (int x = 0; x < w32 * 2; x++) out[(size_t)y * w32 * 2 + x] = b[addr8(bw * 2, x, y)];
}
static bool decode14(const u8* data, size_t n, int W, int H, std::vector<u8>& idx) {
    if (W < 16 || H < 8 || (W & 15) || (H & 7)) return false;
    const int aw = W / 8, ah = H / 4, bw = W / 4, bh = H / 4, fw = W / 2, fh = H / 2;
    if (n < (size_t)aw * ah * 4 + (size_t)bw * bh * 4) return false;
    const u8* A = data;
    std::vector<u32> bwords((size_t)bw * bh); std::memcpy(bwords.data(), data + (size_t)aw * ah * 4, bwords.size() * 4);
    std::vector<u8> sel; ct32_to_t8(bwords.data(), bw, bh, sel);                 // fw x fh selector bytes
    std::vector<u32> fb((size_t)fw * fh);
    for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) {
        const u8* a = A + ((size_t)(y / 2) * aw + x / 4) * 4; u8 s = sel[(size_t)y * fw + x];
        u32 r = std::min(255u, (u32)a[0] + (s & 3)), g = std::min(255u, (u32)a[1] + ((s >> 2) & 3)), b = std::min(255u, (u32)a[2] + ((s >> 4) & 3));
        u32 al = (a[3] & 0xfcu) | ((a[3] + (s >> 6)) & 3u);
        fb[(size_t)y * fw + x] = r | g << 8 | b << 16 | al << 24;
    }
    ct32_to_t8(fb.data(), fw, fh, idx);
    return true;
}

// ---------------------------------------------------------------- shapes
static u32 rgba_from_ps2(const u8* e) {            // PS2 alpha: 0x80 = opaque
    u32 a = e[3] >= 0x80 ? 255 : e[3] * 2;
    return a << 24 | (u32)e[2] << 16 | (u32)e[1] << 8 | e[0];              // R,G,B,A in memory order
}
static bool decode_shape(const std::vector<u8>& c, const std::string& shape, ArtImage& im) {
    if (c.size() < 32 || std::memcmp(c.data(), "SHPS", 4) != 0) return false;
    u32 cnt = le32(&c[8]);
    for (u32 i = 0; i < cnt && 24 + (size_t)i * 8 <= c.size(); i++) {
        std::string nm((const char*)&c[16 + i * 8], 4);
        while (!nm.empty() && (nm.back() == ' ' || nm.back() == 0)) nm.pop_back();
        if (!shape.empty() && nm != shape) continue;
        u32 off = le32(&c[20 + i * 8]);
        if ((size_t)off + 16 > c.size()) return false;
        const u8* h = &c[off];
        u32 type = h[0], size = h[1] | h[2] << 8 | h[3] << 16; int w = h[4] | h[5] << 8, hh = h[6] | h[7] << 8;
        if (w <= 0 || hh <= 0 || w > 2048 || hh > 2048) return false;
        const u8* px = h + 16; size_t avail = c.size() - off - 16;
        im.w = w; im.h = hh; im.px.resize((size_t)w * hh);
        if (type == 5) {
            if (avail < (size_t)w * hh * 4) return false;
            for (size_t k = 0; k < (size_t)w * hh; k++) im.px[k] = rgba_from_ps2(px + k * 4);
            return true;
        }
        if (type != 2 && type != 14) return false;
        size_t pal_at = (size_t)off + size;                                   // the palette is the next section
        if (pal_at + 16 + 1024 > c.size()) return false;
        const u8* pal = &c[pal_at + 16];
        u32 lut[256];
        for (int k = 0; k < 256; k++) lut[k] = rgba_from_ps2(pal + (size_t)((k & 0xe7) | ((k & 8) << 1) | ((k & 16) >> 1)) * 4);   // PS2 CLUT order
        std::vector<u8> idx;
        if (type == 2) { if (avail < (size_t)w * hh) return false; idx.assign(px, px + (size_t)w * hh); }
        else if (!decode14(px, avail, w, hh, idx)) return false;
        for (size_t k = 0; k < (size_t)w * hh; k++) im.px[k] = lut[idx[k]];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- archives on the disc
struct BigEntry { u32 off, size; };
struct BigDir { u32 lba = 0, size = 0; std::map<std::string, BigEntry> ents; };
static std::map<std::string, BigDir> s_bigs;
static bool s_opened_here = false;
static bool read_bytes(u32 lba, u32 off, u32 n, std::vector<u8>& out) {
    u32 first = off / 2048, last = (off + n + 2047) / 2048;
    std::vector<u8> buf((size_t)(last - first) * 2048);
    if (!iso_read_sectors(lba + first, last - first, buf.data())) return false;
    out.assign(buf.begin() + (off - first * 2048), buf.begin() + (off - first * 2048) + n);
    return true;
}
static BigDir* big_dir(const std::string& path) {
    auto it = s_bigs.find(path);
    if (it != s_bigs.end()) return it->second.lba ? &it->second : nullptr;
    BigDir& d = s_bigs[path];
    u32 lba, size;
    if (iso_find(path.c_str(), &lba, &size) != 0 || size < 16) return nullptr;
    std::vector<u8> hdr;
    if (!read_bytes(lba, 0, 16, hdr) || std::memcmp(hdr.data(), "BIGF", 4) != 0) return nullptr;
    u32 n = be32(&hdr[8]), hsize = std::min(be32(&hdr[12]) + 64, size);
    if (!read_bytes(lba, 0, hsize, hdr)) return nullptr;
    size_t p = 16;
    for (u32 k = 0; k < n && p + 9 <= hdr.size(); k++) {
        u32 o = be32(&hdr[p]), s = be32(&hdr[p + 4]); p += 8;
        size_t e = p; while (e < hdr.size() && hdr[e]) e++;
        if ((u64)o + s <= size) d.ents[std::string((const char*)&hdr[p], e - p)] = {o, s};
        p = e + 1;
    }
    d.lba = lba; d.size = size;
    return &d;
}

bool game_art_open(const char* iso_path) {
    if (iso_is_open()) return true;
    if (!iso_path || !iso_path[0] || !iso_open(iso_path)) return false;
    s_opened_here = true;
    if (g_settings.uniforms == 1) iso_add_overlay("mods/uniforms-2026");      // the menus show the logos the game will show
    return true;
}
void game_art_close() {
    s_bigs.clear();
    if (s_opened_here) { iso_close(); s_opened_here = false; }
}
bool game_art_load(const char* big, const char* file, const char* shape, ArtImage& out) {
    if (!iso_is_open()) return false;
    BigDir* d = big_dir(big);
    if (!d) return false;
    auto it = d->ents.find(file);
    if (it == d->ents.end() || it->second.size > (32u << 20)) return false;
    std::vector<u8> raw, dec;
    if (!read_bytes(d->lba, it->second.off, it->second.size, raw)) return false;
    const std::vector<u8>* c = &raw;
    if (raw.size() > 2 && raw[1] == 0xfb && (raw[0] & 0x7e) == 0x10) { if (!refpack_decompress(raw.data(), raw.size(), dec)) return false; c = &dec; }
    out.name = file;
    return decode_shape(*c, shape ? shape : "", out);
}
bool game_art_file(const char* path, u64& offset, u32& size) {
    u32 lba;
    if (!iso_is_open() || iso_find(path, &lba, &size) != 0) return false;
    offset = (u64)lba * 2048;
    return true;
}
