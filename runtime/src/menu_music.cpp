// Menu music for the launcher: streams the game's own menu songs straight from the user's disc image
// (DATA/AUDIO/MUSIC/MENUn.ASF: EA "SCHl" stream, blocks of EA-XA ADPCM per channel, 32 kHz stereo) and/or the
// player's imported songs (music/*.asf, the same format) through SDL's audio queue, shuffled, one after another.
// Nothing is copied or shipped; a song is decoded while it plays.
#include "menu_music.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef MVP_SDL
#include <SDL.h>
#include "game_art.h"
#include "music_import.h"
#include <filesystem>

struct Song { std::string file; u64 off = 0, size = 0; std::string label; };   // a disc song (inside the image) or music/x.asf
static std::vector<Song> s_list;
static FILE* s_f = nullptr; static std::string s_f_path;
static u64 s_base = 0, s_pos = 0, s_size = 0;       // current song inside its file
static SDL_AudioDeviceID s_dev = 0;
static int s_rate = 32000, s_ch = 2, s_track = -1;
static float s_gain = 0;                             // smoothed volume

static bool rd(u64 off, void* dst, size_t n) {
    if (!s_f) return false;
#if defined(_WIN32)
    _fseeki64(s_f, (long long)off, SEEK_SET);
#else
    fseeko(s_f, (off_t)off, SEEK_SET);
#endif
    return fread(dst, 1, n, s_f) == n;
}
// EA-XA: 15-byte frames (1 header + 28 nibbles) after two 16-bit history samples
static void decode_channel(const u8* p, size_t n, int count, s16* out, int stride) {
    static const int T[8] = {0, 240, 460, 392, 0, 0, -208, -220};
    if (n < 4) return;
    int h1 = (s16)(p[0] | p[1] << 8), h2 = (s16)(p[2] | p[3] << 8);
    size_t q = 4; int done = 0;
    while (done < count && q + 15 <= n) {
        u8 f = p[q]; int c1 = T[f >> 4 & 3], c2 = T[(f >> 4 & 3) + 4], sh = (f & 15) + 8;
        for (int i = 0; i < 28 && done < count; i++) {
            u8 b = p[q + 1 + i / 2]; int nib = (i & 1) ? (b & 15) : (b >> 4);
            int s = (int)((u32)nib << 28) >> sh;
            s = (s + c1 * h1 + c2 * h2 + 128) >> 8;
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            h2 = h1; h1 = s; out[(size_t)done * stride] = (s16)s; done++;
        }
        q += 15;
    }
}
static bool open_track(int track) {
    if (track < 0 || track >= (int)s_list.size()) return false;
    const Song& sg = s_list[(size_t)track];
    if (!s_f || s_f_path != sg.file) { if (s_f) fclose(s_f); s_f = fopen(sg.file.c_str(), "rb"); s_f_path = sg.file; if (!s_f) return false; }
    const u64 off = sg.off; const u64 size = sg.size;
    if (size < 64) return false;
    u8 h[8];
    if (!rd(off, h, 8) || std::memcmp(h, "SCHl", 4) != 0) return false;
    u32 hs = h[4] | h[5] << 8 | h[6] << 16 | (u32)h[7] << 24;
    if (hs < 12 || hs > 4096) return false;
    std::vector<u8> pt(hs); rd(off, pt.data(), hs);
    int rate = 22050, ch = 1;                                    // "PT" header: tag, length, big-endian value
    for (size_t i = 12; i + 2 < hs;) {
        u8 tag = pt[i++]; if (tag == 0xff) break;
        if (tag == 0xfc || tag == 0xfd || tag == 0xfe) continue;
        u8 len = pt[i++]; u32 v = 0; for (int k = 0; k < len && i < hs; k++) v = v << 8 | pt[i++];
        if (tag == 0x82) ch = (int)v; else if (tag == 0x84) rate = (int)v;
    }
    if (ch < 1 || ch > 2 || rate < 8000 || rate > 48000) return false;
    s_base = off; s_size = size; s_pos = hs; s_rate = rate; s_ch = ch; s_track = track;
    return true;
}
// decodes blocks until `want` more sample frames are queued
static void pump(float volume) {
    if (!s_dev || !s_f) return;
    const u32 bytes_per_frame = 2 * s_ch;
    int guard = 0;
    while (SDL_GetQueuedAudioSize(s_dev) / bytes_per_frame < (u32)s_rate * 6 / 10 && guard++ < 64) {
        u8 h[8];
        if (s_pos + 8 > s_size || !rd(s_base + s_pos, h, 8)) { s_pos = s_size; }
        u32 bs = s_pos < s_size ? (h[4] | h[5] << 8 | h[6] << 16 | (u32)h[7] << 24) : 0;
        if (s_pos >= s_size || bs < 8 || !std::memcmp(h, "SCEl", 4)) {          // end of the song: the next one
            const int n = (int)s_list.size(); int next = s_track; bool ok = false;
            for (int k = 0; k < n && !ok; k++) { next = (next + 1) % n; ok = open_track(next); }
            if (!ok) return;
            continue;
        }
        if (!std::memcmp(h, "SCDl", 4) && bs > 8u + 4 + 4 * s_ch && bs < (1u << 20)) {
            std::vector<u8> blk(bs); rd(s_base + s_pos, blk.data(), bs);
            u32 n = blk[8] | blk[9] << 8 | blk[10] << 16 | (u32)blk[11] << 24;
            if (n > 0 && n < 65536) {
                std::vector<s16> pcm((size_t)n * s_ch, 0);
                const u32 data = 12 + 4 * s_ch;
                for (int c = 0; c < s_ch; c++) {
                    u32 o = blk[12 + c * 4] | blk[13 + c * 4] << 8 | blk[14 + c * 4] << 16 | (u32)blk[15 + c * 4] << 24;
                    if (data + o < bs) decode_channel(&blk[data + o], bs - data - o, (int)n, pcm.data() + c, s_ch);
                }
                for (size_t i = 0; i < pcm.size(); i++) {
                    if ((i % s_ch) == 0) s_gain += (volume - s_gain) * 0.0005f;       // no clicks when the volume changes
                    pcm[i] = (s16)(pcm[i] * s_gain);
                }
                SDL_QueueAudio(s_dev, pcm.data(), (u32)(pcm.size() * 2));
            }
        }
        s_pos += bs;
    }
}

bool menu_music_start(const char* iso_path, float volume, int mode) {
    if (s_dev) return true;
    if (std::getenv("MVP_NO_AUDIO")) return false;
    s_list.clear();
    std::vector<MusicTrack> own; if (mode != 0) own = music_tracks();
    if ((mode != 1 || own.empty()) && iso_path && iso_path[0])                 // the disc's nine songs (needs game_art_open())
        for (int t = 1; t <= 9; t++) {
            char path[64]; snprintf(path, sizeof path, "DATA/AUDIO/MUSIC/MENU%d.ASF", t);
            Song sg; u32 size = 0; if (!game_art_file(path, sg.off, size)) continue;
            sg.size = size; sg.file = iso_path; sg.label = "Menu song " + std::to_string(t) + " from your disc";
            s_list.push_back(std::move(sg));
        }
    for (const MusicTrack& t : own) {
        Song sg; sg.file = t.path; std::error_code ec; sg.size = (u64)std::filesystem::file_size(t.path, ec); if (ec) continue;
        sg.label = t.artist == "My music" ? t.title : t.artist + " - " + t.title;
        s_list.push_back(std::move(sg));
    }
    if (s_list.empty()) return false;
    for (size_t i = s_list.size(); i > 1; i--) std::swap(s_list[i - 1], s_list[(size_t)(SDL_GetPerformanceCounter() >> 3) % i]);   // shuffle
    bool ok = false;
    for (int k = 0; k < (int)s_list.size() && !ok; k++) ok = open_track(k);
    if (ok && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = {}, have = {};
        want.freq = s_rate; want.format = AUDIO_S16LSB; want.channels = (Uint8)s_ch; want.samples = 1024;
        s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    }
    if (!s_dev) { if (s_f) fclose(s_f); s_f = nullptr; s_f_path.clear(); return false; }
    s_gain = 0; pump(volume);
    SDL_PauseAudioDevice(s_dev, 0);
    return true;
}
void menu_music_update(float volume) { pump(volume); }
void menu_music_stop() {
    if (s_dev) { SDL_ClearQueuedAudio(s_dev); SDL_CloseAudioDevice(s_dev); s_dev = 0; SDL_QuitSubSystem(SDL_INIT_AUDIO); }
    if (s_f) { fclose(s_f); s_f = nullptr; } s_f_path.clear();
}
bool menu_music_playing() { return s_dev != 0; }
std::string menu_music_label() { return s_dev && s_track >= 0 && s_track < (int)s_list.size() ? s_list[(size_t)s_track].label : std::string(); }
#else
bool menu_music_start(const char*, float, int) { return false; }
void menu_music_update(float) {}
void menu_music_stop() {}
bool menu_music_playing() { return false; }
std::string menu_music_label() { return std::string(); }
#endif
