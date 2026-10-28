// ISO9660 reader for the user's own disc image. Paths are matched case-insensitively.
// Overlays (mods/<name>/ with the disc's folder layout, e.g. mods/rosters-2026/DATA/DATABASE/DATABASE.BIG)
// replace files without touching the ISO: the replacement is served from sectors past the end of the disc
// and the file's directory record is patched on the fly, because the game parses the ISO directory itself.
#include "iop.h"
#include "ee_runtime.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <mutex>

static FILE* s_iso = nullptr;
struct IsoEntry { u32 lba, size; u32 rec_lba = 0, rec_off = 0; u32 orig_lba = 0, orig_size = 0; };
static std::map<std::string, IsoEntry> s_files;   // "DATA/DATABASE/HIST.DAT" -> entry

static std::string norm(const char* p) {
    std::string s(p);
    size_t c = s.find(':'); if (c != std::string::npos) s = s.substr(c + 1);
    size_t v = s.find(';'); if (v != std::string::npos) s = s.substr(0, v);
    std::string o;
    for (char ch : s) { if (ch == '\\') ch = '/'; o += (char)std::toupper((unsigned char)ch); }
    while (!o.empty() && o[0] == '/') o.erase(0, 1);
    // collapse "//"
    std::string r; for (char ch : o) { if (ch == '/' && !r.empty() && r.back() == '/') continue; r += ch; }
    return r;
}

// A replacement is a virtual file made of segments, each from memory, from a host file (read when needed) or
// from sectors of the image itself (the original file, or an earlier replacement of it).
struct Seg { u64 voff = 0, len = 0; int kind = 0; std::vector<u8> mem; std::string host; u64 src = 0; };   // kind 0 mem, 1 host file (src = offset), 2 image bytes (src = byte offset)
struct Overlay { u32 lba, size; std::vector<Seg> segs; u32 rec_lba, rec_off; std::string name; };
static std::vector<Overlay> s_over;
static u32 s_over_next = 0;          // first free virtual sector
static bool read_raw(u32 lba, u32 count, u8* dst);
#include <mutex>
static bool read_host(const std::string& path, u64 off, u8* dst, size_t n) {
    static std::mutex mx; std::lock_guard<std::mutex> lk(mx);      // the game's reader and the menu's art loader
    static std::string open_path; static FILE* f = nullptr;
    if (open_path != path) { if (f) fclose(f); f = fopen(path.c_str(), "rb"); open_path = f ? path : std::string(); }
    if (!f) return false;
#if defined(_WIN32)
    _fseeki64(f, (long long)off, SEEK_SET);
#else
    fseeko(f, (off_t)off, SEEK_SET);
#endif
    return fread(dst, 1, n, f) == n;
}
static bool read_bytes_at(u64 off, u8* dst, size_t n);
static std::recursive_mutex s_seg_mx;                       // segments that change while the game runs (iso_big_switch)
static bool overlay_sector(const Overlay& o, u32 L, u8* d) {
    std::lock_guard<std::recursive_mutex> lk(s_seg_mx);
    const u64 v0 = (u64)(L - o.lba) * 2048, v1 = v0 + 2048;
    std::memset(d, 0, 2048);
    for (const Seg& g : o.segs) {
        u64 a = std::max(v0, g.voff), b = std::min(v1, g.voff + g.len);
        if (a >= b) continue;
        u8* to = d + (a - v0); const u64 so = a - g.voff; const size_t n = (size_t)(b - a);
        if (g.kind == 0) std::memcpy(to, g.mem.data() + so, n);
        else if (g.kind == 1) { if (!read_host(g.host, g.src + so, to, n)) return false; }
        else if (!read_bytes_at(g.src + so, to, n)) return false;
    }
    return true;
}
bool iso_read_sectors(u32 lba, u32 count, u8* dst) {
    if (!s_iso) return false;
    if (s_over.empty()) return read_raw(lba, count, dst);
    for (u32 k = 0; k < count; k++) {            // sector by sector: virtual sectors, patched directory sectors
        u32 L = lba + k; u8* d = dst + (size_t)k * 2048;
        bool done = false;
        for (const Overlay& o : s_over) {
            u32 n = (o.size + 2047) / 2048;
            if (L >= o.lba && L < o.lba + n) {
                static const bool log = std::getenv("MVP_OVLLOG") != nullptr;          // which replaced files the game reads, and where
                if (log && k == 0) rlog("ovl", "%s: bytes %u.. (%u sectors)", o.name.c_str(), (L - o.lba) * 2048, count);
                if (!overlay_sector(o, L, d)) return false; done = true; break;
            }
        }
        if (done) continue;
        if (!read_raw(L, 1, d)) return false;
        for (const Overlay& o : s_over) if (o.rec_lba == L) {
            u8* r = d + o.rec_off;
            auto both = [&](int at, u32 v) { std::memcpy(r + at, &v, 4); r[at + 4] = v >> 24; r[at + 5] = v >> 16; r[at + 6] = v >> 8; r[at + 7] = v; };
            both(2, o.lba); both(10, o.size);
        }
    }
    return true;
}
static bool read_bytes_at(u64 off, u8* dst, size_t n) {          // bytes of the image (real or virtual sectors)
    u8 sec[2048];
    while (n) {
        u32 L = (u32)(off / 2048); size_t in = (size_t)(off % 2048), c = std::min(n, (size_t)2048 - in);
        if (!iso_read_sectors(L, 1, sec)) return false;
        std::memcpy(dst, sec + in, c); dst += c; off += c; n -= c;
    }
    return true;
}
#include <mutex>
static std::mutex s_read_mx;              // the game's reader thread and the menu's art loader share the file
static bool read_raw(u32 lba, u32 count, u8* dst) {
    std::lock_guard<std::mutex> lk(s_read_mx);
#if defined(_WIN32)
    _fseeki64(s_iso, (long long)lba * 2048, SEEK_SET);
#else
    fseeko(s_iso, (off_t)lba * 2048, SEEK_SET);
#endif
    return fread(dst, 2048, count, s_iso) == count;
}

static void walk(u32 lba, u32 size, const std::string& path) {
    std::vector<u8> d((size + 2047) & ~2047u);
    if (!iso_read_sectors(lba, (u32)d.size() / 2048, d.data())) return;
    for (size_t i = 0; i < d.size();) {
        u8 len = d[i];
        if (len == 0) { i = (i / 2048 + 1) * 2048; continue; }
        u32 elba, esz; std::memcpy(&elba, &d[i + 2], 4); std::memcpy(&esz, &d[i + 10], 4);
        u8 flags = d[i + 25], nl = d[i + 32];
        std::string name((const char*)&d[i + 33], nl);
        if (!(nl == 1 && (name[0] == 0 || name[0] == 1))) {
            size_t sc = name.find(';'); if (sc != std::string::npos) name = name.substr(0, sc);
            std::string full = path.empty() ? name : path + "/" + name;
            if (flags & 2) walk(elba, esz, full);
            else s_files[norm(full.c_str())] = {elba, esz, lba + (u32)(i / 2048), (u32)(i % 2048), elba, esz};
        }
        i += len;
    }
}

bool iso_open(const char* path) {
    s_iso = fopen(path, "rb");
    if (!s_iso) return false;
    u8 pvd[2048];
    if (!iso_read_sectors(16, 1, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) { fclose(s_iso); s_iso = nullptr; return false; }
    u32 rl, rs; std::memcpy(&rl, pvd + 156 + 2, 4); std::memcpy(&rs, pvd + 156 + 10, 4);
    walk(rl, rs, "");
    rlog("iso", "opened %s: %zu files", path, s_files.size());
#if defined(_WIN32)
    _fseeki64(s_iso, 0, SEEK_END); long long end = _ftelli64(s_iso);
#else
    fseeko(s_iso, 0, SEEK_END); long long end = (long long)ftello(s_iso);
#endif
    s_over_next = (u32)((end + 2047) / 2048) + 64;                        // just past the end of the image
    return true;
}

static std::map<std::string, IsoEntry>::iterator place_overlay(std::map<std::string, IsoEntry>::iterator f, Overlay&& o) {
    for (Overlay& old : s_over) if (old.name == f->first) { old.rec_lba = 0; }   // later replacements win
    o.lba = s_over_next; s_over_next += (o.size + 2047) / 2048 + 16;
    o.rec_lba = f->second.rec_lba; o.rec_off = f->second.rec_off; o.name = f->first;
    f->second.lba = o.lba; f->second.size = o.size;
    s_over.push_back(std::move(o));
    return f;
}
static u32 be32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
// Replaces members of an EA "BIGF" archive on the disc without copying the archive: the directory at its start
// is rewritten to point at the new members, which are appended after the original data. Files that are not
// members yet are added: the directory grows into the data that followed it, and the members stored there are
// moved to the end too (the game looks members up by name; the directory is not sorted).
static int overlay_big_members(std::map<std::string, IsoEntry>::iterator f, const std::vector<std::pair<std::string, std::string>>& members, const char* dir) {
    const u64 base = (u64)f->second.lba * 2048; const u32 old_size = f->second.size;
    u8 h[16];
    if (old_size < 16 || !read_bytes_at(base, h, 16) || (std::memcmp(h, "BIGF", 4) && std::memcmp(h, "BIG4", 4))) { rlog("iso", "overlay %s: not a BIG archive, members ignored", f->first.c_str()); return 0; }
    const u32 count = be32(h + 8), hdr = be32(h + 12);
    if (hdr < 16 || hdr > old_size || hdr > (8u << 20) || count > 100000) return 0;
    std::vector<u8> mem(hdr);
    if (!read_bytes_at(base, mem.data(), hdr)) return 0;
    std::map<std::string, size_t> at;                                   // member name (lower case) -> offset of its entry
    size_t pend = 16;                                                   // end of the last entry
    for (size_t p = 16, i = 0; i < count && p + 9 <= hdr; i++) {
        size_t e = p + 8; while (e < hdr && mem[e]) e++;
        std::string nm((const char*)&mem[p + 8], e - p - 8); for (char& c : nm) { c = (char)std::tolower((unsigned char)c); if (c == '\\') c = '/'; }
        if (!at.count(nm)) at[nm] = p;
        p = e + 1; pend = p;
    }
    // new members: their entries go after the last one (the bytes that followed the entries stay after them)
    std::vector<std::pair<std::string, std::string>> fresh;
    for (const auto& m : members) {
        std::string nm = m.first; for (char& c : nm) c = (char)std::tolower((unsigned char)c);
        if (!at.count(nm) && nm.size() < 200) fresh.push_back({nm, m.second});
    }
    u32 new_hdr = hdr;
    if (!fresh.empty()) {
        std::vector<u8> ins;
        for (auto& fm : fresh) {
            size_t p = pend + ins.size(); at[fm.first] = p;
            for (int k = 0; k < 8; k++) ins.push_back(0);
            ins.insert(ins.end(), fm.first.begin(), fm.first.end()); ins.push_back(0);
        }
        mem.insert(mem.begin() + (long)pend, ins.begin(), ins.end());
        new_hdr = (u32)mem.size();
        put_be32(&mem[8], count + (u32)fresh.size()); put_be32(&mem[12], new_hdr);
    }
    Overlay o; u64 end = std::max<u64>(old_size, new_hdr); int n = 0;
    std::vector<Seg> added;
    auto place = [&](size_t entry, u64 sz) -> u64 {
        end = (end + 63) & ~63ull; u64 off = end;
        put_be32(&mem[entry], (u32)off); put_be32(&mem[entry + 4], (u32)sz); end += sz; return off;
    };
    std::map<size_t, bool> replaced;
    for (const auto& m : members) {
        std::string nm = m.first; for (char& c : nm) c = (char)std::tolower((unsigned char)c);
        auto it = at.find(nm);
        if (it == at.end()) continue;
        std::error_code ec; u64 sz = (u64)std::filesystem::file_size(m.second, ec); if (ec || sz == 0 || sz > 0x7fffffffull) continue;
        u64 off = place(it->second, sz); replaced[it->second] = true;
        Seg g; g.kind = 1; g.voff = off; g.len = sz; g.host = m.second; g.src = 0; added.push_back(std::move(g));
        n++;
    }
    if (!n) return 0;
    if (new_hdr > hdr) {                                                // members the grown directory now covers move to the end
        for (auto& [nm, p] : at) {
            if (replaced.count(p)) continue;
            u32 off = be32(&mem[p]), sz = be32(&mem[p + 4]);
            if (off >= new_hdr || (u64)off + sz > old_size) continue;
            u64 to = place(p, sz); replaced[p] = true;
            Seg g; g.kind = 2; g.voff = to; g.len = sz; g.src = base + off; added.push_back(std::move(g));
        }
    }
    mem[4] = (u8)end; mem[5] = (u8)(end >> 8); mem[6] = (u8)(end >> 16); mem[7] = (u8)(end >> 24);   // total size (little-endian)
    Seg head; head.kind = 0; head.voff = 0; head.len = mem.size(); head.mem = std::move(mem);
    o.size = (u32)end; o.segs.push_back(std::move(head));
    if (new_hdr < old_size) { Seg rest; rest.kind = 2; rest.voff = new_hdr; rest.len = old_size - new_hdr; rest.src = base + new_hdr; o.segs.push_back(std::move(rest)); }
    for (Seg& g : added) o.segs.push_back(std::move(g));
    place_overlay(f, std::move(o));
    fprintf(stderr, "overlay: %s: %d members (%zu new) from %s\n", f->first.c_str(), n, fresh.size(), dir);
    return n;
}
// Adds every file under dir (mirroring the disc layout) as a replacement. A folder named like an archive on the
// disc (mods/x/DATA/MODELS.BIG/u025a.ssh) replaces single members of that archive. Returns the number of files.
int iso_add_overlay(const char* dir) {
    namespace fs = std::filesystem;
    std::error_code ec; int n = 0;
    if (!fs::is_directory(dir, ec)) return 0;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> members;       // archive -> (member, host file)
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        auto f = s_files.find(norm(rel.c_str()));
        if (f == s_files.end()) {
            std::string parent = fs::path(rel).parent_path().generic_string();
            if (!parent.empty() && s_files.count(norm(parent.c_str()))) { members[norm(parent.c_str())].push_back({it->path().filename().string(), it->path().string()}); continue; }
            if (rel.find('/') != std::string::npos && rel.find("DATA/") == 0) rlog("iso", "overlay %s: no such file on the disc, ignored", rel.c_str());
            continue;
        }
        u64 sz = (u64)fs::file_size(it->path(), ec); if (ec || sz == 0 || sz > 0x7fffffffull) continue;
        Overlay o; o.size = (u32)sz;
        Seg g; g.kind = 1; g.voff = 0; g.len = sz; g.host = it->path().string(); o.segs.push_back(std::move(g));
        place_overlay(f, std::move(o)); n++;
        fprintf(stderr, "overlay: %s (%llu bytes) from %s\n", rel.c_str(), (unsigned long long)sz, dir);
    }
    for (auto& m : members) { std::sort(m.second.begin(), m.second.end()); n += overlay_big_members(s_files.find(m.first), m.second, dir); }
    return n;
}
// Members of a BIG archive whose contents can change while the game runs (an era pack's uniforms follow the
// dynasty's season). Each gets a slot as large as its largest candidate (any member of the archive as it is on the
// disc); the directory never changes, only which disc member fills the slot (the rest of the slot reads as zeros).
struct BigCand { u64 src; u32 size; std::string host; };              // a disc member (image bytes) or a host file
struct BigSwitch { size_t ovl, seg; std::map<std::string, BigCand> cand; std::string cur; };
static std::map<std::string, BigSwitch> s_switch;          // "models.big/u005a.ssh" -> slot
static std::map<std::string, std::pair<u32, u32>> parse_big_dir(u64 base, u32 size, std::vector<u8>* mem_out, std::map<std::string, size_t>* at_out) {
    std::map<std::string, std::pair<u32, u32>> out; u8 h[16];
    if (size < 16 || !read_bytes_at(base, h, 16) || (std::memcmp(h, "BIGF", 4) && std::memcmp(h, "BIG4", 4))) return out;
    const u32 count = be32(h + 8), hdr = be32(h + 12);
    if (hdr < 16 || hdr > size || hdr > (8u << 20) || count > 100000) return out;
    std::vector<u8> mem(hdr); if (!read_bytes_at(base, mem.data(), hdr)) return out;
    for (size_t p = 16, i = 0; i < count && p + 9 <= hdr; i++) {
        size_t e = p + 8; while (e < hdr && mem[e]) e++;
        std::string nm((const char*)&mem[p + 8], e - p - 8); for (char& c : nm) c = (char)std::tolower((unsigned char)c);
        if (!out.count(nm)) { out[nm] = {be32(&mem[p]), be32(&mem[p + 4])}; if (at_out) (*at_out)[nm] = p; }
        p = e + 1;
    }
    if (mem_out) mem_out->swap(mem);
    return out;
}
// slots: member -> candidate members (all looked up on the disc's own archive). Returns the number of slots made.
int iso_big_switchable(const char* archive, const std::map<std::string, std::vector<std::string>>& slots) {
    auto f = s_files.find(norm(archive));
    if (f == s_files.end() || slots.empty()) return 0;
    const u64 base = (u64)f->second.lba * 2048, obase = (u64)f->second.orig_lba * 2048; const u32 size = f->second.size;
    std::vector<u8> mem; std::map<std::string, size_t> at;
    auto cur = parse_big_dir(base, size, &mem, &at);
    auto orig = parse_big_dir(obase, f->second.orig_size, nullptr, nullptr);
    if (cur.empty() || orig.empty()) return 0;
    const u32 hdr = (u32)mem.size();
    u64 end = std::max<u64>(size, hdr);
    Overlay o; std::vector<std::pair<std::string, BigSwitch>> made;
    for (const auto& [member, cands] : slots) {
        std::string m = member; for (char& c : m) c = (char)std::tolower((unsigned char)c);
        auto it = cur.find(m); if (it == cur.end()) continue;
        BigSwitch sw; u32 mx = it->second.second;
        sw.cand[""] = {base + it->second.first, it->second.second, ""};      // "": as the archive had it (pack overlay included)
        for (const std::string& cn : cands) {
            if (!cn.empty() && cn[0] == '@') {                             // "@path": a file of ours
                std::error_code ec; const u64 sz = (u64)std::filesystem::file_size(cn.substr(1), ec);
                if (ec || !sz || sz > (16u << 20)) continue;
                sw.cand[cn] = {0, (u32)sz, cn.substr(1)}; mx = std::max(mx, (u32)sz); continue;
            }
            std::string n = cn; for (char& c : n) c = (char)std::tolower((unsigned char)c);
            auto ot = orig.find(n); if (ot == orig.end()) continue;
            sw.cand[n] = {obase + ot->second.first, ot->second.second, ""}; mx = std::max(mx, ot->second.second);
        }
        if (sw.cand.size() < 2) continue;
        end = (end + 63) & ~63ull; const u64 off = end; end += mx;
        put_be32(&mem[at[m]], (u32)off); put_be32(&mem[at[m] + 4], mx);
        Seg g; g.kind = 2; g.voff = off; g.len = it->second.second; g.src = base + it->second.first;
        sw.seg = 2 + made.size(); made.push_back({f->first + "/" + m, std::move(sw)}); o.segs.push_back(std::move(g));
    }
    if (made.empty()) return 0;
    mem[4] = (u8)end; mem[5] = (u8)(end >> 8); mem[6] = (u8)(end >> 16); mem[7] = (u8)(end >> 24);
    Seg head; head.kind = 0; head.voff = 0; head.len = mem.size(); head.mem = std::move(mem);
    Seg rest; rest.kind = 2; rest.voff = hdr; rest.len = size > hdr ? size - hdr : 0; rest.src = base + hdr;
    o.segs.insert(o.segs.begin(), std::move(rest)); o.segs.insert(o.segs.begin(), std::move(head));
    o.size = (u32)end;
    const size_t idx = s_over.size();
    place_overlay(f, std::move(o));
    for (auto& [k, sw] : made) { sw.ovl = idx; s_switch[k] = std::move(sw); }
    fprintf(stderr, "overlay: %s: %zu members follow the season\n", f->first.c_str(), made.size());
    return (int)made.size();
}
// Fills the slot of archive/member with a candidate ("" = as the archive had it). False if nothing changed.
bool iso_big_switch(const char* archive, const char* member, const char* cand) {
    std::string k = norm(archive) + "/" + member; size_t sl = k.find('/' , norm(archive).size());
    for (size_t i = sl; i < k.size(); i++) k[i] = (char)std::tolower((unsigned char)k[i]);
    auto it = s_switch.find(k); if (it == s_switch.end()) return false;
    std::string c = cand; if (c.empty() || c[0] != '@') for (char& ch : c) ch = (char)std::tolower((unsigned char)ch);
    auto ct = it->second.cand.find(c); if (ct == it->second.cand.end() || it->second.cur == c) return false;
    Seg& g = s_over[it->second.ovl].segs[it->second.seg];
    std::lock_guard<std::recursive_mutex> lk(s_seg_mx);
    if (ct->second.host.empty()) { g.kind = 2; g.src = ct->second.src; g.host.clear(); }
    else { g.kind = 1; g.src = 0; g.host = ct->second.host; }
    g.len = ct->second.size; it->second.cur = c;
    return true;
}
// Whole files whose contents can change while the game runs (an era pack's ballparks follow the dynasty's season):
// the file becomes as large as its largest candidate (other files of the disc, as the disc has them, or "=" for its
// own original); only which one fills it changes (zeros after it). The directory record keeps that size.
struct FileSwitch { size_t ovl; std::map<std::string, std::pair<u64, u32>> cand; std::string cur; };
static std::map<std::string, FileSwitch> s_fswitch;
std::vector<std::string> iso_list(const char* prefix) {             // disc paths starting with prefix, sorted
    std::vector<std::string> out; const std::string p = norm(prefix);
    for (auto it = s_files.lower_bound(p); it != s_files.end() && it->first.compare(0, p.size(), p) == 0; ++it) out.push_back(it->first);
    return out;
}
bool iso_file_switchable(const char* path, const std::vector<std::string>& cands) {
    auto f = s_files.find(norm(path));
    if (f == s_files.end()) return false;
    FileSwitch sw; u32 mx = f->second.size;
    sw.cand[""] = {(u64)f->second.lba * 2048, f->second.size};
    sw.cand["="] = {(u64)f->second.orig_lba * 2048, f->second.orig_size}; mx = std::max(mx, f->second.orig_size);
    for (const std::string& c : cands) {
        auto g = s_files.find(norm(c.c_str())); if (g == s_files.end()) continue;
        sw.cand[g->first] = {(u64)g->second.orig_lba * 2048, g->second.orig_size}; mx = std::max(mx, g->second.orig_size);
    }
    Overlay o; o.size = mx;
    Seg g; g.kind = 2; g.voff = 0; g.len = f->second.size; g.src = (u64)f->second.lba * 2048; o.segs.push_back(std::move(g));
    sw.ovl = s_over.size();
    place_overlay(f, std::move(o));
    s_fswitch[f->first] = std::move(sw);
    return true;
}
bool iso_file_switch(const char* path, const char* cand) {
    auto it = s_fswitch.find(norm(path)); if (it == s_fswitch.end()) return false;
    std::string c = cand[0] && cand[0] != '=' ? norm(cand) : std::string(cand);
    auto ct = it->second.cand.find(c); if (ct == it->second.cand.end() || it->second.cur == c) return false;
    Seg& g = s_over[it->second.ovl].segs[0];
    std::lock_guard<std::recursive_mutex> lk(s_seg_mx);
    g.src = ct->second.first; g.len = ct->second.second; it->second.cur = c;
    return true;
}
// One replacement from memory (custom music, patched string tables). path = the disc path.
bool iso_add_overlay_data(const char* path, std::vector<u8>&& data, const char* from) {
    auto f = s_files.find(norm(path));
    if (f == s_files.end() || data.empty()) return false;
    Overlay o; o.size = (u32)data.size();
    Seg g; g.kind = 0; g.voff = 0; g.len = data.size(); g.mem = std::move(data); o.segs.push_back(std::move(g));
    fprintf(stderr, "overlay: %s (%u bytes) from %s\n", path, o.size, from ? from : "memory");
    place_overlay(f, std::move(o));
    return true;
}
// The file as the game will see it (the disc's, or the overlay that replaces it).
bool iso_file_bytes(const char* path, std::vector<u8>& out) {
    u32 lba, size;
    if (iso_find(path, &lba, &size) < 0) return false;
    std::vector<u8> buf(((size_t)size + 2047) & ~(size_t)2047);
    if (!buf.empty() && !iso_read_sectors(lba, (u32)(buf.size() / 2048), buf.data())) return false;
    buf.resize(size); out.swap(buf);
    return true;
}
bool iso_is_open() { return s_iso != nullptr; }

int iso_find(const char* path, u32* lba, u32* size) {
    auto it = s_files.find(norm(path));
    if (it == s_files.end()) return -1;
    *lba = it->second.lba; *size = it->second.size;
    return 0;
}

void iso_close() {
    if (s_iso) fclose(s_iso);
    s_iso = nullptr; s_files.clear(); s_over.clear(); s_over_next = 0;
}

// Launcher: checks that path is the user's MVP Baseball 2005 disc (USA, SLUS-21135) and, if elf_out is set,
// copies the game program out of it. Returns "" when the disc is good, otherwise a message for the user.
static u32 crc32_buf(const u8* p, size_t n) {
    static u32 t[256]; static bool init = false;
    if (!init) { for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
    u32 c = 0xffffffffu; for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8); return c ^ 0xffffffffu;
}
// The one disc the recompiled code was made from: MVP Baseball 2005, USA (NTSC-U), SLUS-21135, program SLUS_211.35
static const u32 kElfSize = 8130840, kElfCrc = 0xeda6ec50;
std::string iso_check_game(const char* path, const char* elf_out) {
    if (s_iso) return "internal error: a disc is already open";
    {   // formats people often have instead of a plain .iso
        std::string p = path; std::string ext; size_t d = p.find_last_of('.'); if (d != std::string::npos) ext = p.substr(d + 1);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == "chd") return "This is a CHD (compressed) image. Convert it to .iso first: chdman extractdvd -i game.chd -o game.iso (chdman comes with MAME).";
        if (ext == "cso" || ext == "zso") return "This is a compressed CSO/ZSO image. Decompress it to .iso first (for example with maxcso --decompress).";
        if (ext == "zip" || ext == "7z" || ext == "rar" || ext == "gz") return "This is an archive. Unpack it first and choose the .iso inside.";
        if (ext == "bin" || ext == "cue" || ext == "img" || ext == "mdf" || ext == "nrg") { /* may still be a plain ISO 9660 image: try it */ }
    }
    if (!iso_open(path)) return "This file is not a DVD image (ISO 9660). Make an .iso from your disc; .bin/.cue, .mdf or .nrg rips have to be converted to .iso first.";
    std::string err;
    u32 lba = 0, size = 0;
    if (iso_find("SYSTEM.CNF", &lba, &size) < 0) err = "This disc image has no SYSTEM.CNF, so it isn't a PS2 game disc.";
    else {
        std::vector<u8> cnf(((size + 2047) & ~2047u) + 1, 0);
        iso_read_sectors(lba, (size + 2047) / 2048, cnf.data()); cnf[size] = 0;
        std::string c((const char*)cnf.data());
        if (c.find("SLUS_211.35") == std::string::npos) {
            size_t b = c.find("cdrom0:"); std::string id = b == std::string::npos ? "unknown" : c.substr(b + 8, 11);
            if (id.rfind("SLES", 0) == 0 || id.rfind("SCES", 0) == 0) err = "This is a European (PAL) disc (" + id + "). Only the USA disc of MVP Baseball 2005 (SLUS-21135) works.";
            else if (id.rfind("SLPM", 0) == 0 || id.rfind("SLPS", 0) == 0 || id.rfind("SLKA", 0) == 0) err = "This is an Asian (NTSC-J) disc (" + id + "). Only the USA disc of MVP Baseball 2005 (SLUS-21135) works.";
            else err = "This is a different PS2 game (" + id + "). Only MVP Baseball 2005, USA version (SLUS-21135) works.";
        }
    }
    if (err.empty() && iso_find("DATA/DATABASE/DATABASE.BIG", &lba, &size) < 0) err = "The game data is missing from this disc image (incomplete rip?).";
    if (err.empty() && iso_find("SLUS_211.35", &lba, &size) == 0) {
        std::vector<u8> elf(((size + 2047) & ~2047u));
        if (!iso_read_sectors(lba, (u32)elf.size() / 2048, elf.data()) || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
            err = "Could not read the game program from the disc image (damaged rip?).";
        else if (size != kElfSize || crc32_buf(elf.data(), size) != kElfCrc)
            err = "This is MVP Baseball 2005 (USA), but its game program differs from the version the recomp was made from (a different printing, a modified or a damaged image). Make a fresh 1:1 .iso of your disc.";
        else {
            if (elf_out) {
            std::string dir = elf_out; size_t sl = dir.find_last_of("/\\");
            if (sl != std::string::npos) { std::error_code ec; std::filesystem::create_directories(dir.substr(0, sl), ec); }
            FILE* f = fopen(elf_out, "wb");
            if (!f || fwrite(elf.data(), 1, size, f) != size) err = std::string("Could not write ") + elf_out + " (is the folder read-only?)";
            if (f) fclose(f);
            }
        }
    } else if (err.empty()) err = "The game program (SLUS_211.35) is missing from this disc image (incomplete rip?).";
    iso_close();
    return err;
}
