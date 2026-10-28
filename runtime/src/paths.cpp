// The game folder: where the settings, memory cards, the game program copied from the disc, the features
// (mods/, textures/hd/) and screenshots live. Every path in the runtime is relative to it, so main() makes it the
// current directory first.
//   1. $MVP_HOME
//   2. the current directory, when it already is one (a source checkout, a portable folder started from a shell)
//   3. the folder of the program, when it is one or is writable and not inside a macOS app bundle (portable:
//      Windows and Linux zips)
//   4. per user: macOS ~/Library/Application Support/MVP Baseball 2005 Recomp, Linux ~/.local/share/mvp2005-recomp,
//      Windows %LOCALAPPDATA%\MVP Baseball 2005 Recomp
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
static bool writable(const fs::path& d) {
    std::error_code ec; fs::path t = d / ".mvp_write_test";
    FILE* f = std::fopen(t.string().c_str(), "w"); if (!f) return false;
    std::fclose(f); fs::remove(t, ec); return true;
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
        if (!bundle && (is_game_folder(prog) || writable(prog))) dir = prog;
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
