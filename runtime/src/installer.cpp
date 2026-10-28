// Features: optional downloads (see installer.h). Downloads run curl, unpacking runs tar: both come with
// macOS, Windows 10 (1803) and later, and practically every Linux, so the game needs no network library.
#include "installer.h"
#include "settings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
extern void rlog(const char* tag, const char* fmt, ...);

#ifndef MVP_FEATURES_URL
#define MVP_FEATURES_URL ""
#endif

// ---------------------------------------------------------------- a small JSON reader
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false; double n = 0; std::string s;
    std::vector<JVal> a; std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const { for (auto& p : o) if (p.first == k) return &p.second; return nullptr; }
    std::string str(const char* k, const std::string& d = "") const { const JVal* v = get(k); return v && v->t == STR ? v->s : d; }
    double num(const char* k, double d = 0) const { const JVal* v = get(k); return v && v->t == NUM ? v->n : d; }
    bool boolean(const char* k, bool d = false) const { const JVal* v = get(k); return v && v->t == BOOL ? v->b : d; }
};
struct JParser {
    const char* p; const char* e; bool ok = true;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void utf8(std::string& o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xc0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xe0 | c >> 12); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xf0 | c >> 18); o += (char)(0x80 | (c >> 12 & 63)); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
    }
    std::string string() {
        std::string o; p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break; case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                case 'u': { if (e - p < 5) { ok = false; return o; } unsigned c = (unsigned)std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16); p += 4;
                            if (c >= 0xd800 && c < 0xdc00 && e - p > 6 && p[1] == '\\' && p[2] == 'u') { unsigned d = (unsigned)std::strtoul(std::string(p + 3, p + 7).c_str(), nullptr, 16); c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); p += 6; }
                            utf8(o, c); break; }
                default: o += *p;
                }
                p++;
            } else o += *p++;
        }
        if (p < e) p++; else ok = false;
        return o;
    }
    JVal value(int depth = 0) {
        JVal v; ws(); if (p >= e || depth > 64) { ok = false; return v; }
        if (*p == '{') {
            v.t = JVal::OBJ; p++; ws();
            if (p < e && *p == '}') { p++; return v; }
            while (ok && p < e) {
                ws(); if (p >= e || *p != '"') { ok = false; break; }
                std::string k = string(); ws(); if (p >= e || *p != ':') { ok = false; break; } p++;
                v.o.emplace_back(k, value(depth + 1)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false;
            }
        } else if (*p == '[') {
            v.t = JVal::ARR; p++; ws();
            if (p < e && *p == ']') { p++; return v; }
            while (ok && p < e) {
                v.a.push_back(value(depth + 1)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false;
            }
        } else if (*p == '"') { v.t = JVal::STR; v.s = string(); }
        else if (!std::strncmp(p, "true", 4)) { v.t = JVal::BOOL; v.b = true; p += 4; }
        else if (!std::strncmp(p, "false", 5)) { v.t = JVal::BOOL; p += 5; }
        else if (!std::strncmp(p, "null", 4)) { p += 4; }
        else { char* q; v.t = JVal::NUM; v.n = std::strtod(p, &q); if (q == p) ok = false; p = q; }
        return v;
    }
};
static bool json_parse(const std::string& text, JVal& out) { JParser jp{text.data(), text.data() + text.size()}; out = jp.value(); return jp.ok; }

// ---------------------------------------------------------------- SHA-256
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64]; size_t blen = 0; uint64_t total = 0;
    static uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }
    void block(const uint8_t* b) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
        for (int i = 16; i < 64; i++) { uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & bb) ^ (a & c) ^ (bb & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const uint8_t* p, size_t n) {
        total += n;
        while (n) { size_t k = std::min(n, 64 - blen); std::memcpy(buf + blen, p, k); blen += k; p += k; n -= k; if (blen == 64) { block(buf); blen = 0; } }
    }
    std::string hex() {
        uint64_t bits = total * 8; uint8_t pad = 0x80; add(&pad, 1); uint8_t z = 0;
        while (blen != 56) add(&z, 1);
        uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i)); add(L, 8);
        char o[65]; for (int i = 0; i < 8; i++) std::snprintf(o + i * 8, 9, "%08x", h[i]); return o;
    }
};

// ---------------------------------------------------------------- processes (curl, tar) that can be cancelled
static std::atomic<bool> s_cancel{false};
// runs argv with its output to log; returns the exit code, -1 when it couldn't start, -2 when cancelled.
// tick() is called about 10 times a second while it runs.
template <class F> static int run_process(const std::vector<std::string>& argv, const std::string& log, F tick) {
#ifdef _WIN32
    std::wstring cmd;
    auto widen = [](const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n); return w; };
    for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += L'"'; for (wchar_t c : widen(a)) { if (c == L'"') cmd += L'\\'; cmd += c; } cmd += L'"'; }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(widen(log).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = out; si.hStdError = out; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { if (out != INVALID_HANDLE_VALUE) CloseHandle(out); return -1; }
    int rc = -1;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) { DWORD c = 0; GetExitCodeProcess(pi.hProcess, &c); rc = (int)c; break; }
        tick();
        if (s_cancel) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); rc = -2; break; }
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return rc;
#else
    std::vector<char*> av; for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid = 0;
    int r = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r != 0) return -1;
    int rc = -1;
    for (;;) {
        int st = 0; pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128; if (WIFEXITED(st) && WEXITSTATUS(st) == 127) rc = -1; break; }
        if (w < 0) break;
        tick();
        if (s_cancel) { kill(pid, SIGTERM); for (int i = 0; i < 50 && waitpid(pid, &st, WNOHANG) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100)); kill(pid, SIGKILL); waitpid(pid, &st, 0); rc = -2; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return rc;
#endif
}
static std::string tool(const char* name) {
#ifdef _WIN32
    char sys[MAX_PATH]; UINT n = GetSystemDirectoryA(sys, MAX_PATH);           // Windows' own curl.exe / tar.exe (not a stray one in PATH)
    if (n && n < MAX_PATH) { std::string p = std::string(sys) + "\\" + name + ".exe"; std::error_code ec; if (fs::exists(p, ec)) return p; }
    return std::string(name) + ".exe";
#else
    for (const char* d : {"/usr/bin/", "/bin/", "/usr/local/bin/", "/opt/homebrew/bin/"}) { std::string p = std::string(d) + name; if (access(p.c_str(), X_OK) == 0) return p; }
    return name;
#endif
}
static uint64_t file_size(const std::string& p) { std::error_code ec; uint64_t n = fs::file_size(p, ec); return ec ? 0 : n; }

// ---------------------------------------------------------------- state
static std::vector<Feature> s_feats;
static std::string s_source, s_base_url, s_local_dir;
static std::mutex s_mx;
static FeatureProgress s_prog;
static std::thread s_worker;
static std::atomic<bool> s_busy{false};
static std::vector<std::pair<std::string, bool>> s_job;          // what the running install does, in order: name, install (else remove)
static const char* kDir = "features";

std::vector<Feature>& features() { return s_feats; }
std::string features_source() { return s_source; }
std::string human_bytes(uint64_t b) {
    char o[32];
    if (b >= 1000000000ull) std::snprintf(o, sizeof o, b >= 10000000000ull ? "%.0f GB" : "%.1f GB", b / 1e9);
    else if (b >= 1000000ull) std::snprintf(o, sizeof o, "%.0f MB", b / 1e6);
    else if (b >= 1000ull) std::snprintf(o, sizeof o, "%.0f KB", b / 1e3);
    else std::snprintf(o, sizeof o, "%llu bytes", (unsigned long long)b);
    return o;
}
static std::map<std::string, std::string> read_installed() {
    std::map<std::string, std::string> m;
    if (FILE* f = std::fopen((std::string(kDir) + "/installed.txt").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            char id[256] = {0}, ver[256] = {0};
            if (std::sscanf(line, "%255s %255s", id, ver) >= 1 && id[0] != '#') m[id] = ver;
        }
        std::fclose(f);
    }
    return m;
}
static void write_installed(const std::map<std::string, std::string>& m) {
    std::error_code ec; fs::create_directories(kDir, ec);
    std::string tmp = std::string(kDir) + "/installed.txt.tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        std::fprintf(f, "# features installed by the launcher: id version\n");
        for (auto& p : m) std::fprintf(f, "%s %s\n", p.first.c_str(), p.second.empty() ? "-" : p.second.c_str());
        std::fclose(f);
        fs::rename(tmp, std::string(kDir) + "/installed.txt", ec);
    }
}
// a feature being installed has features/<id>.incomplete: its version, then the archives already unpacked. Until
// the last one is in, the feature isn't installed (its folders are there after the first archive of several), and
// Install continues with the archives that are missing instead of starting over.
static std::string incomplete_path(const std::string& id) { return std::string(kDir) + "/" + id + ".incomplete"; }
static std::set<std::string> read_incomplete(const Feature& f) {
    std::set<std::string> out;
    if (FILE* fp = std::fopen(incomplete_path(f.id).c_str(), "r")) {
        char line[512]; bool ok = false;
        if (std::fgets(line, sizeof line, fp)) { std::string v = line; while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back(); ok = v == "version " + f.version; }
        while (ok && std::fgets(line, sizeof line, fp)) { std::string v = line; while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back(); if (!v.empty()) out.insert(v); }
        std::fclose(fp);
    }
    return out;
}
static void begin_incomplete(const Feature& f) {
    std::error_code ec; fs::create_directories(kDir, ec);
    const std::string p = incomplete_path(f.id);
    if (!read_incomplete(f).empty()) return;                                 // continuing
    if (FILE* fp = std::fopen(p.c_str(), "w")) { std::fprintf(fp, "version %s\n", f.version.c_str()); std::fclose(fp); }
}
static void mark_unpacked(const Feature& f, const std::string& file) {
    if (FILE* fp = std::fopen(incomplete_path(f.id).c_str(), "a")) { std::fprintf(fp, "%s\n", file.c_str()); std::fclose(fp); }
}
static void refresh_state() {
    auto inst = read_installed();
    for (Feature& f : s_feats) {
        std::error_code ec; bool all = !f.paths.empty();
        for (auto& p : f.paths) if (!fs::exists(p, ec)) { all = false; break; }
        const bool partial = fs::exists(incomplete_path(f.id), ec);           // stopped half way: still ticked, Install continues it
        if (partial) all = false;
        f.installed = all;
        auto it = inst.find(f.id);
        f.installed_version = it != inst.end() && all ? it->second : "";
        f.selected = f.installed || f.required || partial;
        if (!g_settings.setup_done && inst.empty() && f.def) f.selected = true;      // the first setup: the recommended ones
    }
}
static std::string url_for(const std::string& file, const std::string& url) {
    if (!url.empty()) return url;
    if (file.find("://") != std::string::npos) return file;
    return s_base_url.empty() ? "" : s_base_url + file;
}
static std::string local_copy(const std::string& file) {         // archives next to the manifest (offline installs)
    std::error_code ec;
    for (const std::string& d : {s_local_dir, std::string(kDir)}) {
        if (d.empty()) continue;
        std::string p = d + "/" + file;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) return p;
    }
    return "";
}
static bool parse_manifest(const std::string& text, std::string& err) {
    JVal j;
    if (!json_parse(text, j) || j.t != JVal::OBJ) { err = "The feature list is damaged."; return false; }
    { std::string b = j.str("base_url"); if (!b.empty()) s_base_url = b.back() == '/' ? b : b + "/"; }
    std::vector<Feature> v;
    if (const JVal* a = j.get("features")) for (const JVal& c : a->a) {
        Feature f;
        f.id = c.str("id"); f.name = c.str("name", f.id); f.group = c.str("group"); f.desc = c.str("description"); f.version = c.str("version", "1");
        f.install_size = (uint64_t)c.num("install_size"); f.def = c.boolean("default"); f.required = c.boolean("required");
        auto strs = [&](const char* k, std::vector<std::string>& out) { if (const JVal* x = c.get(k)) for (const JVal& s : x->a) if (s.t == JVal::STR) out.push_back(s.s); };
        strs("requires", f.requires_); strs("excludes", f.excludes); strs("paths", f.paths); strs("remove", f.remove);
        f.era = c.boolean("era"); f.season = (int)c.num("season");
        if (const JVal* x = c.get("parts")) for (const JVal& p : x->a) {
            FeaturePart pt; pt.file = p.str("file"); pt.url = p.str("url"); pt.sha256 = p.str("sha256"); pt.size = (uint64_t)p.num("size");
            f.parts.push_back(pt); f.download_size += pt.size;
        }
        if (const JVal* x = c.get("previews")) for (const JVal& p : x->a) {
            FeaturePreview pv; pv.file = p.str("file"); pv.url = p.str("url"); pv.caption = p.str("caption"); pv.after_file = p.str("after"); pv.after_url = p.str("after_url");
            f.previews.push_back(pv);
        }
        auto sets = [&](const char* k, std::vector<std::pair<std::string, int>>& out) { if (const JVal* x = c.get(k)) for (auto& p : x->o) if (p.second.t == JVal::NUM || p.second.t == JVal::BOOL) out.push_back({p.first, p.second.t == JVal::BOOL ? (int)p.second.b : (int)p.second.n}); };
        sets("on_install", f.on_install); sets("on_remove", f.on_remove);
        if (!f.id.empty()) v.push_back(f);
    }
    if (v.empty()) { err = "The feature list is empty."; return false; }
    s_feats.swap(v);
    return true;
}
static std::string read_text(const std::string& p) {
    std::string t; if (FILE* f = std::fopen(p.c_str(), "rb")) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) t.append(b, n); std::fclose(f); } return t;
}
static std::string manifest_url() {
    if (const char* u = std::getenv("MVP_FEATURES_URL")) return u;
    if (!g_settings.features_url.empty()) return g_settings.features_url;
    return MVP_FEATURES_URL;
}
bool features_load(std::string& err, bool refresh) {
    if (s_busy) return !s_feats.empty();
    err.clear();
    std::error_code ec; fs::create_directories(kDir, ec);
    // 1) a local list: MVP_FEATURES_DIR or features/ holding manifest.json next to the archives (offline install)
    std::string local;
    if (const char* d = std::getenv("MVP_FEATURES_DIR")) { s_local_dir = d; local = std::string(d) + "/manifest.json"; }
    // 2) the release's list, fetched (kept as features/manifest.json for offline starts)
    const std::string cached = std::string(kDir) + "/manifest.json";
    std::string url = manifest_url();
    if (local.empty() && !url.empty()) {
        static bool fetched = false;
        if (!fetched || refresh) {
            fetched = true;
            const std::string tmp = cached + ".download", log = std::string(kDir) + "/download.log";
            fs::remove(tmp, ec);
            int rc = run_process(std::vector<std::string>{tool("curl"), "-L", "--fail", "-s", "-S", "--connect-timeout", "15", "--max-time", "60", "-o", tmp, url}, log, [] {});
            if (rc == 0 && file_size(tmp) > 0) { fs::rename(tmp, cached, ec); }
            else { fs::remove(tmp, ec); rlog("features", "could not fetch %s (curl %d)", url.c_str(), rc); if (!fs::exists(cached, ec)) err = rc == -1 ? "curl is missing, so features can't be downloaded." : "Couldn't reach the download server. Check your internet connection and try again."; }
            size_t sl = url.rfind('/'); if (sl != std::string::npos) s_base_url = url.substr(0, sl + 1);
        }
    }
    if (local.empty()) local = cached;
    if (!fs::exists(local, ec)) { if (err.empty()) err = url.empty() ? "No feature list: this build has no download address. Put the features folder (manifest.json and its archives) next to the game." : "No feature list yet."; s_feats.clear(); return false; }
    if (s_local_dir.empty()) { size_t sl = local.find_last_of("/\\"); s_local_dir = sl == std::string::npos ? "." : local.substr(0, sl); }
    std::string perr;
    if (!parse_manifest(read_text(local), perr)) { err = perr; return false; }
    s_source = local;
    refresh_state();
    std::string f; for (auto& x : s_feats) f += x.id + (x.installed ? "* " : " ");
    rlog("features", "%zu features from %s: %s", s_feats.size(), local.c_str(), f.c_str());
    return true;
}
static Feature* find_feat(const std::string& id) { for (Feature& f : s_feats) if (f.id == id) return &f; return nullptr; }
void features_select(const std::string& id, bool on) {
    Feature* f = find_feat(id); if (!f || (f->required && !on)) return;
    f->selected = on;
    if (on) {
        for (auto& r : f->requires_) features_select(r, true);
        for (auto& x : f->excludes) if (Feature* g = find_feat(x)) g->selected = false;
    } else for (Feature& g : s_feats) if (g.selected && std::find(g.requires_.begin(), g.requires_.end(), id) != g.requires_.end()) features_select(g.id, false);
}
static bool needs_update(const Feature& f) { return f.installed && !f.installed_version.empty() && f.installed_version != f.version; }
bool features_changes_pending() {
    for (const Feature& f : s_feats) if (f.selected != f.installed || (f.selected && needs_update(f))) return true;
    return false;
}
uint64_t features_disk_free() { std::error_code ec; fs::space_info si = fs::space(".", ec); return ec ? 0 : si.available; }
void features_plan(uint64_t& download, uint64_t& add_disk, uint64_t& free_disk) {
    download = add_disk = free_disk = 0;
    for (const Feature& f : s_feats) {
        if (f.selected && (!f.installed || needs_update(f))) {
            bool local = true; for (auto& p : f.parts) if (local_copy(p.file).empty()) local = false;
            if (!local) download += f.download_size;
            if (!f.installed) add_disk += f.install_size;
        }
        if (!f.selected && f.installed) free_disk += f.install_size;
    }
}
FeatureProgress features_progress() { std::lock_guard<std::mutex> lk(s_mx); return s_prog; }
std::vector<std::pair<std::string, bool>> features_job() { std::lock_guard<std::mutex> lk(s_mx); return s_job; }
void features_cancel() { s_cancel = true; }
bool features_busy() { return s_busy; }

static void set_prog(const std::function<void(FeatureProgress&)>& f) { std::lock_guard<std::mutex> lk(s_mx); f(s_prog); }

// Downloads run several at once (kConnections curl processes, each fetching a different archive): one connection
// to GitHub's download servers rarely gets near a fast line's speed, several together do. Archives are checked and
// unpacked in order on the worker thread while the next ones download. Downloading ahead only goes as far as the
// drive has room for (everything still to unpack, plus the archives on their way): a nearly full drive downloads
// one archive at a time, as before.
static const int kConnections = 4;
struct PartJob {
    Feature* f = nullptr; const FeaturePart* p = nullptr;
    std::string src;                       // the archive on disk (in features/downloads/, or an offline copy)
    bool downloaded = false;               // fetched by us (deleted after unpacking)
    std::atomic<int> state{0};             // 0 waiting, 1 downloading, 2 on disk, 3 failed
    std::atomic<uint64_t> got{0};          // bytes on disk so far
    std::string err;
};
static int connections() {
    if (const char* e = std::getenv("MVP_DL_CONNECTIONS")) { int n = std::atoi(e); if (n >= 1 && n <= 16) return n; }
    return kConnections;
}

// one archive into features/downloads/ (resumable); on any thread
static void download_part(PartJob& j) {
    std::error_code ec;
    const FeaturePart& p = *j.p;
    j.src = local_copy(p.file);
    if (!j.src.empty()) { j.got = p.size; j.state = 2; return; }
    const std::string url = url_for(p.file, p.url);
    if (url.empty()) { j.err = "No download address for " + p.file + "."; j.state = 3; return; }
    const std::string dl_dir = std::string(kDir) + "/downloads"; fs::create_directories(dl_dir, ec);
    j.src = dl_dir + "/" + p.file; j.downloaded = true;
    if (p.size && file_size(j.src) == p.size) { j.got = p.size; j.state = 2; return; }
    j.state = 1;
    auto fetch = [&](bool resume) {
        std::vector<std::string> av{tool("curl"), "-L", "--fail", "-s", "-S", "--retry", "5", "--retry-delay", "3", "--connect-timeout", "20"};
        if (resume) { av.push_back("-C"); av.push_back("-"); }
        av.push_back("-o"); av.push_back(j.src); av.push_back(url);
        return run_process(av, std::string(kDir) + "/download.log", [&] { uint64_t n = file_size(j.src); j.got = p.size ? std::min<uint64_t>(n, p.size) : n; });
    };
    int rc = fetch(true);
    if (rc == 33 && p.size && file_size(j.src) == p.size) rc = 0;             // resumed a file that was already complete
    else if (rc == 33) { fs::remove(j.src, ec); rc = fetch(false); }        // the server can't resume: start that file over
    if (rc == 0) { j.got = p.size ? p.size : file_size(j.src); j.state = 2; return; }
    j.err = rc == -2 ? "cancelled" : rc == -1 ? std::string("curl is missing, so features can't be downloaded.")
          : "Download of " + p.file + " failed (curl error " + std::to_string(rc) + "). Check your connection and press Install again: it continues where it stopped.";
    j.state = 3;
}

// one downloaded archive: checked, unpacked into the game folder, deleted; on the worker thread
static bool unpack_part(PartJob& j, std::string& err) {
    std::error_code ec;
    const FeaturePart& p = *j.p;
    if (!p.sha256.empty()) {
        set_prog([&](FeatureProgress& g) { g.phase = FP_VERIFYING; g.step = "Checking " + p.file; });
        Sha256 h; std::vector<uint8_t> buf(1 << 20);
        if (FILE* fp = std::fopen(j.src.c_str(), "rb")) { size_t n; while ((n = std::fread(buf.data(), 1, buf.size(), fp)) > 0) { h.add(buf.data(), n); if (s_cancel) break; } std::fclose(fp); }
        if (s_cancel) { err = "cancelled"; return false; }
        if (h.hex() != p.sha256) {
            if (j.downloaded) fs::remove(j.src, ec);
            err = p.file + " arrived damaged (its checksum doesn't match). Press Install again to download it once more."; return false;
        }
    }
    set_prog([&](FeatureProgress& g) { g.phase = FP_EXTRACTING; g.step = "Unpacking " + p.file; });
    const std::string root = fs::current_path(ec).string();
    int rc = run_process(std::vector<std::string>{tool("tar"), "-xf", j.src, "-C", root}, std::string(kDir) + "/download.log", [] {});
    if (rc == -2) { err = "cancelled"; return false; }
    if (rc != 0) { err = rc == -1 ? std::string("tar is missing, so features can't be unpacked.") : "Unpacking " + p.file + " failed (tar error " + std::to_string(rc) + "). Is the disk full?"; return false; }
    if (j.downloaded) fs::remove(j.src, ec);                                  // the archive's job is done
    return true;
}
static void apply_settings(const std::vector<std::pair<std::string, int>>& v) { for (auto& p : v) settings_set(p.first, p.second); }
// removing a feature turns off only what installing it turned on (a player who chose an era keeps it when today's rosters go)
static void revert_settings(const Feature& f) {
    for (auto& p : f.on_remove) {
        int cur = 0; bool known = settings_get(p.first, cur);
        auto on = std::find_if(f.on_install.begin(), f.on_install.end(), [&](const std::pair<std::string, int>& x) { return x.first == p.first; });
        if (!known || on == f.on_install.end() || cur == on->second) settings_set(p.first, p.second);
    }
}
static void worker() {
    std::vector<Feature*> add, del;
    for (Feature& f : s_feats) {
        if (f.selected && (!f.installed || needs_update(f))) add.push_back(&f);
        std::error_code ec;
        if (!f.selected && (f.installed || fs::exists(incomplete_path(f.id), ec))) del.push_back(&f);
    }
    std::map<std::string, std::set<std::string>> have;                       // archives a stopped install already unpacked
    for (Feature* f : add) have[f->id] = read_incomplete(*f);
    uint64_t total = 0; for (Feature* f : add) for (auto& p : f->parts) if (!have[f->id].count(p.file)) total += p.size;
    { std::lock_guard<std::mutex> lk(s_mx); s_job.clear(); for (Feature* f : del) s_job.push_back({f->name, false}); for (Feature* f : add) s_job.push_back({f->name, true}); }
    set_prog([&](FeatureProgress& g) { g = FeatureProgress(); g.total = total; g.features_total = (int)(add.size() + del.size()); });
    auto inst = read_installed();
    std::string err; int nd = 0;
    std::error_code ec;
    for (Feature* f : del) {
        set_prog([&](FeatureProgress& g) { g.phase = FP_REMOVING; g.feature = f->name; g.step = "Removing " + f->name; });
        for (auto& p : f->remove.empty() ? f->paths : f->remove) fs::remove_all(p, ec);
        fs::remove(incomplete_path(f->id), ec);
        inst.erase(f->id); write_installed(inst);
        revert_settings(*f);
        rlog("features", "removed %s", f->id.c_str());
        set_prog([&](FeatureProgress& g) { g.features_done = ++nd; });
    }
    // every archive of every feature to add, in install order; downloaders take them from the front
    std::vector<std::unique_ptr<PartJob>> jobs;
    for (Feature* f : add) for (const FeaturePart& p : f->parts) if (!have[f->id].count(p.file)) { jobs.emplace_back(new PartJob); jobs.back()->f = f; jobs.back()->p = &p; }
    std::atomic<size_t> next{0}, unpacked{0};
    std::atomic<bool> stop{false};
    std::vector<uint64_t> rest(jobs.size() + 1, 0);                         // rest[i]: bytes of archives i.. (unpacked ~ the same size)
    for (size_t i = jobs.size(); i-- > 0;) rest[i] = rest[i + 1] + jobs[i]->p->size;
    auto room_for = [&](size_t i) {
        size_t u = unpacked;
        if (i == u) return true;                                             // the next one to unpack always goes
        uint64_t coming = 0; for (auto& j : jobs) if (j->state == 1) coming += j->p->size - std::min(j->p->size, j->got.load());
        return features_disk_free() >= rest[u] + coming + jobs[i]->p->size + 1000000000ull;
    };
    std::vector<std::thread> dl;
    const int nconn = std::max(1, std::min<int>(connections(), (int)jobs.size()));
    for (int t = 0; t < nconn; t++) dl.emplace_back([&] {
        for (;;) {
            size_t i;
            for (;;) {                                                       // wait for room: at most kAhead archives ahead of unpacking
                if (stop || s_cancel) return;
                i = next.load();
                if (i >= jobs.size()) return;
                if (i <= unpacked + nconn && room_for(i) && next.compare_exchange_weak(i, i + 1)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            download_part(*jobs[i]);
        }
    });
    auto t_last = std::chrono::steady_clock::now(); uint64_t b_last = 0; double spd = 0;
    auto bytes = [&] { uint64_t n = 0; for (auto& j : jobs) n += j->state == 2 ? j->p->size : j->got.load(); return n; };
    auto tick = [&](const char* step_of) {                                   // progress while waiting for downloads
        auto now = std::chrono::steady_clock::now(); double el = std::chrono::duration<double>(now - t_last).count();
        uint64_t n = bytes();
        if (el >= 0.5) { double s = (n - std::min(n, b_last)) / el; spd = spd == 0 ? s : spd * 0.7 + 0.3 * s; t_last = now; b_last = n; }
        int active = 0; for (auto& j : jobs) if (j->state == 1) active++;
        set_prog([&](FeatureProgress& g) {
            g.done = std::min(n, total); g.speed = spd;
            if (step_of) { g.phase = FP_DOWNLOADING; g.step = std::string("Downloading ") + step_of + (active > 1 ? " (+" + std::to_string(active - 1) + " more at once)" : ""); }
        });
    };
    b_last = bytes();
    size_t k = 0;
    for (Feature* f : add) {
        set_prog([&](FeatureProgress& g) { g.feature = f->name; });
        if (needs_update(*f) && have[f->id].empty()) for (auto& p : f->remove.empty() ? f->paths : f->remove) fs::remove_all(p, ec);
        bool ok = true;
        size_t todo = 0; for (auto& p : f->parts) if (!have[f->id].count(p.file)) todo++;
        for (size_t n = 0; n < todo; n++, k++) {
            PartJob& j = *jobs[k];
            while (j.state < 2 && !s_cancel) { tick(j.p->file.c_str()); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            if (s_cancel) { err = "cancelled"; ok = false; break; }
            if (j.state == 3) { err = j.err; ok = false; break; }
            tick(nullptr);
            begin_incomplete(*f);
            std::atomic<bool> unpacking{true};
            std::thread prog([&] { while (unpacking) { tick(nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(200)); } });
            ok = unpack_part(j, err);                                        // downloads go on meanwhile
            unpacking = false; prog.join();
            unpacked++;
            if (!ok) break;
            mark_unpacked(*f, j.p->file);
        }
        if (!ok) break;
        fs::remove(incomplete_path(f->id), ec);
        inst[f->id] = f->version; write_installed(inst);
        apply_settings(f->on_install);
        rlog("features", "installed %s %s", f->id.c_str(), f->version.c_str());
        set_prog([&](FeatureProgress& g) { g.features_done = ++nd; });
    }
    stop = true;
    if (!err.empty() && err != "cancelled") s_cancel = true;                // a failure stops the other downloads (they resume next time)
    for (auto& t : dl) t.join();
    s_cancel = false;
    if (g_settings.rosters == 2 && !fs::exists("mods/" + g_settings.league + "/pack.json", ec))       // its league pack was removed
        g_settings.rosters = fs::exists("mods/rosters-2026", ec) ? 1 : 0;
    if (g_settings.rosters == 1 && !fs::exists("mods/rosters-2026", ec)) g_settings.rosters = 0;
    settings_save(settings_path());
    refresh_state();
    set_prog([&](FeatureProgress& g) {
        g.phase = err.empty() ? FP_DONE : err == "cancelled" ? FP_CANCELLED : FP_FAILED; g.error = err == "cancelled" ? "" : err;
        g.step = err.empty() ? "Done" : err == "cancelled" ? "Stopped. Press Install to continue where it stopped." : "Stopped";
    });
    if (!err.empty()) rlog("features", "stopped: %s", err.c_str());
    s_busy = false;
}
void features_apply() {
    if (s_busy) return;
    if (s_worker.joinable()) s_worker.join();
    s_cancel = false; s_busy = true;
    s_worker = std::thread(worker);
}

// ---------------------------------------------------------------- previews
static std::mutex s_pv_mx;
static std::map<std::string, int> s_pv_state;          // file -> 1 fetching, 2 there, 3 failed
std::string features_preview_path(const FeaturePreview& p, bool after) {
    const std::string& file = after ? p.after_file : p.file;
    if (file.empty()) return "";
    std::string l = local_copy("previews/" + file); if (!l.empty()) return l;
    std::string c = std::string(kDir) + "/previews/" + file;
    std::lock_guard<std::mutex> lk(s_pv_mx);
    return s_pv_state[file] == 2 ? c : "";
}
void features_fetch_previews() {
    std::vector<std::pair<std::string, std::string>> todo;                      // file, url
    {
        std::lock_guard<std::mutex> lk(s_pv_mx); std::error_code ec;
        for (const Feature& f : s_feats) for (const FeaturePreview& p : f.previews) for (int k = 0; k < 2; k++) {
            const std::string& file = k ? p.after_file : p.file; if (file.empty() || s_pv_state.count(file)) continue;
            if (!local_copy("previews/" + file).empty()) { s_pv_state[file] = 2; continue; }
            std::string c = std::string(kDir) + "/previews/" + file;
            if (fs::exists(c, ec)) { s_pv_state[file] = 2; continue; }
            std::string url = url_for("preview-" + file, k ? p.after_url : p.url);   // release assets have no folders if (url.empty()) { s_pv_state[file] = 3; continue; }
            s_pv_state[file] = 1; todo.push_back({file, url});
        }
    }
    if (todo.empty()) return;
    std::thread([todo] {
        std::error_code ec; fs::create_directories(std::string(kDir) + "/previews", ec);
        for (auto& t : todo) {
            std::string c = std::string(kDir) + "/previews/" + t.first, tmp = c + ".download";
            int rc = run_process(std::vector<std::string>{tool("curl"), "-L", "--fail", "-s", "--connect-timeout", "15", "--max-time", "60", "-o", tmp, t.second}, std::string(kDir) + "/download.log", [] {});
            std::error_code e2;
            if (rc == 0 && file_size(tmp) > 0) fs::rename(tmp, c, e2); else fs::remove(tmp, e2);
            std::lock_guard<std::mutex> lk(s_pv_mx); s_pv_state[t.first] = rc == 0 ? 2 : 3;
        }
    }).detach();
}

// ---------------------------------------------------------------- command line: mvp2005 --features [install|remove id...]
int features_cli(int argc, char** argv) {
    std::string err;
    if (!features_load(err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::string cmd = argc > 0 ? argv[0] : "";
    if (cmd == "install" || cmd == "remove") {
        for (int i = 1; i < argc; i++) { if (!find_feat(argv[i])) { std::fprintf(stderr, "no feature %s\n", argv[i]); return 1; } features_select(argv[i], cmd == "install"); }
        uint64_t dl, add, fr; features_plan(dl, add, fr);
        std::fprintf(stderr, "download %s, %s more on disk, %s freed (free now %s)\n", human_bytes(dl).c_str(), human_bytes(add).c_str(), human_bytes(fr).c_str(), human_bytes(features_disk_free()).c_str());
        features_apply();
        FeatureProgress last;
        while (features_busy()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            FeatureProgress p = features_progress();
            if (p.step != last.step || p.done / 50000000 != last.done / 50000000) std::fprintf(stderr, "  [%d/%d] %s  %s / %s  %.1f MB/s\n", p.features_done, p.features_total, p.step.c_str(), human_bytes(p.done).c_str(), human_bytes(p.total).c_str(), p.speed / 1e6);
            last = p;
        }
        if (s_worker.joinable()) s_worker.join();
        FeatureProgress p = features_progress();
        std::fprintf(stderr, "%s%s\n", p.step.c_str(), p.error.empty() ? "" : (": " + p.error).c_str());
        return p.phase == FP_DONE ? 0 : 1;
    }
    for (const Feature& f : s_feats)
        std::fprintf(stderr, "%-22s %-10s %9s download %9s on disk  %s%s\n", f.id.c_str(), f.installed ? (f.installed_version.empty() ? "found" : ("v" + f.installed_version).c_str()) : "-",
                     human_bytes(f.download_size).c_str(), human_bytes(f.install_size).c_str(), f.name.c_str(), f.def ? "  (recommended)" : "");
    return 0;
}

// ---------------------------------------------------------------- loading the list without blocking the launcher
static std::atomic<int> s_load_state{0};                 // 0 not started, 1 loading, 2 loaded, 3 failed
static std::string s_load_err;
void features_load_async(bool refresh) {
    if (s_load_state == 1 || s_busy) return;
    s_load_state = 1;
    std::thread([refresh] {
        std::string err; bool ok = features_load(err, refresh);
        { std::lock_guard<std::mutex> lk(s_mx); s_load_err = err; }
        if (ok) features_fetch_previews();
        s_load_state = ok ? 2 : 3;
    }).detach();
}
int features_load_state() { return s_load_state; }
std::string features_load_error() { std::lock_guard<std::mutex> lk(s_mx); return s_load_err; }
