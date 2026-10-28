// Native memory cards (replaces MCMAN/MCSERV, RPC server 0x80000400), backed by host folders:
//   memcards/slot1/..., memcards/slot2/...   (override with MVP_MEMCARDS=<dir>)
// Slot 1 is inserted by default; set MVP_MEMCARD2=1 to insert slot 2 as well.
//
// Request layouts were read from the game's libmc (0x5BA768 init, 0x5BB0F0 GetInfo, 0x5BA9A8 Open,
// 0x5BAB08 Close, 0x5BABC8 Seek, 0x5BAD40 Read, 0x5BAE58 Write, 0x5BB780 Flush, 0x5BB450 ChDir,
// 0x5BB270 GetDir, 0x5BB660 Delete). Results go to the 4-byte receive buffer.
#include "ee_ops.h"
#include "ee_runtime.h"
#include "iop.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

static inline u32 rd(const u8* p, int o) { u32 v; std::memcpy(&v, p + o, 4); return v; }
static inline void wr(u8* p, int o, u32 v) { if (p) std::memcpy(p + o, &v, 4); }

enum { MC_OK = 0, MC_NOENTRY = -4, MC_DENIED = -5, MC_FULL = -2, MC_NOCARD = -10 };

static std::string root_dir() {
    const char* e = std::getenv("MVP_MEMCARDS");
    return e ? e : "memcards";
}
static bool inserted(u32 port) { return port == 0 || (port == 1 && std::getenv("MVP_MEMCARD2")); }
static std::string card_dir(u32 port) {
    std::string r = root_dir();
    mkdir_p(r.c_str());
    std::string d = r + "/slot" + std::to_string(port + 1);
    mkdir_p(d.c_str());
    return d;
}

static std::string s_cwd[2] = {"/", "/"};
static bool s_seen[2] = {false, false};

// Resolve a card path (absolute or relative to the current directory) to a host path.
static std::string resolve(u32 port, const char* name, std::string* card_path = nullptr) {
    std::string p = name;
    if (p.empty() || p[0] != '/') p = s_cwd[port & 1] + (s_cwd[port & 1].back() == '/' ? "" : "/") + p;
    std::vector<std::string> parts; std::string cur;
    for (char c : p + "/") {
        if (c == '/') {
            if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    std::string cp = "/"; for (size_t i = 0; i < parts.size(); i++) cp += (i ? "/" : "") + parts[i];
    if (card_path) *card_path = cp;
    return card_dir(port & 1) + cp;
}

struct McFile { FILE* f = nullptr; u32 port = 0; std::string path; };
static std::vector<McFile> s_files(32);

static int mc_open(u32 port, const char* name, u32 mode) {
    std::string host = resolve(port, name);
    if (mode & 0x40) {                                        // mkdir
        struct stat st;
        if (stat(host.c_str(), &st) == 0) return MC_NOENTRY;     // already exists (what MCMAN reports)
        return mkdir_p(host.c_str()) == 0 ? MC_OK : MC_NOENTRY;
    }
    const char* fm;
    struct stat st; bool exists = stat(host.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return MC_DENIED;
    if (!exists && !(mode & 0x200)) return MC_NOENTRY;
    if ((mode & 3) == 1) fm = "rb";
    else if (!exists || (mode & 0x400)) fm = "w+b";
    else fm = "r+b";
    FILE* f = fopen(host.c_str(), fm);
    if (!f) return MC_NOENTRY;
    for (size_t i = 0; i < s_files.size(); i++)
        if (!s_files[i].f) { s_files[i] = {f, port, host}; return (int)i; }
    fclose(f);
    return -7;                                                // too many open files
}

static void fill_entry(u8* e, const std::string& name, bool dir, u32 size) {
    std::memset(e, 0, 64);
    std::time_t t = std::time(nullptr); std::tm tmv = *std::localtime(&t);
    u8 dt[8] = {0, (u8)tmv.tm_sec, (u8)tmv.tm_min, (u8)tmv.tm_hour, (u8)tmv.tm_mday, (u8)(tmv.tm_mon + 1), 0, 0};
    u16 year = (u16)(tmv.tm_year + 1900); std::memcpy(dt + 6, &year, 2);
    std::memcpy(e, dt, 8); std::memcpy(e + 8, dt, 8);
    wr(e, 16, size);
    u16 attr = dir ? 0x8427 : 0x8497;                         // exists | readable | writeable | (dir/file) | PS2 format
    std::memcpy(e + 20, &attr, 2);
    std::strncpy((char*)e + 32, name.c_str(), 31);
}

// card-style wildcards: '?' one character, '*' any run
static bool wild(const char* p, const char* s) {
    for (; *p; p++, s++) {
        if (*p == '*') { for (p++;; s++) { if (wild(p, s)) return true; if (!*s) return false; } }
        if (!*s || (*p != '?' && *p != *s)) return false;
    }
    return !*s;
}

// GetDir: flags 0 starts a listing, anything else continues it (the game asks for a few entries at a time)
struct McEntry { std::string name; bool dir; u32 size; };
static std::vector<McEntry> s_list[2];
static size_t s_list_pos[2] = {0, 0};

static int mc_getdir(u32 port, const char* pattern, u32 flags, u32 maxent, u32 table) {
    port &= 1;
    if (!flags) {
        s_list[port].clear(); s_list_pos[port] = 0;
        std::string host = resolve(port, pattern);
        size_t slash = host.rfind('/');
        std::string dir = host.substr(0, slash), pat = host.substr(slash + 1);
        struct stat st;
        if (pat.find('*') == std::string::npos && pat.find('?') == std::string::npos) {
            if (stat(host.c_str(), &st) != 0) return 0;
            if (S_ISDIR(st.st_mode)) s_list[port].push_back({".", true, 0});       // the directory itself, like the real card
            else s_list[port].push_back({pat, false, (u32)st.st_size});
        } else {
#ifndef _WIN32
            DIR* d = opendir(dir.c_str());
            if (!d) return 0;
            std::vector<std::string> names;
            while (dirent* de = readdir(d)) { std::string nm = de->d_name; if (nm != "." && nm != "..") names.push_back(nm); }
            closedir(d);
#else
            std::vector<std::string> names;
            _finddata_t fd; intptr_t h = _findfirst((dir + "/*").c_str(), &fd);
            if (h == -1) return 0;
            do { std::string nm = fd.name; if (nm != "." && nm != "..") names.push_back(nm); } while (_findnext(h, &fd) == 0);
            _findclose(h);
#endif
            std::sort(names.begin(), names.end());
            if (dir != card_dir(port)) { names.insert(names.begin(), ".."); names.insert(names.begin(), "."); }
            for (auto& nm : names) {
                if (!wild(pat.c_str(), nm.c_str())) continue;
                bool isdir = true; u32 size = 0;
                if (nm != "." && nm != ".." && stat((dir + "/" + nm).c_str(), &st) == 0) { isdir = S_ISDIR(st.st_mode); if (!isdir) size = (u32)st.st_size; }
                s_list[port].push_back({nm, isdir, size});
            }
        }
    }
    int n = 0;
    while ((u32)n < maxent && s_list_pos[port] < s_list[port].size()) {
        const McEntry& en = s_list[port][s_list_pos[port]++];
        if (u8* e = ee_ptr((table + n * 64) & 0x1fffffff)) fill_entry(e, en.name, en.dir, en.size);
        n++;
    }
    return n;
}

static struct McServ : IopServer {
    McServ() : IopServer(0x80000400, "mcserv") {}
    void call(u32 fno, u8* s, u32 ssize, u8* out, u32 rsize, u32) override {
        int r = MC_OK;
        switch (fno) {
        case 0xfe:                                            // init: result, mcserv version, mcman version
            wr(out, 0, 0); wr(out, 4, 0x215); wr(out, 8, 0x218);
            return;
        case 0x01: {                                          // GetInfo(port, slot) -> info buffer
            u32 port = rd(s, 4), info = rd(s, 28);
            if (!inserted(port)) { r = MC_NOCARD; break; }
            card_dir(port);
            if (u8* p = ee_ptr(info & 0x1fffffff)) { wr(p, 0, 2); wr(p, 4, 8000); wr(p, 144, 1); }  // PS2 card, free KB, formatted
            r = s_seen[port & 1] ? 0 : -1;                    // -1 once: "new formatted card"
            s_seen[port & 1] = true;
            break;
        }
        case 0x02: {                                          // Open(port, slot, mode, name)
            u32 port = rd(s, 0), mode = rd(s, 8);
            if (!inserted(port)) { r = MC_NOCARD; break; }
            r = mc_open(port, (const char*)s + 20, mode);
            rlog("mc", "open slot%u '%s' mode %x -> %d", port + 1, (const char*)s + 20, mode, r);
            break;
        }
        case 0x03: {                                          // Close(fd)
            u32 fd = rd(s, 0);
            if (fd < s_files.size() && s_files[fd].f) { fclose(s_files[fd].f); s_files[fd].f = nullptr; } else r = -7;
            break;
        }
        case 0x04: {                                          // Seek(fd, offset, origin)
            u32 fd = rd(s, 0); s32 off = (s32)rd(s, 16); u32 origin = rd(s, 20);
            if (fd >= s_files.size() || !s_files[fd].f) { r = -7; break; }
            fseek(s_files[fd].f, off, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END);
            r = (int)ftell(s_files[fd].f);
            break;
        }
        case 0x05: {                                          // Read(fd, size, ee_buf, aux)
            u32 fd = rd(s, 0), size = rd(s, 12), buf = rd(s, 24), aux = rd(s, 28);
            if (fd >= s_files.size() || !s_files[fd].f) { r = -7; break; }
            std::vector<u8> tmp(size);
            r = (int)fread(tmp.data(), 1, size, s_files[fd].f);
            for (int i = 0; i < r; i++) ee_wr<u8>(buf + i, tmp[i]);
            if (u8* a = ee_ptr(aux & 0x1fffffff)) { wr(a, 0, 0); wr(a, 4, 0); }   // no unaligned fixups needed
            break;
        }
        case 0x06: {                                          // Write(fd, inline head bytes, aligned EE data)
            u32 fd = rd(s, 0), rest = rd(s, 12), head = rd(s, 20), src = rd(s, 24);
            if (fd >= s_files.size() || !s_files[fd].f) { r = -7; break; }
            std::vector<u8> tmp(s + 32, s + 32 + std::min<u32>(head, 16));
            for (u32 i = 0; i < rest; i++) tmp.push_back(ee_rd<u8>(src + i));
            r = (int)fwrite(tmp.data(), 1, tmp.size(), s_files[fd].f);
            break;
        }
        case 0x0a: {                                          // Flush(fd)
            u32 fd = rd(s, 0); if (fd < s_files.size() && s_files[fd].f) fflush(s_files[fd].f);
            break;
        }
        case 0x0c: {                                          // ChDir(port, slot, newdir, curdir_out)
            u32 port = rd(s, 0), cur_out = rd(s, 16);
            if (!inserted(port)) { r = MC_NOCARD; break; }
            if (u8* p = ee_ptr(cur_out & 0x1fffffff)) std::strncpy((char*)p, s_cwd[port & 1].c_str(), 1023);
            const char* nd = (const char*)s + 20;
            if (nd[0]) {
                std::string cp; std::string host = resolve(port, nd, &cp);
                struct stat st;
                if (stat(host.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) s_cwd[port & 1] = cp; else r = MC_NOENTRY;
            }
            break;
        }
        case 0x0d: {                                          // GetDir(port, slot, flags, maxent, table, name)
            u32 port = rd(s, 0), flags = rd(s, 8), maxent = rd(s, 12), table = rd(s, 16);
            if (!inserted(port)) { r = MC_NOCARD; break; }
            r = mc_getdir(port, (const char*)s + 20, flags, maxent, table);
            rlog("mc", "getdir slot%u '%s' %s max %u -> %d", port + 1, (const char*)s + 20, flags ? "more" : "first", maxent, r);
            break;
        }
        case 0x0f: {                                          // Delete(port, slot, name)
            u32 port = rd(s, 0);
            if (!inserted(port)) { r = MC_NOCARD; break; }
            std::string host = resolve(port, (const char*)s + 20);
            r = (std::remove(host.c_str()) == 0) ? MC_OK : MC_NOENTRY;
            break;
        }
        default:
            rlog("mc", "fno 0x%x (ssize %u) -> 0", fno, ssize);
        }
        if (out && rsize >= 4) wr(out, 0, (u32)r);
    }
} s_mcserv;
