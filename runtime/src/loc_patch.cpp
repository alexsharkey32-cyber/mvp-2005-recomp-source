#include "loc_patch.h"
#include "iop.h"
#include "ee_runtime.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <algorithm>

static u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put32(std::vector<u8>& v, u32 x) { v.push_back((u8)x); v.push_back((u8)(x >> 8)); v.push_back((u8)(x >> 16)); v.push_back((u8)(x >> 24)); }

bool LocFile::parse(const std::vector<u8>& d) {
    if (d.size() < 32 || std::memcmp(d.data(), "LOCH", 4)) return false;
    const u32 l = rd32(&d[16]);
    if ((size_t)l + 16 > d.size() || std::memcmp(&d[l], "LOCL", 4)) return false;
    const u32 size = rd32(&d[l + 4]), n = rd32(&d[l + 12]);
    zero_ = rd32(&d[l + 8]);
    if ((size_t)l + size > d.size() || n > 100000 || (size_t)l + 16 + 4ull * n > d.size()) return false;
    str.assign(n, std::u16string());
    for (u32 i = 0; i < n; i++) {
        size_t p = (size_t)l + rd32(&d[l + 16 + 4 * i]);
        while (p + 1 < d.size()) { char16_t ch = (char16_t)(d[p] | d[p + 1] << 8); if (!ch) break; str[i] += ch; p += 2; }
        // room left after a string by an earlier patch (NULs up to the next string, beyond the 2-character end) is kept
        const size_t next = i + 1 < n ? (size_t)l + rd32(&d[l + 16 + 4 * (i + 1)]) : (size_t)l + size;
        if (next > p && next <= d.size() && (next - p) / 2 > 2) {
            bool zeros = true; for (size_t q = p; q < next; q++) if (d[q]) { zeros = false; break; }
            if (zeros) str[i].append((next - p) / 2 - 2, u'\0');
        }
    }
    head_.assign(d.begin(), d.begin() + l); tail_.assign(d.begin() + l + size, d.end());
    return true;
}
std::vector<u8> LocFile::build() const {
    const u32 n = (u32)str.size(), base = 16 + 4 * n;
    std::vector<u8> body, offs;
    for (u32 i = 0; i < n; i++) {
        put32(offs, base + (u32)body.size());
        for (char16_t ch : str[i]) { body.push_back((u8)ch); body.push_back((u8)(ch >> 8)); }
        body.insert(body.end(), 4, 0);
    }
    std::vector<u8> locl = {'L', 'O', 'C', 'L'}; put32(locl, 0); put32(locl, zero_); put32(locl, n);
    locl.insert(locl.end(), offs.begin(), offs.end()); locl.insert(locl.end(), body.begin(), body.end());
    while (locl.size() & 3) locl.push_back(0);
    const u32 ls = (u32)locl.size(); locl[4] = (u8)ls; locl[5] = (u8)(ls >> 8); locl[6] = (u8)(ls >> 16); locl[7] = (u8)(ls >> 24);
    std::vector<u8> out(head_);
    out.insert(out.end(), locl.begin(), locl.end()); out.insert(out.end(), tail_.begin(), tail_.end());
    return out;
}
std::u16string loc_utf16(const std::string& s, size_t max_chars, bool latin1_only) {
    std::u16string o;
    for (size_t i = 0; i < s.size() && o.size() < max_chars;) {
        u8 c = (u8)s[i]; u32 cp = '?'; int n = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) { cp = (c & 0x1f) << 6 | ((u8)s[i + 1] & 0x3f); n = 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < s.size()) { cp = (c & 0x0f) << 12 | ((u8)s[i + 1] & 0x3f) << 6 | ((u8)s[i + 2] & 0x3f); n = 3; }
        else if ((c & 0xf8) == 0xf0) n = 4;
        i += n;
        if (cp < 0x20 || cp > 0xffff) continue;
        o += (char16_t)(latin1_only && cp > 0xff ? '?' : cp);
    }
    return o;
}
int loc_apply_strings(const char* dir) {
    std::string path = std::string(dir) + "/strings.txt";
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return 0;
    std::vector<std::pair<std::u16string, std::u16string>> subst; char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::string l(line); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t tab = l.find('\t');
        if (l.empty() || l[0] == '#' || tab == std::string::npos || tab == 0) continue;
        subst.push_back({loc_utf16(l.substr(0, tab)), loc_utf16(l.substr(tab + 1))});
    }
    fclose(f);
    if (subst.empty()) return 0;
    int changed = 0;
    for (const char* file : {"DATA/FEENG.LOC", "DATA/IGENG.LOC"}) {
        std::vector<u8> d; LocFile loc;
        if (!iso_file_bytes(file, d) || !loc.parse(d)) { rlog("iso", "%s: string table not recognised, %s not applied", file, path.c_str()); continue; }
        int n = 0;
        for (std::u16string& s : loc.str)
            for (const auto& kv : subst) {
                bool hit = false;
                for (size_t p = s.find(kv.first); p != std::u16string::npos; p = s.find(kv.first, p + kv.second.size())) { s.replace(p, kv.first.size(), kv.second); hit = true; }
                if (hit) n++;
            }
        if (n && iso_add_overlay_data(file, loc.build(), path.c_str())) changed += n;
    }
    return changed;
}

// ---------------------------------------------------------------- club names in the menus' string tables
// The menus name a club from the database's city and the string tables' nickname (FEENG.LOC 384.., IGENG.LOC
// 137..: "Orioles", "Red Sox" ... then the full names "Baltimore Orioles" ...), in the game's division order. An era
// pack's dynasty renames clubs while it runs (hle.cpp era_teams_apply), so at boot every one of those strings gets
// room to grow (NULs after the name, inside the string) and later the copies the game loaded into its memory are
// rewritten in place.
static const int kLocArt[30] = {10, 11, 12, 13, 14, 5, 6, 7, 8, 9, 1, 2, 3, 4, 26, 27, 28, 29, 30, 20, 21, 22, 23, 24, 25, 15, 16, 17, 18, 19};
static const size_t kNickCap = 20, kFullCap = 32;
static std::u16string trimmed(std::u16string s) { while (!s.empty() && s.back() == 0) s.pop_back(); return s; }
struct LocWhere { u32 count; int at; };
// The string tables the game loaded: found by their "LOCL" block and string count; remembered, checked each time,
// looked for again (at most every few seconds) when one has gone or none was found yet.
static std::vector<u32> s_counts;                                    // string counts of the tables we patched
static std::vector<u32> s_tables;                                    // their LOCL blocks in the guest's memory
static u64 s_last_scan = 0;
static u32 lr32(const u8* p) { return (u32)p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static bool table_ok(u32 a) {
    extern u8* g_ram; if (a < 0x100000 || a + 16 >= 0x2000000) return false; const u8* p = g_ram + a;
    if (p[0] != 'L' || p[1] != 'O' || p[2] != 'C' || p[3] != 'L') return false;
    const u32 size = lr32(p + 4), cnt = lr32(p + 12);
    if (a + size > 0x2000000 || 16 + 4ull * cnt > size) return false;
    for (u32 c : s_counts) if (c == cnt) return true;
    return false;
}
static bool tables_changed() {                                         // true when the set of tables is new
    bool all = !s_tables.empty();
    for (u32 a : s_tables) if (!table_ok(a)) all = false;
    if (all && s_tables.size() >= s_counts.size()) return false;
    extern u64 host_time_ns();
    const u64 now = host_time_ns(); if (now - s_last_scan < 3000000000ull && s_last_scan) return false;
    s_last_scan = now;
    std::vector<u32> found;
    for (u32 a = 0x100000; a + 16 < 0x2000000; a += 4) if (table_ok(a)) found.push_back(a);
    if (found == s_tables) return false;
    s_tables = found; return !found.empty();
}
static std::vector<LocWhere> s_loc_where;                   // per table: its string count, where the club names start
static std::u16string padded(const std::string& s, size_t cap) { std::u16string u = loc_utf16(s, cap - 1); u.resize(cap, 0); return u; }
int loc_set_club_names(const std::string* nick, const std::string* full) {   // indexed by art id 1..30 ("" = keep)
    int changed = 0;
    for (const char* file : {"DATA/FEENG.LOC", "DATA/IGENG.LOC"}) {
        std::vector<u8> d; LocFile loc;
        if (!iso_file_bytes(file, d) || !loc.parse(d)) continue;
        int at = -1;                                                        // where "Orioles" .. "Giants" start
        for (size_t i = 0; i + 62 <= loc.str.size() && at < 0; i++)
            if (trimmed(loc.str[i]) == u"Orioles" && trimmed(loc.str[i + 1]) == u"Red Sox" && trimmed(loc.str[i + 32]) == u"Baltimore Orioles") at = (int)i;
        if (at < 0) { rlog("iso", "%s: club names not found", file); continue; }
        s_loc_where.push_back({(u32)loc.str.size(), at}); s_counts.push_back((u32)loc.str.size());
        for (int k = 0; k < 30; k++) {
            const int art = kLocArt[k];
            std::u16string n = nick[art].empty() ? trimmed(loc.str[at + k]) : loc_utf16(nick[art]);
            std::u16string f = full[art].empty() ? trimmed(loc.str[at + 32 + k]) : loc_utf16(full[art]);
            n.resize(std::max(n.size(), kNickCap), 0); f.resize(std::max(f.size(), kFullCap), 0);
            loc.str[at + k] = n; loc.str[at + 32 + k] = f; changed++;
        }
        iso_add_overlay_data(file, loc.build(), "club names");
    }
    return changed;
}
// In the game's memory: the club's nickname and full name are rewritten in place, in the room left at boot.
static std::map<int, std::pair<std::string, std::string>> s_club_wanted;
static void put_str(u32 table, int index, const std::u16string& v) {
    extern u8* g_ram; const u8* p = g_ram + table;
    const u32 size = lr32(p + 4), off = lr32(p + 16 + 4 * index); if (off + 2 * v.size() > size) return;
    u8* q = g_ram + table + off;
    for (size_t i = 0; i < v.size(); i++) { q[2 * i] = (u8)v[i]; q[2 * i + 1] = (u8)(v[i] >> 8); }
}
static int apply_club(int art) {
    auto w = s_club_wanted.find(art); int k = -1; for (int i = 0; i < 30; i++) if (kLocArt[i] == art) k = i;
    if (w == s_club_wanted.end() || k < 0) return 0;
    extern u8* g_ram; int n = 0;
    for (u32 t : s_tables) { if (!table_ok(t)) continue; const u32 cnt = lr32(g_ram + t + 12);
        for (const LocWhere& lw : s_loc_where) if (lw.count == cnt) { put_str(t, lw.at + k, padded(w->second.first, kNickCap)); put_str(t, lw.at + 32 + k, padded(w->second.second, kFullCap)); n++; } }
    return n;
}
int loc_ram_set_club(int art, const std::string& nick, const std::string& full) {
    s_club_wanted[art] = {nick, full}; tables_changed(); return apply_club(art);
}
// ---------------------------------------------------------------- other strings that change while the game runs
// loc_track(key, values): at boot, every string of both tables that is exactly one of values gets room to grow and
// is remembered under key; loc_ram_set(key, text) later rewrites those strings in the tables the game loaded.
// (Ballpark names: the menus show the park of the season.)
struct LocTracked { u32 count; int index; };
static std::map<std::string, std::vector<LocTracked>> s_tracked;
static const size_t kTrackCap = 40;
int loc_track(const std::string& key, const std::vector<std::string>& values) {
    int n = 0;
    for (const char* file : {"DATA/FEENG.LOC", "DATA/IGENG.LOC"}) {
        std::vector<u8> d; LocFile loc;
        if (!iso_file_bytes(file, d) || !loc.parse(d)) continue;
        int hits = 0;
        for (size_t i = 0; i < loc.str.size(); i++)
            for (const std::string& v : values)
                if (!v.empty() && trimmed(loc.str[i]) == loc_utf16(v)) { loc.str[i] = trimmed(loc.str[i]); loc.str[i].resize(kTrackCap, 0); s_tracked[key].push_back({(u32)loc.str.size(), (int)i}); hits++; break; }
        if (hits && std::find(s_counts.begin(), s_counts.end(), (u32)loc.str.size()) == s_counts.end()) s_counts.push_back((u32)loc.str.size());
        if (hits) { iso_add_overlay_data(file, loc.build(), key.c_str()); n += hits; }
    }
    return n;
}
static std::map<std::string, std::string> s_track_wanted;
static int apply_tracked(const std::string& key) {
    auto it = s_tracked.find(key); auto w = s_track_wanted.find(key); if (it == s_tracked.end() || w == s_track_wanted.end()) return 0;
    extern u8* g_ram; int n = 0; const std::u16string nu = padded(w->second, kTrackCap);
    for (u32 t : s_tables) { if (!table_ok(t)) continue; const u32 cnt = lr32(g_ram + t + 12);
        for (const LocTracked& tr : it->second) if (tr.count == cnt) { put_str(t, tr.index, nu); n++; } }
    return n;
}
int loc_ram_set(const std::string& key, const std::string& text) {
    s_track_wanted[key] = text; tables_changed(); return apply_tracked(key);
}
// Called now and then: when the game has (re)loaded its string tables, what was set goes in again.
void loc_ram_refresh() {
    if (s_club_wanted.empty() && s_track_wanted.empty()) return;
    if (!tables_changed()) return;
    int n = 0;
    for (auto& w : s_club_wanted) n += apply_club(w.first);
    for (auto& w : s_track_wanted) n += apply_tracked(w.first);
    rlog("loc", "string tables at %zu places in memory: %d names put in", s_tables.size(), n);
}
