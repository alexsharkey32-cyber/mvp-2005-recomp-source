// Custom music. The game's menu soundtrack is nine files on the disc (DATA/AUDIO/MUSIC/MENU1-9.ASF): EA "SCHl"
// streams of EA-XA ADPCM, 32 kHz stereo, in blocks of 1/15 s, which the game reads and decodes itself. Importing
// a song converts it to exactly that format (music/<name>.asf); at start the runtime serves the converted files
// in place of the disc's (like any other mod overlay) and writes the song names into the game's string table,
// so the game's own "EA SPORTS TRAX" display and jukebox show them.
//
// WAV files are read directly. Everything else (MP3, AAC/M4A, FLAC, AIFF, OGG ...) goes through a converter that
// is already on the computer: afconvert on macOS (part of the system), ffmpeg elsewhere if it is installed.
#include "music_import.h"
#include "loc_patch.h"
#include "iop.h"
#include "ee_runtime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
namespace fs = std::filesystem;
static const int kRate = 32000;
static const char* kDir = "music";

// ---------------------------------------------------------------- reading audio
static u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
static bool read_file(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f); return ok;
}
// RIFF/WAVE, PCM 8/16/24/32-bit or 32/64-bit float, any rate, any channel count -> stereo float
static bool read_wav(const std::string& path, std::vector<float>& st, int& rate) {
    std::vector<u8> d;
    if (!read_file(path, d) || d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, ch = 0, bits = 0, align = 0; rate = 0;
    const u8* data = nullptr; size_t dlen = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        u32 sz = rd32(&d[p + 4]); const u8* body = &d[p + 8];
        size_t avail = std::min<size_t>(sz, d.size() - p - 8);
        if (!std::memcmp(&d[p], "fmt ", 4) && avail >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); rate = (int)rd32(body + 4); align = rd16(body + 12); bits = rd16(body + 14);
            if (fmt == 0xfffe && avail >= 26) fmt = rd16(body + 24);                  // WAVE_FORMAT_EXTENSIBLE: sub-format
        } else if (!std::memcmp(&d[p], "data", 4)) { data = body; dlen = avail; break; }
        p += 8 + (size_t)sz + (sz & 1);
    }
    if (!data || ch < 1 || ch > 16 || rate < 4000 || rate > 400000 || (fmt != 1 && fmt != 3)) return false;
    int bytes = bits / 8; if (bytes < 1 || bytes > 8 || align < bytes * ch) align = bytes * ch;
    size_t frames = dlen / (size_t)align;
    st.resize(frames * 2);
    auto sample = [&](const u8* p) -> float {
        if (fmt == 3) { if (bytes == 4) { float v; std::memcpy(&v, p, 4); return v; } if (bytes == 8) { double v; std::memcpy(&v, p, 8); return (float)v; } return 0; }
        switch (bytes) {
        case 1: return (p[0] - 128) / 128.0f;
        case 2: return (s16)rd16(p) / 32768.0f;
        case 3: return (float)((s32)(p[0] << 8 | p[1] << 16 | (u32)p[2] << 24) >> 8) / 8388608.0f;
        case 4: return (float)((s32)rd32(p) / 2147483648.0);
        }
        return 0;
    };
    for (size_t i = 0; i < frames; i++) {
        const u8* p = data + i * (size_t)align;
        float l = sample(p), r = ch >= 2 ? sample(p + bytes) : l;
        st[i * 2] = l; st[i * 2 + 1] = r;
    }
    return frames > 0;
}

static std::string shell_quote(const std::string& s) {
#if defined(_WIN32)
    std::string o = "\""; for (char c : s) { if (c == '"') o += '\\'; o += c; } return o + "\"";
#else
    std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'";
#endif
}
// decodes anything the system's converter understands into a temporary 32 kHz stereo WAV
static bool s_have_ffmpeg = false;          // set by convert_external: was ffmpeg there to try?
static bool convert_external(const std::string& src, const std::string& tmp_wav) {
    std::vector<std::string> cmds;
#if defined(_WIN32)
    s_have_ffmpeg = std::system("ffmpeg -version >NUL 2>&1") == 0;
#else
    s_have_ffmpeg = std::system("ffmpeg -version >/dev/null 2>&1") == 0;
#endif
#if defined(__APPLE__)
    cmds.push_back("afconvert -f WAVE -d LEI16@32000 -c 2 " + shell_quote(src) + " " + shell_quote(tmp_wav) + " >/dev/null 2>&1");
#endif
#if defined(_WIN32)
    cmds.push_back("ffmpeg -v error -y -i " + shell_quote(src) + " -vn -ar 32000 -ac 2 -c:a pcm_s16le " + shell_quote(tmp_wav) + " >NUL 2>&1");
#else
    cmds.push_back("ffmpeg -v error -y -i " + shell_quote(src) + " -vn -ar 32000 -ac 2 -c:a pcm_s16le " + shell_quote(tmp_wav) + " >/dev/null 2>&1");
#endif
    for (const std::string& c : cmds) {
        std::error_code ec; fs::remove(tmp_wav, ec);
        if (std::system(c.c_str()) == 0 && fs::exists(tmp_wav, ec) && fs::file_size(tmp_wav, ec) > 44) return true;
    }
    return false;
}

// ---------------------------------------------------------------- resampling (windowed sinc, any ratio)
static void resample(const std::vector<float>& in, int in_rate, std::vector<float>& out, int out_rate) {
    if (in_rate == out_rate) { out = in; return; }
    const size_t nin = in.size() / 2;
    const double step = (double)in_rate / out_rate;                    // input samples per output sample
    const double fc = std::min(1.0, (double)out_rate / in_rate) * 0.94;  // cutoff relative to the input Nyquist
    const int zc = 16;                                                 // zero crossings on each side
    const int half = (int)std::ceil(zc / fc);                          // taps on each side
    const int phases = 512;
    std::vector<float> tab((size_t)(phases + 1) * (2 * half));        // kernel for fractional offsets 0..1
    for (int ph = 0; ph <= phases; ph++) {
        double frac = (double)ph / phases, sum = 0;
        for (int k = 0; k < 2 * half; k++) {
            double x = (k - half + 1) - frac;                          // distance from the output position, in input samples
            double t = x / half;                                       // -1..1 across the window
            double w = std::fabs(t) >= 1 ? 0 : 0.42 + 0.5 * std::cos(M_PI * t) + 0.08 * std::cos(2 * M_PI * t);    // Blackman
            double s = x == 0 ? 1 : std::sin(M_PI * fc * x) / (M_PI * fc * x);
            double v = fc * s * w; tab[(size_t)ph * (2 * half) + k] = (float)v; sum += v;
        }
        for (int k = 0; k < 2 * half; k++) tab[(size_t)ph * (2 * half) + k] /= (float)sum;        // unity gain at DC
    }
    const size_t nout = (size_t)((double)nin / step);
    out.assign(nout * 2, 0.0f);
    for (size_t i = 0; i < nout; i++) {
        double pos = i * step; long long ip = (long long)pos; double frac = pos - ip;
        double fp = frac * phases; int ph = std::min(phases - 1, (int)fp); const float mix = (float)(fp - ph);
        const float* k0 = &tab[(size_t)ph * (2 * half)]; const float* k1 = k0 + 2 * half;       // between two table rows
        float l = 0, r = 0;
        long long base = ip - half + 1;
        for (int t = 0; t < 2 * half; t++) {
            long long j = base + t; if (j < 0 || j >= (long long)nin) continue;
            const float k = k0[t] + (k1[t] - k0[t]) * mix;
            l += in[(size_t)j * 2] * k; r += in[(size_t)j * 2 + 1] * k;
        }
        out[i * 2] = l; out[i * 2 + 1] = r;
    }
}

// ---------------------------------------------------------------- EA-XA encoder
// A frame is one header byte (filter << 4 | shift) and 28 4-bit residuals. The decoder computes
//   s = ((nibble << (12 - shift)) * 256 + c1 * h1 + c2 * h2 + 128) >> 8
// so the encoder tries the four predictors, and for each the shifts around the one its largest residual needs,
// running the decoder in the loop; the combination with the least squared error wins.
static const int kC1[4] = {0, 240, 460, 392}, kC2[4] = {0, 0, -208, -220};
static inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
struct XaState { int h1 = 0, h2 = 0; };
static void xa_encode_frame(const s16* in, int n, XaState& st, u8* out) {      // n <= 28 valid samples (rest = 0)
    s16 x[28]; for (int i = 0; i < 28; i++) x[i] = i < n ? in[i] : 0;
    double best = 1e300; u8 best_bytes[15] = {0}; XaState best_st = st;
    for (int f = 0; f < 4; f++) {
        // open-loop residual range -> the shift this predictor needs
        int h1 = st.h1, h2 = st.h2, peak = 0;
        for (int i = 0; i < 28; i++) { int pred = (kC1[f] * h1 + kC2[f] * h2 + 128) >> 8; int r = x[i] - pred; peak = std::max(peak, std::abs(r)); h2 = h1; h1 = x[i]; }
        int need = 0; while (need < 12 && (peak >> need) > 7) need++;            // residual = nibble << need
        for (int sh2 = std::max(0, need - 1); sh2 <= std::min(12, need + 1); sh2++) {
            int shift = 12 - sh2;                                                // header value
            u8 bytes[15]; std::memset(bytes, 0, sizeof bytes); bytes[0] = (u8)(f << 4 | shift);
            int a1 = st.h1, a2 = st.h2; double err = 0;
            for (int i = 0; i < 28; i++) {
                int pred = kC1[f] * a1 + kC2[f] * a2;                            // << 8
                int target = ((int)x[i] << 8) - pred - 128;
                int q = (target + (1 << (sh2 + 7))) >> (sh2 + 8);                // nearest nibble
                q = q < -8 ? -8 : q > 7 ? 7 : q;
                int bestq = q; int bests = 0; double beste = 1e300;
                for (int dq = -1; dq <= 1; dq++) {                               // rounding and clamping interact: check the neighbours
                    int qq = q + dq; if (qq < -8 || qq > 7) continue;
                    int s = clamp16(((qq << sh2) * 256 + pred + 128) >> 8);
                    double e = (double)(s - x[i]) * (s - x[i]);
                    if (e < beste) { beste = e; bestq = qq; bests = s; }
                }
                err += beste; a2 = a1; a1 = bests;
                if (i & 1) bytes[1 + i / 2] |= (u8)(bestq & 15); else bytes[1 + i / 2] |= (u8)((bestq & 15) << 4);
            }
            if (err < best) { best = err; std::memcpy(best_bytes, bytes, 15); best_st.h1 = a1; best_st.h2 = a2; }
        }
    }
    std::memcpy(out, best_bytes, 15); st = best_st;
}
static void put32(std::vector<u8>& v, u32 x) { v.push_back((u8)x); v.push_back((u8)(x >> 8)); v.push_back((u8)(x >> 16)); v.push_back((u8)(x >> 24)); }
// stereo s16 at 32 kHz -> the complete .ASF
static std::vector<u8> asf_build(const std::vector<s16>& pcm) {
    const size_t total = pcm.size() / 2;
    std::vector<u8> out;
    // SCHl: the same "PT" header as the disc's songs, with this song's sample count
    const bool wide = total >= (1u << 24);                                       // more than 8.7 minutes: a 4-byte count
    const u8 h1[] = {'S', 'C', 'H', 'l', 0x2c, 0, 0, 0, 'P', 'T', 0x05, 0x00, 0x06, 0x01, 0x65, 0x0b, 0x01, 0x02, 0xfd, 0x80, 0x01, 0x02, 0x85};
    const u8 h2[] = {0x82, 0x01, 0x02, 0x84, 0x02, 0x7d, 0x00, 0xa0, 0x01, 0x0a, 0x8c, 0x01, 0x04, 0xff};
    out.insert(out.end(), h1, h1 + sizeof h1);
    out.push_back(wide ? 4 : 3); if (wide) out.push_back((u8)(total >> 24));
    out.push_back((u8)(total >> 16)); out.push_back((u8)(total >> 8)); out.push_back((u8)total);
    out.insert(out.end(), h2, h2 + sizeof h2);
    while (out.size() < 0x2c) out.push_back(0);
    size_t cc = out.size(); out.insert(out.end(), {'S', 'C', 'C', 'l', 12, 0, 0, 0, 0, 0, 0, 0});
    XaState st[2]; std::vector<s16> chan(28);
    size_t done = 0; u32 blocks = 0;
    const double per_block = kRate / 15.0;
    while (done < total) {
        blocks++;
        size_t end = (size_t)std::ceil(blocks * per_block / 28.0) * 28;           // blocks end on frame boundaries just past k/15 s
        if (end > total) end = total;
        const u32 n = (u32)(end - done), frames = (n + 27) / 28;
        const u32 chsize = (4 + frames * 15 + 3) & ~3u;
        const u32 bs = 8 + 4 + 8 + 2 * chsize;
        size_t at = out.size(); out.resize(at + bs, 0);
        std::memcpy(&out[at], "SCDl", 4);
        auto w32 = [&](size_t o, u32 v) { out[o] = (u8)v; out[o + 1] = (u8)(v >> 8); out[o + 2] = (u8)(v >> 16); out[o + 3] = (u8)(v >> 24); };
        w32(at + 4, bs); w32(at + 8, n); w32(at + 12, 0); w32(at + 16, chsize);
        for (int c = 0; c < 2; c++) {
            u8* p = &out[at + 20 + c * chsize];
            p[0] = (u8)st[c].h1; p[1] = (u8)(st[c].h1 >> 8); p[2] = (u8)st[c].h2; p[3] = (u8)(st[c].h2 >> 8);
            for (u32 f = 0; f < frames; f++) {
                int cnt = (int)std::min<u32>(28, n - f * 28);
                for (int i = 0; i < cnt; i++) chan[i] = pcm[(done + f * 28 + i) * 2 + c];
                xa_encode_frame(chan.data(), cnt, st[c], p + 4 + f * 15);
            }
        }
        done = end;
    }
    out[cc + 8] = (u8)blocks; out[cc + 9] = (u8)(blocks >> 8); out[cc + 10] = (u8)(blocks >> 16); out[cc + 11] = (u8)(blocks >> 24);
    out.insert(out.end(), {'S', 'C', 'E', 'l', 8, 0, 0, 0});
    return out;
}

// ---------------------------------------------------------------- decoding (tests, track length)
static void xa_decode_channel(const u8* p, size_t n, int count, s16* out, int stride) {
    if (n < 4) return;
    int h1 = (s16)rd16(p), h2 = (s16)rd16(p + 2);
    size_t q = 4; int done = 0;
    while (done < count && q + 15 <= n) {
        u8 f = p[q]; int c1 = kC1[f >> 4 & 3], c2 = kC2[f >> 4 & 3], sh = (f & 15) + 8;
        for (int i = 0; i < 28 && done < count; i++) {
            u8 b = p[q + 1 + i / 2]; int nib = (i & 1) ? (b & 15) : (b >> 4);
            int s = (int)((u32)nib << 28) >> sh;
            s = clamp16((s + c1 * h1 + c2 * h2 + 128) >> 8);
            h2 = h1; h1 = s; out[(size_t)done * stride] = (s16)s; done++;
        }
        q += 15;
    }
}
static bool asf_info(const std::vector<u8>& d, size_t& hdr, u32& samples, int& rate, int& ch) {
    if (d.size() < 16 || std::memcmp(d.data(), "SCHl", 4)) return false;
    hdr = rd32(&d[4]); if (hdr < 12 || hdr > d.size()) return false;
    rate = 22050; ch = 1; samples = 0;
    for (size_t i = 12; i + 2 < hdr;) {
        u8 tag = d[i++]; if (tag == 0xff) break;
        if (tag == 0xfc || tag == 0xfd || tag == 0xfe) continue;
        u8 len = d[i++]; u32 v = 0; for (int k = 0; k < len && i < hdr; k++) v = v << 8 | d[i++];
        if (tag == 0x82) ch = (int)v; else if (tag == 0x84) rate = (int)v; else if (tag == 0x85) samples = v;
    }
    return ch >= 1 && ch <= 2 && rate >= 8000 && rate <= 48000;
}
bool music_decode_asf(const std::string& path, std::vector<s16>& pcm, int& rate, int& ch) {
    std::vector<u8> d; size_t pos; u32 samples;
    if (!read_file(path, d) || !asf_info(d, pos, samples, rate, ch)) return false;
    pcm.clear();
    while (pos + 8 <= d.size()) {
        u32 bs = rd32(&d[pos + 4]); if (bs < 8 || pos + bs > d.size()) break;
        if (!std::memcmp(&d[pos], "SCDl", 4)) {
            u32 n = rd32(&d[pos + 8]); size_t at = pcm.size(); pcm.resize(at + (size_t)n * ch);
            for (int c = 0; c < ch; c++) { u32 o = rd32(&d[pos + 12 + c * 4]); const u32 data = 12 + 4 * ch;
                                           if (data + o < bs) xa_decode_channel(&d[pos + data + o], bs - data - o, (int)n, pcm.data() + at + c, ch); }
        } else if (!std::memcmp(&d[pos], "SCEl", 4)) break;
        pos += bs;
    }
    return !pcm.empty();
}
bool music_write_wav(const std::string& path, const std::vector<s16>& pcm, int rate, int ch) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    std::vector<u8> h; u32 bytes = (u32)(pcm.size() * 2);
    h.insert(h.end(), {'R', 'I', 'F', 'F'}); put32(h, 36 + bytes); h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(h, 16);
    h.push_back(1); h.push_back(0); h.push_back((u8)ch); h.push_back(0); put32(h, (u32)rate); put32(h, (u32)(rate * ch * 2));
    h.push_back((u8)(ch * 2)); h.push_back(0); h.push_back(16); h.push_back(0); h.insert(h.end(), {'d', 'a', 't', 'a'}); put32(h, bytes);
    fwrite(h.data(), 1, h.size(), f); fwrite(pcm.data(), 2, pcm.size(), f); fclose(f);
    return true;
}

// ---------------------------------------------------------------- the library (music/)
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool music_is_audio_file(const std::string& path) {
    static const char* const ext[] = {".wav", ".wave", ".mp3", ".m4a", ".aac", ".flac", ".aif", ".aiff", ".aifc", ".caf", ".ogg", ".oga", ".opus", ".wma", ".mp4", ".alac"};
    std::string e = lower(fs::path(path).extension().string());
    for (const char* x : ext) if (e == x) return true;
    return false;
}
static void split_title(const std::string& stem, std::string& artist, std::string& title) {
    size_t d = stem.find(" - ");
    if (d != std::string::npos && d > 0 && d + 3 < stem.size()) { artist = stem.substr(0, d); title = stem.substr(d + 3); }
    else { artist = "My music"; title = stem; }
}
std::vector<MusicTrack> music_tracks() {
    std::vector<MusicTrack> v; std::error_code ec;
    for (auto it = fs::directory_iterator(kDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".asf") continue;
        MusicTrack t; t.path = it->path().string();
        split_title(it->path().stem().string(), t.artist, t.title);
        u8 h[64] = {0}; FILE* f = fopen(t.path.c_str(), "rb"); if (!f) continue;
        size_t got = fread(h, 1, sizeof h, f); fclose(f);
        std::vector<u8> hv(h, h + got); size_t hdr; u32 samples; int rate, ch;
        if (!asf_info(hv, hdr, samples, rate, ch)) continue;
        t.seconds = (double)samples / rate;
        v.push_back(std::move(t));
    }
    std::sort(v.begin(), v.end(), [](const MusicTrack& a, const MusicTrack& b) { return lower(a.path) < lower(b.path); });
    return v;
}
std::string music_import(const std::string& src) {
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return "File not found: " + src;
    std::string stem = fs::path(src).stem().string();
    if (stem.empty()) stem = "song";
    fs::create_directories(kDir, ec);
    const std::string dst = (fs::path(kDir) / (stem + ".asf")).string();
    if (lower(fs::path(src).extension().string()) == ".asf") {                    // already in the game's format
        std::vector<u8> d; size_t hdr; u32 samples; int rate, ch;
        if (!read_file(src, d) || !asf_info(d, hdr, samples, rate, ch) || rate != kRate || ch != 2) return fs::path(src).filename().string() + ": not a 32 kHz stereo EA music file";
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return ec ? "Could not write " + dst : "";
    }
    std::vector<float> in; int rate = 0;
    if (!read_wav(src, in, rate)) {
        const std::string tmp = (fs::path(kDir) / ".import.wav").string();
        bool ok = convert_external(src, tmp) && read_wav(tmp, in, rate);
        fs::remove(tmp, ec);
        if (!ok)
#if defined(__APPLE__)
            return fs::path(src).filename().string() + ": macOS could not read this file (afconvert). MP3, M4A/AAC, FLAC, AIFF and WAV work.";
#else
            return fs::path(src).filename().string() + (s_have_ffmpeg ? ": ffmpeg could not read this file. Is it really an audio file?"
                                                                      : ": only WAV files can be read directly. Install ffmpeg to import MP3, M4A, FLAC or OGG.");
#endif
    }
    std::vector<float> rs; resample(in, rate, rs, kRate);
    in.clear(); in.shrink_to_fit();
    if (rs.size() / 2 < (size_t)kRate) return fs::path(src).filename().string() + ": shorter than one second";
    if (rs.size() / 2 > (size_t)kRate * 60 * 20) rs.resize((size_t)kRate * 60 * 20 * 2);          // 20 minutes at most
    // peak-normalise only when the file would clip; a short fade at both ends avoids clicks
    float peak = 0; for (float v : rs) peak = std::max(peak, std::fabs(v));
    const float gain = peak > 0.999f ? 0.999f / peak : 1.0f;
    const size_t n = rs.size() / 2, fade = (size_t)kRate / 50;
    std::vector<s16> pcm(rs.size());
    for (size_t i = 0; i < n; i++) {
        float g = gain; if (i < fade) g *= (float)i / fade; if (n - 1 - i < fade) g *= (float)(n - 1 - i) / fade;
        for (int c = 0; c < 2; c++) pcm[i * 2 + c] = (s16)clamp16((int)std::lround(rs[i * 2 + c] * g * 32767.0f));
    }
    rs.clear(); rs.shrink_to_fit();
    std::vector<u8> asf = asf_build(pcm);
    FILE* f = fopen(dst.c_str(), "wb");
    if (!f || fwrite(asf.data(), 1, asf.size(), f) != asf.size()) { if (f) fclose(f); return "Could not write " + dst; }
    fclose(f);
    return "";
}
int music_remove_all() {
    int n = 0; std::error_code ec;
    for (const MusicTrack& t : music_tracks()) if (fs::remove(t.path, ec)) n++;
    return n;
}

// ---------------------------------------------------------------- serving it to the game
// The song names are the 27 strings after "EA SPORTS(tm) TRAX" in FEENG.LOC: artist, title, album for each of the
// nine songs (loc_patch.h has the file format).
static bool loc_set_songs(std::vector<u8>& d, const std::vector<int>& slot_track, const std::vector<MusicTrack>& tracks) {
    LocFile loc;
    if (!loc.parse(d)) return false;
    const size_t n = loc.str.size(); int first = -1;
    for (size_t i = 0; i + 28 <= n; i++) if (loc.str[i].size() == 15 && loc.str[i].compare(0, 9, u"EA SPORTS") == 0 && loc.str[i].compare(11, 4, u"TRAX") == 0) { first = (int)i + 1; break; }
    if (first < 0) return false;
    for (size_t slot = 0; slot < slot_track.size() && slot < 9; slot++) {
        if (slot_track[slot] < 0) continue;
        const MusicTrack& t = tracks[(size_t)slot_track[slot]];
        loc.str[first + slot * 3] = loc_utf16(t.artist, 40, true);              // the game's fonts are Latin-1
        loc.str[first + slot * 3 + 1] = loc_utf16(t.title, 48, true);
        loc.str[first + slot * 3 + 2] = u"Your music";
    }
    d = loc.build();
    return true;
}
int music_apply(int mode) {
    if (mode < 1) return 0;
    std::vector<MusicTrack> tracks = music_tracks();
    if (tracks.empty()) return 0;
    std::mt19937 rng((u32)time(nullptr) ^ (u32)host_time_ns());
    std::vector<int> order((size_t)tracks.size()); for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<int> slot(9, -1);
    if (mode == 1) for (int k = 0; k < 9; k++) slot[k] = order[(size_t)k % order.size()];           // fewer than nine songs repeat
    else {                                                                                           // mixed: a share of the nine slots
        int n = (int)tracks.size(), take = std::min(n, (9 * n + n + 8) / (n + 9));
        std::vector<int> slots = {0, 1, 2, 3, 4, 5, 6, 7, 8}; std::shuffle(slots.begin(), slots.end(), rng);
        for (int k = 0; k < take; k++) slot[slots[k]] = order[k];
    }
    int used = 0;
    for (int k = 0; k < 9; k++) {
        if (slot[k] < 0) continue;
        std::vector<u8> d;
        if (!read_file(tracks[(size_t)slot[k]].path, d)) { slot[k] = -1; continue; }
        char path[64]; snprintf(path, sizeof path, "DATA/AUDIO/MUSIC/MENU%d.ASF", k + 1);
        if (iso_add_overlay_data(path, std::move(d), tracks[(size_t)slot[k]].path.c_str())) used++; else slot[k] = -1;
    }
    if (used) {
        std::vector<u8> locf;
        if (iso_file_bytes("DATA/FEENG.LOC", locf) && loc_set_songs(locf, slot, tracks)) iso_add_overlay_data("DATA/FEENG.LOC", std::move(locf), "song names");
        else rlog("music", "song names not written (FEENG.LOC layout not recognised)");
    }
    return used;
}
