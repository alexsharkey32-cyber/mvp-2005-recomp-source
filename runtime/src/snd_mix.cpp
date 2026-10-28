// EA SND driver ticks + audio output.
//
// The real SNDDRV.IRX runs off the SPU2's 48 kHz output: every 512 output samples (10.67 ms, 93.75 Hz) it
// signals the EE's SND thread (user SIF cmd 1, type 0, opt = IOP buffer), and the EE answers by SIF-DMAing
// a 0xAF0-byte command block to that buffer (built by 0x5123B0 / 0x5128F8). EA's mixer runs on the EE; the
// block carries its output. Block layout (offsets from the block start):
//   0x00  header {0, size, ...}
//   0x10  section 3: {3, size} + register/voice state (size bytes from +8)
//   ....  section 2 (optional): {2, size, -, -} + data
//   ....  section 1: {1, size, -, -} + PCM: channel buffers of n s16 samples each, planar, right then left
//         (n = rate*512/48000)
//   ....  section 4 (optional, 2 KB)
//   ....  section 5: {5, tick}, section 6: end
// We pace the ticks by the audio device's queue (so sound never drifts or crackles) and queue the PCM.
// MVP_SNDDUMP=dir writes blocks to disk (every MVP_SNDDUMP_EVERY-th, from tick MVP_SNDDUMP_FROM);
// MVP_WAVOUT=file.wav records the output.
#include "ee_runtime.h"
#include "iop.h"
#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#ifdef MVP_SDL
#include <SDL.h>
#endif

u64 host_time_ns();
int kernel_turbo();
extern bool g_fast_forward;
void iop_send_user_cmd(u32 cid, const u32* payload, u32 nwords, u32 opt);

static const int OUT_RATE = 48000, TICK_SAMPLES = 512;
static const u64 TICK_NS = 1000000000ull * TICK_SAMPLES / OUT_RATE;    // 10.67 ms
static const u32 MARK = 0x5eadbeefu;

static const char* s_dump = std::getenv("MVP_SNDDUMP");
static bool s_started = false;
static u32 s_buf[2] = {0x1f0000, 0x1f1000};
static u32 s_tick = 0;               // ticks sent
static bool s_waiting = false;       // a tick is out and its block hasn't arrived yet
static u64 s_sent_ns = 0, s_next_ns = 0;
static u32 s_blocks = 0, s_dropped = 0;
static FILE* s_wav = nullptr; static u32 s_wav_bytes = 0;
#ifdef MVP_SDL
static SDL_AudioDeviceID s_dev = 0;
#endif
static int s_dev_ok = 0;             // 1 = audio device open

static void wav_header(FILE* f, u32 bytes) {
    u32 h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, (u32)OUT_RATE, (u32)OUT_RATE * 4, 0x00100004, 0x61746164, bytes};
    fseek(f, 0, SEEK_SET); fwrite(h, 4, 11, f);
}

void snd_audio_init() {
    if (const char* w = std::getenv("MVP_WAVOUT")) { s_wav = fopen(w, "wb"); if (s_wav) wav_header(s_wav, 0); }
#ifdef MVP_SDL
    extern bool g_headless;
    if (std::getenv("MVP_NO_AUDIO") || g_headless) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { rlog("audio", "SDL audio init failed: %s", SDL_GetError()); return; }
    SDL_AudioSpec want = {}, have = {};
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!s_dev) { rlog("audio", "no audio device: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(s_dev, 0);
    s_dev_ok = 1;
    rlog("audio", "output %d Hz stereo, buffer %d", have.freq, have.samples);
#endif
}
void snd_audio_pause(bool p) {          // the window went to the background (Pause in the background)
#ifdef MVP_SDL
    if (s_dev) SDL_PauseAudioDevice(s_dev, p ? 1 : 0);
#else
    (void)p;
#endif
}
void snd_audio_shutdown() {
    if (const char* d = std::getenv("MVP_SPUDUMP")) { extern std::vector<u8> g_spu_ram; if (FILE* f = fopen(d, "wb")) { fwrite(g_spu_ram.data(), 1, g_spu_ram.size(), f); fclose(f); } }
    if (s_wav) { wav_header(s_wav, s_wav_bytes); fclose(s_wav); s_wav = nullptr; }
#ifdef MVP_SDL
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; s_dev_ok = 0; }
#endif
}

void snd_driver_started() {
    s_started = true;
    // the driver's silent loop at 0x5000 (voices end there): one block with loop start + repeat + end
    extern std::vector<u8> g_spu_ram;
    std::memset(&g_spu_ram[0x5000], 0, 16); g_spu_ram[0x5000 + 1] = 7;
}

// bytes of audio waiting in the device (-1 = no device)
static int queued_bytes() {
#ifdef MVP_SDL
    if (s_dev_ok) return (int)SDL_GetQueuedAudioSize(s_dev);
#endif
    return -1;
}

static void output(const s16* lr, int frames) {
    static std::vector<s16> tmp;
    float vol = g_settings.volume / 100.0f;
    if (g_settings.mute) vol = 0;
    tmp.assign(lr, lr + frames * 2);
    if (vol != 1.0f) for (s16& s : tmp) s = (s16)std::clamp((int)(s * vol), -32768, 32767);
    if (s_wav) { fwrite(tmp.data(), 2, tmp.size(), s_wav); s_wav_bytes += (u32)tmp.size() * 2; }
#ifdef MVP_SDL
    if (s_dev_ok && !g_fast_forward && kernel_turbo() == 1) SDL_QueueAudio(s_dev, tmp.data(), (u32)tmp.size() * 2);
#endif
}


// ---------------------------------------------------------------- SPU2 voices
// Section 3 of the block mirrors the driver's view of the 48 hardware voices (0x86F0 in SNDDRV.IRX):
//   +0 {u32 VMIXEL, u32 VMIXER}  +8 {u32 VMIXL, u32 VMIXR} (48-bit masks: low 24 bits core 0, next 24 core 1)
//   +16 misc  +32 48 x {u32 start address (low 23 bits; 0 = key off), u16 pitch, s8 volL, s8 volR}
// A new address keys the voice on (SSA = address, LSA = 0x5000 = a silent loop, ADSR1 0x000F, ADSR2 0x0005);
// if the voice is still sounding it is keyed off first and keyed on at the next tick, as the driver does.
extern std::vector<u8> g_spu_ram;
struct Voice {
    u32 ssa = 0, lsa = 0, cur = 0;      // byte addresses; cur = current ADPCM block
    u32 pos = 0;                        // 12-bit fractional sample position inside the decoded block (units 1/4096)
    int nib = 28;                       // samples decoded in blk[]
    s16 blk[28] = {}; s16 hist[4] = {}; // decoded block, last 4 output samples (for interpolation)
    s32 p1 = 0, p2 = 0;                 // ADPCM predictor history
    u16 pitch = 0x1000; float vl = 0, vr = 0;
    int env = 0; int phase = 0;         // 0 off, 1 attack, 2 sustain, 3 release
    u32 cached_addr = 0; u16 cached_pitch = 0; s8 cached_l = 0, cached_r = 0;
    bool silent_loop = false;
};
static Voice s_v[48];
static u64 s_dry_mask = ~0ull;
static const u32 SILENT = 0x5000;

static float vol_of(s8 v) { return v >= 0 ? v * 129 / 16384.0f : -(((-v) * 129) / 16384.0f); }

static void decode_block(Voice& v) {
    static const int F0[5] = {0, 60, 115, 98, 122}, F1[5] = {0, 0, -52, -55, -60};
    const u8* b = &g_spu_ram[v.cur & 0x1ffff0];
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7; if (filt > 4) filt = 0; if (shift > 12) shift = 9;
    u8 flags = b[1];
    if (flags & 4) v.lsa = v.cur;                         // loop start
    for (int i = 0; i < 28; i++) {
        int n = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        s32 s = (s32)(s16)(n << 12) >> shift;
        s += (v.p1 * F0[filt] + v.p2 * F1[filt] + 32) >> 6;
        s = std::clamp(s, -32768, 32767);
        v.p2 = v.p1; v.p1 = s; v.blk[i] = (s16)s;
    }
    v.silent_loop = (v.cur & ~0x3fu) == SILENT;
    if (flags & 1) {                                      // loop end: jump to LSA; no repeat bit = voice ends
        v.cur = v.lsa;
        if (!(flags & 2)) { v.phase = 0; v.env = 0; }
    } else v.cur += 16;
}

static void key_on(Voice& v) {
    v.cur = v.ssa; v.lsa = SILENT; v.pos = 0; v.p1 = v.p2 = 0; std::memset(v.hist, 0, sizeof v.hist);
    v.phase = 1; v.env = 0; v.nib = 0; decode_block(v);
}

static void spu_section(const u8* d) {
    u32 w[4]; std::memcpy(w, d, 16);
    s_dry_mask = (u64)w[2] | ((u64)(w[3] & 0xffffff) << 32);
    u64 kon = 0, koff = 0;
    for (int i = 0; i < 48; i++) {
        Voice& v = s_v[i];
        const u8* e = d + 32 + i * 8;
        u32 addr; std::memcpy(&addr, e, 4); u16 pitch; std::memcpy(&pitch, e + 4, 2);
        s8 vl = (s8)e[6], vr = (s8)e[7];
        if (pitch != v.cached_pitch) { v.pitch = pitch; v.cached_pitch = pitch; }
        if (vl != v.cached_l) { v.vl = vol_of(vl); v.cached_l = vl; }
        if (vr != v.cached_r) { v.vr = vol_of(vr); v.cached_r = vr; }
        if (addr != v.cached_addr) {
            u32 a = addr & 0x7fffff;
            if (a) {
                if (v.phase != 0 && v.env > 0) { koff |= 1ull << i; continue; }   // still sounding: off now, on next tick
                v.ssa = a; v.vl = vol_of(vl); v.vr = vol_of(vr);
                kon |= 1ull << i;
            } else koff |= 1ull << i;
            v.cached_addr = addr;
        }
    }
    for (int i = 0; i < 48; i++) {
        if (kon >> i & 1) rlog("spu-kon", "voice %d addr %06x pitch %04x vol %.3f %.3f", i, s_v[i].ssa, s_v[i].pitch, s_v[i].vl, s_v[i].vr);
        if (kon >> i & 1) key_on(s_v[i]);
        if (koff >> i & 1) if (s_v[i].phase) s_v[i].phase = 3;
    }
}

// renders n output samples of all voices, added to out (stereo interleaved, float)
static void spu_render(float* out, int n) {
    for (int i = 0; i < 48; i++) {
        Voice& v = s_v[i];
        if (!v.phase || !(s_dry_mask >> i & 1)) continue;
        for (int k = 0; k < n; k++) {
            // envelope (the driver's fixed ADSR: near-instant attack, full sustain, fast linear release)
            if (v.phase == 1) { v.env += 14336; if (v.env >= 0x7fff) { v.env = 0x7fff; v.phase = 2; } }
            else if (v.phase == 3) { v.env -= 512; if (v.env <= 0) { v.env = 0; v.phase = 0; break; } }
            // advance
            v.pos += v.pitch > 0x3fff ? 0x3fff : v.pitch;
            while (v.pos >= 4096) {
                v.pos -= 4096;
                v.hist[0] = v.hist[1]; v.hist[1] = v.hist[2]; v.hist[2] = v.hist[3];
                if (v.nib >= 28) { decode_block(v); v.nib = 0; if (!v.phase) break; }
                v.hist[3] = v.blk[v.nib++];
            }
            if (!v.phase) break;
            // cubic (Catmull-Rom) interpolation between hist[1] and hist[2]
            float t = v.pos / 4096.0f, y0 = v.hist[0], y1 = v.hist[1], y2 = v.hist[2], y3 = v.hist[3];
            float s = y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
            if (v.silent_loop) s = 0;
            s *= v.env / 32767.0f;
            out[k * 2] += s * v.vl; out[k * 2 + 1] += s * v.vr;
        }
    }
}

static u64 s_sec_count[8], s_sec_bytes[8]; static u32 s_vmixe_seen = 0, s_misc_seen[4];
void snd_stats() {
    if (!std::getenv("MVP_SNDSTATS")) return;
    fprintf(stderr, "SND sections:"); for (int t = 1; t < 7; t++) fprintf(stderr, " type%d=%llu (%llu bytes)", t, (unsigned long long)s_sec_count[t], (unsigned long long)s_sec_bytes[t]);
    fprintf(stderr, "\nSND VMIXE masks seen: %x, misc words OR: %08x %08x %08x %08x\n", s_vmixe_seen, s_misc_seen[0], s_misc_seen[1], s_misc_seen[2], s_misc_seen[3]);
}
static void process_block(const u8* blk, u32 tick) {
    if (s_dump) {
        static const u32 every = std::getenv("MVP_SNDDUMP_EVERY") ? std::atoi(std::getenv("MVP_SNDDUMP_EVERY")) : 60;
        static const u32 from = std::getenv("MVP_SNDDUMP_FROM") ? std::atoi(std::getenv("MVP_SNDDUMP_FROM")) : 0;
        if (tick % every == 0 && tick >= from) {
            char path[512]; snprintf(path, sizeof path, "%s/snd_%06u.bin", s_dump, tick);
            if (FILE* f = fopen(path, "wb")) { fwrite(blk, 1, 0xaf0, f); fclose(f); }
        }
    }
    static s16 out[TICK_SAMPLES * 2];
    static float mix[TICK_SAMPLES * 2];
    bool have_pcm = false;
    u32 off = 0x10;
    for (int guard = 0; guard < 16 && off + 16 <= 0xaf0; guard++) {
        u32 type, size; std::memcpy(&type, blk + off, 4); std::memcpy(&size, blk + off + 4, 4);
        if (type == 6 || type == 0 || type > 6) break;
        if (type == 5) { off += 16; continue; }
        u32 data = off + (type == 3 ? 8 : 16);
        if (type == 4) size = 0x800;
        s_sec_count[type]++; s_sec_bytes[type] += size;
        if (type == 3 && size >= 32) {
            u32 w[8]; std::memcpy(w, blk + off + 8, 32);
            if (w[0] | w[1]) s_vmixe_seen |= 1;
            for (int k = 0; k < 4; k++) s_misc_seen[k] |= w[4 + k];
            // +16: reverb mode, +17 delay, +18 feedback (the driver passes them to sceSdSetEffectAttr).
            // MVP keeps the mode at 0 (off) everywhere we've looked, so the SPU2 reverb isn't emulated.
            static bool warned = false;
            if ((w[4] & 0xff) && !warned) { warned = true; rlog("snd", "reverb mode %u requested (not emulated)", w[4] & 0xff); }
        }
        if (type == 2 || type == 4) {                // never seen in intro, menus, attract mode or a match
            static bool warned[8];
            if (!warned[type]) { warned[type] = true; rlog("snd", "block section type %u (%u bytes) ignored", type, size); }
        }
        if (type == 3 && size >= 32 + 48 * 8) spu_section(blk + off + 8);
        if (data + size > 0xaf0) break;
        if (type == 1 && size) {
            // planar channels; 2 channels normally (mono is duplicated by the EE). The first buffer is the RIGHT
            // channel: SNDDRV.IRX (0x61e8, tables at 0xd3f8/0xd408) copies the second buffer into the first
            // half of each SPU2 AutoDMA block (left) and the first buffer into the second half (right).
            int nch = 2, n = (int)size / 2 / nch;
            if (n > 0 && n <= 2048) {
                const s16* R = (const s16*)(blk + data); const s16* L = R + n;
                for (int i = 0; i < TICK_SAMPLES; i++) {
                    // resample n -> 512 (n = 512 at 48 kHz; 256/384 for 24/36 kHz mixes)
                    float p = (float)i * n / TICK_SAMPLES; int a = (int)p; float f = p - a; int b = std::min(a + 1, n - 1);
                    out[i * 2] = (s16)(L[a] + (L[b] - L[a]) * f);
                    out[i * 2 + 1] = (s16)(R[a] + (R[b] - R[a]) * f);
                }
                have_pcm = true;
            }
        }
        off = data + size;
        off = (off + 15) & ~15u;
    }
    if (!have_pcm) std::memset(out, 0, sizeof out);
    for (int i = 0; i < TICK_SAMPLES * 2; i++) mix[i] = out[i];
    spu_render(mix, TICK_SAMPLES);
    for (int i = 0; i < TICK_SAMPLES * 2; i++) out[i] = (s16)std::clamp((int)std::lrint(mix[i]), -32768, 32767);
    output(out, TICK_SAMPLES);
}

// Called from the kernel event loop. Sends driver ticks at the audio rate and consumes finished blocks.
bool snd_poll() {
    if (!s_started) return false;
    bool any = false;
    u64 now = host_time_ns();
    if (s_waiting) {
        u32 b = s_buf[(s_tick - 1) & 1];
        u32 w2; std::memcpy(&w2, iop_mem(b) + 8, 4);
        if (w2 != MARK) { s_waiting = false; s_blocks++; process_block(iop_mem(b), s_tick - 1); any = true; }
        else if (now - s_sent_ns < 100000000ull) return false;      // give the EE up to 100 ms
        else { s_waiting = false; s_dropped++; }
    }
    // is another tick due? (asking SDL takes its audio lock: at most every 0.5 ms, a tick is 10.7 ms)
    static u64 s_last_q = 0;
    if (s_dev_ok && !g_fast_forward && kernel_turbo() == 1) {
        if (now - s_last_q < 500000) return any;
        s_last_q = now;
    }
    int q = queued_bytes();
    bool due;
    if (q >= 0 && !g_fast_forward && kernel_turbo() == 1) {
        const int target = TICK_SAMPLES * 4 * 4;                    // keep ~4 ticks (43 ms) queued
        due = q < target;
        if (q == 0 && s_blocks > 8) {
            // underrun (e.g. after a loading stall): refill with a little silence to avoid rapid crackle
#ifdef MVP_SDL
            static s16 z[TICK_SAMPLES * 2]; SDL_QueueAudio(s_dev, z, sizeof z);
#endif
        }
    } else {
        u64 period = TICK_NS / (u64)std::max(1, kernel_turbo());
        if (g_fast_forward) period /= 4;
        if (!s_next_ns || now > s_next_ns + 8 * period) s_next_ns = now;
        due = now >= s_next_ns;
        if (due) s_next_ns += period;
    }
    if (!due) return any;
    u32 b = s_buf[s_tick & 1];
    std::memcpy(iop_mem(b) + 8, &MARK, 4);
    u32 payload[1] = {0};
    iop_send_user_cmd(1, payload, 1, b);
    s_tick++; s_waiting = true; s_sent_ns = now;
    return true;
}
void snd_vsync(u64) {
    static u64 last = 0; u64 t = host_time_ns();
    if (t - last > 5000000000ull) {
        if (last) rlog("audio", "ticks %u blocks %u dropped %u queued %d", s_tick, s_blocks, s_dropped, queued_bytes());
        last = t;
    }
}
const char* snd_status() {
    static char b[96];
    if (s_dev_ok) snprintf(b, sizeof b, "48 kHz stereo, %d ms buffered", std::max(0, queued_bytes()) * 1000 / (OUT_RATE * 4));
    else snprintf(b, sizeof b, "no audio device");
    return b;
}
