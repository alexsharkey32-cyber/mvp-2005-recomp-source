// The game folder: where the settings, memory cards, the game program copied from the disc, the features
// (mods/, textures/hd/) and screenshots live. Every path in the runtime is relative to it, so main() makes it the
// current directory first.
//   1. $MVP_HOME
//   2. the current directory, when it already is one (a source checkout, a portable folder started from a shell)
//   3. the folder of the program, when it already is one (a portable install: version 1.0 kept everything there,
//      and a folder can be made portable by putting a "portable" file next to the program)
//   4. per user: macOS ~/Library/Application Support/MVP Baseball 2005 Recomp, Linux ~/.local/share/mvp2005-recomp,
//      Windows %LOCALAPPDATA%\MVP Baseball 2005 Recomp. Outside the program's folder, so a new version can simply
//      replace the old one. The first time, the data of an older version unzipped next to this one
//      (../MVPBaseball2005-*) is moved over.
// Files the release ships next to the program (seed/: the league rates, era logos ...) are copied in when missing.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#ifdef MVP_SDL
#include <SDL.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
static std::string s_program_dir;

static bool is_game_folder(const fs::path& d) {
    std::error_code ec;
    for (const char* m : {"mvp2005.ini", "mods", "game", "features", "memcards"}) if (fs::exists(d / m, ec)) return true;
    return false;
}
// what belongs to the player in a game folder (everything else there came with the program)
static const char* const kPlayerData[] = {"mvp2005.ini", "memcards", "mods", "textures", "features", "game", "music", "teams", "screenshots"};
// an older version's folder next to this one, with the player's things in it (the newest such folder)
static fs::path older_install(const fs::path& prog) {
    std::error_code ec; fs::path best; fs::file_time_type bt{};
    fs::path me = prog.lexically_normal(); if (!me.has_filename()) me = me.parent_path();   // (SDL's base path ends in a slash)
    fs::path parent = me.parent_path();
    for (auto it = fs::directory_iterator(parent, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path d = it->path();
        if (d == me || !it->is_directory(ec) || d.filename().string().rfind("MVPBaseball2005", 0) != 0) continue;
        if (!fs::exists(d / "mvp2005.ini", ec)) continue;
        auto t = fs::last_write_time(d / "mvp2005.ini", ec);
        if (best.empty() || t > bt) { best = d; bt = t; }
    }
    return best;
}
// moves the player's things from an older version's folder into dir; false when they can't move (another drive):
// then that folder stays the game folder
static bool move_player_data(const fs::path& from, const fs::path& dir) {
    std::error_code ec; fs::create_directories(dir, ec);
    for (const char* n : kPlayerData) {
        if (!fs::exists(from / n, ec) || fs::exists(dir / n, ec)) continue;
        fs::rename(from / n, dir / n, ec);
        if (ec) { std::fprintf(stderr, "could not move %s to %s: %s\n", (from / n).string().c_str(), dir.string().c_str(), ec.message().c_str()); return false; }
    }
    {   // the disc image the launcher remembers may have been inside the old folder
        const fs::path ini = dir / "mvp2005.ini"; std::string text;
        if (FILE* f = std::fopen(ini.string().c_str(), "rb")) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); std::fclose(f); }
        const std::string a = from.string(), b = dir.string(); bool changed = false;
        for (size_t at = 0; !a.empty() && (at = text.find(a, at)) != std::string::npos; at += b.size()) { text.replace(at, a.size(), b); changed = true; }
        if (changed) if (FILE* f = std::fopen(ini.string().c_str(), "wb")) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    }
    if (FILE* f = std::fopen((from / "MOVED.txt").string().c_str(), "w")) {
        std::fprintf(f, "Your settings, saves and features were moved to\n%s\nwhere newer versions find them. This folder can be deleted.\n", dir.string().c_str()); std::fclose(f);
    }
    std::fprintf(stderr, "moved the game folder of %s to %s\n", from.string().c_str(), dir.string().c_str());
    return true;
}
static std::string program_dir(const char* argv0) {
#ifdef MVP_SDL
    if (char* b = SDL_GetBasePath()) { std::string s = b; SDL_free(b); return s; }
#endif
#ifdef _WIN32
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n && n < MAX_PATH) return fs::path(buf).parent_path().string();
#elif defined(__linux__)
    { std::error_code ec; fs::path p = fs::read_symlink("/proc/self/exe", ec); if (!ec) return p.parent_path().string(); }
#endif
    std::error_code ec; fs::path p = fs::absolute(argv0 ? argv0 : ".", ec);
    return p.parent_path().string();
}
const std::string& program_folder() { return s_program_dir; }

void data_dir_init(const char* argv0) {
    std::error_code ec;
    s_program_dir = program_dir(argv0);
    fs::path prog = s_program_dir;
    fs::path dir;
    if (const char* h = std::getenv("MVP_HOME")) dir = h;
    else if (is_game_folder(fs::current_path(ec))) dir = fs::current_path(ec);
    else {
        const bool bundle = prog.string().find(".app/Contents") != std::string::npos;
        if (!bundle && (is_game_folder(prog) || fs::exists(prog / "portable", ec))) dir = prog;
        else {
#if defined(__APPLE__)
            const char* home = std::getenv("HOME");
            dir = fs::path(home ? home : ".") / "Library" / "Application Support" / "MVP Baseball 2005 Recomp";
#elif defined(_WIN32)
            const char* la = std::getenv("LOCALAPPDATA");
            dir = fs::path(la ? la : ".") / "MVP Baseball 2005 Recomp";
#else
            const char* x = std::getenv("XDG_DATA_HOME"); const char* home = std::getenv("HOME");
            dir = x && *x ? fs::path(x) / "mvp2005-recomp" : fs::path(home ? home : ".") / ".local" / "share" / "mvp2005-recomp";
#endif
#if !defined(__APPLE__)
            if (!is_game_folder(dir)) {                                     // first start of this version: an older one next to it?
                fs::path old = older_install(prog);
                if (!old.empty() && !move_player_data(old, dir)) dir = old;
            }
#endif
        }
    }
    fs::create_directories(dir, ec);
    fs::current_path(dir, ec);
    if (ec) std::fprintf(stderr, "game folder %s: %s\n", dir.string().c_str(), ec.message().c_str());
    // seed files that ship with the program (the app bundle's Resources/seed, or seed/ next to the program)
    for (fs::path seed : {prog / "seed", prog / ".." / "Resources" / "seed"}) {
        if (!fs::is_directory(seed, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(seed, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            fs::path rel = fs::relative(it->path(), seed, ec), dst = fs::current_path(ec) / rel;
            if (it->is_directory(ec)) { fs::create_directories(dst, ec); continue; }
            if (!fs::exists(dst, ec)) fs::copy_file(it->path(), dst, ec);
        }
    }
#ifdef _WIN32
    if (!GetConsoleWindow() && !std::getenv("MVP_STDERR")) std::freopen("mvp2005.log", "w", stderr);   // a GUI program: the log goes to a file
#endif
    std::fprintf(stderr, "game folder: %s\n", fs::current_path(ec).string().c_str());
}
