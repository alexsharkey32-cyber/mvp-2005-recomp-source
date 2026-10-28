#include <algorithm>
#include "settings.h"
#include <map>
#include <unordered_map>
#include <array>
#include <cmath>
#include <cmath>
#include <array>
#include <cstring>
// Native replacements for guest functions listed in tools/recomp/overrides.txt.
#include "ee_ops.h"
#include "ee_runtime.h"
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <filesystem>

void hle_return(EEContext* c) { c->r[2].UD[0] = 0; }

// ---------------------------------------------------------------- calendar year
// The game's calendar starts in 2005. g_year_shift (first year of the loaded roster's dynasty - 2005) is added to
// every year constant in the game's code (tools/recomp/years.txt: 63 instructions) and to the years its two date
// conversions take and give (tools/recomp/wraps.txt). A date is {u8 month, u8 day, u16 year}; a day number counts from 1/1/1948 and is 16 bits wide in the schedule
// (hence the game's last year, 2125). Both conversions are the game's own code (f_..._orig); only the year is
// moved on the way in and out, so 4/4/2027 is, inside, the day that 4/4/2005 always was.
int g_year_shift = 0;
void f_004934b8_orig(EEContext* c); static void tie_dump(EEContext* c);
void f_00493610_orig(EEContext* c);
void hle_date_to_days(EEContext* c) {                       // a0 = date (the game's code leaves month - 1 in it)
    const u32 p = c->r[4].UW[0]; const int sh = g_year_shift;
    if (!sh || !p) { f_004934b8_orig(c); return; }
    const u16 y = ee_rd<u16>(p + 2);
    const bool moved = y >= 100;                             // below 100 the game adds 1900 itself: left alone
    if (moved) ee_wr<u16>(p + 2, (u16)(y - sh));
    f_004934b8_orig(c);
    if (moved) ee_wr<u16>(p + 2, (u16)(ee_rd<u16>(p + 2) + sh));
}
int g_last_date_year = 0, g_max_date_year = 0, g_last_date_month = 0;   // the dates the game made last / at most (draft classes)
void hle_days_to_date(EEContext* c) {                       // a0 = date out, a1 = day number; returns a0
    const u32 p = c->r[4].UW[0];
    f_00493610_orig(c);
    if (g_year_shift && p) ee_wr<u16>(p + 2, (u16)(ee_rd<u16>(p + 2) + g_year_shift));
    static const int datelog = std::getenv("MVP_DATELOG") ? std::atoi(std::getenv("MVP_DATELOG")) : 0;   // debug: who makes dates of that month (1-12)
    if (datelog && p && ee_rd<u8>(p) == datelog && c->r[31].UW[0] != 0x74fec8) {
        static int n = 0;
        static bool dumped = false;
        if (!dumped && c->r[31].UW[0] == 0x202b4c) {         // the tiebreak message: the last records of the four schedules
            dumped = true; const u32 cal = ee_rd<u32>(0x7d66f8);
            for (int L = 0; L < 4; L++) {
                const u32 sc = ee_rd<u32>(cal + 12 + 4 * L), cnt = ee_rd<u32>(sc + 4), rec = ee_rd<u32>(sc + 8);
                fprintf(stderr, "[sched] level %d: %u records (cap %u) at %08x\n", L, cnt, ee_rd<u32>(sc), rec);
                for (u32 i = cnt > 30 ? cnt - 30 : 0; i < cnt; i++) {
                    fprintf(stderr, "[sched]  %u:", i); for (int k = 0; k < 28; k += 4) fprintf(stderr, " %08x", ee_rd<u32>(rec + 28 * i + k)); fprintf(stderr, "\n"); }
            }
            fprintf(stderr, "[sched] today %u\n", 0u);
        }
        if (n++ < 3000) { fprintf(stderr, "[datelog] %d/%d/%d day %u ra=%08x stack:", ee_rd<u8>(p), ee_rd<u8>(p + 1), ee_rd<u16>(p + 2), c->r[5].UW[0], c->r[31].UW[0]);
            for (int k = 0; k < c->shadow_n && k < 64; k++) fprintf(stderr, " %08x", c->shadow[k]); fprintf(stderr, "\n"); }
    }
    if (p) { g_last_date_month = ee_rd<u8>(p); g_last_date_year = ee_rd<u16>(p + 2); if (g_last_date_year < 2200 && g_last_date_year > g_max_date_year) g_max_date_year = g_last_date_year; }
}

// End of a level's season (func_0070d1f0(schedules, today), at the start of every simulated day): within three
// days after the last game of a level's schedule, and while the level's races are not all decided, the game adds
// one-game playoffs for ties at the top (func_0070e2e0 dates them the day after) and returns 1: the sim stops
// with "%1 one game playoff has been scheduled for %2" (event 7) before the day moves on. When a race is open
// without a tie to play off, nothing is added, the day never moves on and the message comes back forever (seen
// in a 1977 league on 5 September, the minor leagues' last day, and in 2027 on the majors' last day, 3 October).
// The original's answer is kept when it scheduled something; when the four schedules come out as they went in it
// was nothing to stop for.
void f_0070d1f0_orig(EEContext* c); void f_001b6220(EEContext* c);
static u32 sched_signature(u32 mgr) {
    u32 h = 0;
    for (int L = 0; L < 4; L++) {
        const u32 sc = ee_rd<u32>(mgr + 40 + 4 * L); if (sc < 0x100000 || sc >= 0x2000000) continue;
        const u32 n = ee_rd<u32>(sc + 4), rec = ee_rd<u32>(sc + 8); h = h * 31 + n;
        if (rec < 0x100000 || rec >= 0x2000000) continue;
        for (u32 i = n > 32 ? n - 32 : 0; i < n; i++) h = h * 131 + ee_rd<u16>(rec + 28 * i);
    }
    return h;
}
static void tie_dump(EEContext* c) {                       // debug (MVP_TIEDEBUG): the majors' six division leaders and runners-up
                 // the majors' six division leaders and runners-up
                GPR128 sr[32]; FPR sf[32]; std::memcpy(sr, c->r, sizeof sr); std::memcpy(sf, c->f, sizeof sf);
                c->r[4].UD[0] = ee_rd<u32>(0x7d6d60); f_001b6220(c); const u32 S = c->r[2].UW[0];
                for (u32 d = 0; d < 6; d++) {
                    const u32 vt = ee_rd<u32>(S + 3516);
                    c->r[4].UD[0] = S + (s16)ee_rd<u16>(vt + 32); c->r[5].UD[0] = d; ee_call(c, ee_rd<u32>(vt + 36));
                    const u32 D = c->r[2].UW[0];
                    for (u32 k = 0; k < 5; k++) {
                        const u32 vt2 = ee_rd<u32>(D + 16);
                        c->r[4].UD[0] = D + (s16)ee_rd<u16>(vt2 + 168); c->r[5].UD[0] = k; ee_call(c, ee_rd<u32>(vt2 + 172));
                        const u32 e = c->r[2].UW[0];
                        fprintf(stderr, "[tie] div %u #%u @%08x D=%08x:", d, k, e, D); for (int w = 0; w < 9; w++) fprintf(stderr, " %08x", ee_rd<u32>(e + 4 * w));
                        float pct; u32 pw = ee_rd<u32>(e + 16); std::memcpy(&pct, &pw, 4); fprintf(stderr, "  pct %.4f\n", pct);
                    }
                }
                std::memcpy(c->r, sr, sizeof sr); std::memcpy(c->f, sf, sizeof sf);
            }
u32 g_sim_today = 0;                                         // the day the dynasty's calendar last started (0: none yet)
void hle_tiebreak_check(EEContext* c) {
    const u32 mgr = c->r[4].UW[0], today = c->r[5].UW[0];
    g_sim_today = today;
    const u32 before = sched_signature(mgr);
    f_0070d1f0_orig(c);
    if (c->r[2].UW[0] == 1 && sched_signature(mgr) == before) {
        static u32 told = 0xffffffff;
        if (told != today) { told = today; rlog("season", "day %u: a race still open, no tie to play off: the season goes on", today);
            if (std::getenv("MVP_TIEDEBUG")) tie_dump(c);
        }
        c->r[2].UD[0] = 0;
    }
}

// Quality (0..1) of one pitch, behind simulated games and the pitcher ratings the game shows:
// a0 = type (0 = fastball), f12 = movement, f14 = control, f15 = velocity, each as rating / maximum (velocity:
// mph / 101). Fastball: control / 2 + velocity * 1.5 - 0.93; others: (control + movement) / 2 + 0.04.
// The 2005 database's pitchers average 90 mph, today's real ones 94: with real velocities every fastball rates
// 0.06 higher and simulated seasons score 3.6 runs a game. The roster builder writes the constant that puts the
// average big-league fastball back where the game's was (mods/rosters-2026/sim.txt).
float g_fb_base = 0.93f;
void f_0019d2b0_orig(EEContext* c);
void hle_pitch_quality(EEContext* c) {
    if (g_fb_base == 0.93f || c->r[4].UW[0] != 0 || ee_rd<u32>(0x7de764) != 0) { f_0019d2b0_orig(c); return; }
    const float q = (c->f[14].f + c->f[15].f) * 0.5f + c->f[15].f - g_fb_base;
    c->f[0].f = q > 1.0f ? 1.0f : q < 0.0f ? 0.0f : q;
}

// Spread of the ratings the simulation uses. func_00182f10(table, attribute, row) = a rating as value / maximum,
// what the simulated plate appearance reads. With a spread k (mods/<league>/sim.txt: contact_spread,
// power_spread; default 1 = the game's own) a hitter's split contact / power is pulled toward the running league
// mean m: m + (v - m) * k, plus contact_shift / power_shift. The ratings shown on screen are untouched.
float g_contact_spread = 1.0f, g_power_spread = 1.0f, g_contact_shift = 0.0f, g_power_shift = 0.0f;   // + shift: the whole league up / down
void f_00182f10_orig(EEContext* c);
// Base running on hits in simulated games. The game sends a runner by his speed against a fixed line: from
// second on a single only above 0.6 (31% scored, real major leagues about 60%), from first to third above 0.7
// (18%, real about 28%), from first home on a double above 0.8 (10%, real about 40%). With "Simulated games: real
// league" the speed read at those three places comes back as a draw instead: 1 (go) with a chance that grows with
// his speed, else his speed held under the line. Called from func_00714b00 (runner on second) and func_00714cd0
// (runner on first; s1 = 0 single, 1 double); the return address tells which.
extern int g_sim_calibrate;
float g_run_go[3][2] = {{0.38f, 0.65f}, {0.10f, 0.45f}, {0.08f, 0.60f}};   // chance = a + b * speed: 2nd on 1B, 1st to 3rd on 1B, 1st home on 2B
static float run_draw(EEContext* c, float speed) {
    const u32 ra = c->r[31].UW[0];
    int k = -1; float line = 0;
    if (ra == 0x714bd0) { k = 0; line = 0.6f; }
    else if (ra == 0x714dc0) { const u32 a1 = c->r[17].UW[0]; if (a1 == 0) { k = 1; line = 0.7f; } else if (a1 == 1) { k = 2; line = 0.8f; } }
    if (k < 0) return speed;
    static u32 x = 0x9e3779b9u; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const float u = (x >> 8) * (1.0f / 16777216.0f);
    float p = g_run_go[k][0] + g_run_go[k][1] * speed; p = p < 0.02f ? 0.02f : p > 0.97f ? 0.97f : p;
    return u < p ? 1.0f : std::min(speed, line - 0.05f);
}
// Home-run and hit factors (mods/<pack>/hr_factor.txt, tools/eras/hr_factor.py; the hit factor, third column,
// multiplies singles, doubles and triples: the simulation gave contact 90 only 5-7 % more hits in play than the
// league, so batting races were luck): the ratings are quantile-mapped, so the top
// of every era is rated alike and the simulation gave its best power hitters about twice the league's home runs
// whatever they really hit (a simulated 1927: Ruth 15, Gehrig 22). A hitter's factor (his real rate over what his
// power rating gives) multiplies his home-run chance in hle_pa_probs. The batter is the row whose power the plate
// appearance's fill (func_00713558) reads first from a split batting table (the game's copy for the game).
// Pitchers likewise (mods/<pack>/pitch_factor.txt, tools/eras/pitch_factor.py: "key strikeouts walks"): the
// simulation spread strikeouts 0.73-1.23 times the league (10th-90th percentile of regulars) against real aces at
// 1.8-2.5. The pitcher of a plate appearance is the fielding club's current one (slot byte at club + 380 of the
// club other than game + 80); his factors multiply the strikeout and walk buckets. The batter's own strikeout and walk
// factors (bat_factor.txt, tools/eras/bat_factor.py) multiply them too.
struct BatFactor { float hr, hit; };
static std::unordered_map<u32, std::pair<float, float>> s_pitch_factor;          // pitcher key -> (strikeouts, walks)
static std::unordered_map<u32, std::pair<float, float>> s_bat_kb;                // hitter key -> (strikeouts, walks): bat_factor.txt
static std::unordered_map<u32, BatFactor> s_hr_factor;
static bool s_in_pa_fill = false; static u32 s_fill_state = 0; static int s_hf_reads = 0;
float g_pa_hr_factor = 1.0f, g_pa_hit_factor = 1.0f, g_pa_bat_k = 1.0f, g_pa_bat_bb = 1.0f;
int bat_factors_load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r"); if (!f) return 0;
    char ln[128]; int n = 0;
    while (fgets(ln, sizeof ln, f)) { unsigned k; float a, b; if (ln[0] != '#' && sscanf(ln, "%x %f %f", &k, &a, &b) == 3 && a > 0.1f && a < 5 && b > 0.1f && b < 5) { s_bat_kb[k] = {a, b}; n++; } }
    fclose(f); if (n) rlog("sim", "%d hitter strikeout / walk factors (%s)", n, path.c_str());
    return n;
}
int pitch_factors_load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r"); if (!f) return 0;
    char ln[128]; int n = 0;
    while (fgets(ln, sizeof ln, f)) { unsigned k; float a, b; if (ln[0] != '#' && sscanf(ln, "%x %f %f", &k, &a, &b) == 3 && a > 0.2f && a < 5 && b > 0.2f && b < 5) { s_pitch_factor[k] = {a, b}; n++; } }
    fclose(f); if (n) rlog("sim", "%d pitcher factors (%s)", n, path.c_str());
    return n;
}
int hr_factors_load(const std::string& path) {
    if (const char* e = std::getenv("MVP_HRF_TEST")) { unsigned k; float v; if (sscanf(e, "%x=%f", &k, &v) == 2) { s_hr_factor.clear(); s_hr_factor[k] = {v, 1.0f}; return 1; } }   // debug
    FILE* f = fopen(path.c_str(), "r"); if (!f) return 0;
    char ln[128]; int n = 0;
    while (fgets(ln, sizeof ln, f)) {
        unsigned k; float v, h = 1.0f; if (ln[0] == '#') continue;
        const int got = sscanf(ln, "%x %f %f", &k, &v, &h);
        if (got >= 2 && v > 0.05f && v < 20 && h > 0.3f && h < 3) { s_hr_factor[k] = {v, h}; n++; }
    }
    fclose(f); if (n) rlog("sim", "%d home-run factors (%s)", n, path.c_str());
    return n;
}
void f_00713558_orig(EEContext* c);
static u64 s_hf_n[4];                                         // debug (MVP_TRACE_HRF): fills, power reads in them, keys found, factors
std::unordered_map<u32, std::pair<float, float>> g_pitch_factor_by_state;
std::unordered_map<u32, std::pair<float, float>> g_bat_kb_by_state;
std::unordered_map<u32, std::pair<float, float>> g_hr_factor_by_state;   // (home runs, hits in play) of the batter, per state
void hle_pa_fill(EEContext* c) {
    s_in_pa_fill = true; s_fill_state = c->r[4].UW[0]; s_hf_reads = 0; g_pa_hr_factor = g_pa_hit_factor = g_pa_bat_k = g_pa_bat_bb = 1.0f; f_00713558_orig(c); s_in_pa_fill = false;
    if (!s_hr_factor.empty()) { if (g_hr_factor_by_state.size() > 256) g_hr_factor_by_state.clear(); g_hr_factor_by_state[s_fill_state] = {g_pa_hr_factor, g_pa_hit_factor}; }
    if (!s_bat_kb.empty()) { if (g_bat_kb_by_state.size() > 256) g_bat_kb_by_state.clear(); g_bat_kb_by_state[s_fill_state] = {g_pa_bat_k, g_pa_bat_bb}; }
    // the pitcher: the fielding club's (the other one) current pitcher, slot byte at club + 380
    if (!s_pitch_factor.empty() || std::getenv("MVP_TRACE_PF")) {
        float kf = 1.0f, bf = 1.0f; u32 pkey = 0;
        const u32 a = ee_rd<u32>(s_fill_state + 56), gm = a ? ee_rd<u32>(a + 20) : 0;
        const u32 side = ee_rd<u32>(s_fill_state - 20 + 80);
        if (gm >= 0x100000 && gm < 0x2000000 && side < 2) {
            const u32 club = gm + (1 - side) * 3684; const u8 slot = ee_rd<u8>(club + 380);
            if (slot < ee_rd<u8>(club + 996)) pkey = ee_rd<u32>(club + 1000 + 52 * slot);
            auto it = s_pitch_factor.find(pkey); if (it != s_pitch_factor.end()) { kf = it->second.first; bf = it->second.second; }
        }
        if (g_pitch_factor_by_state.size() > 256) g_pitch_factor_by_state.clear();
        g_pitch_factor_by_state[s_fill_state] = {kf, bf};
        if (std::getenv("MVP_TRACE_PF")) { static int n = 0; if (n++ < 20) rlog("pf", "pitcher %08x k %.2f bb %.2f", pkey, kf, bf); }
    }
    if (std::getenv("MVP_TRACE_HRF")) { static int k = 0; if (k++ < 12) { const u32 g = s_fill_state - 20; const u32 a = ee_rd<u32>(s_fill_state + 56), gm = a ? ee_rd<u32>(a + 20) : 0;
        rlog("hrf", "fill state %08x factor %.2f fielding(g+80) %u flag(7376) %u", s_fill_state, g_pa_hr_factor, ee_rd<u32>(g + 80), gm ? ee_rd<u32>(gm + 7376) : 99); } }
    static const bool tr = std::getenv("MVP_TRACE_HRF") != nullptr;
    if (tr && ++s_hf_n[0] % 20000 == 0) rlog("hrf", "fills %llu power reads %llu keys %llu factors %llu", (unsigned long long)s_hf_n[0], (unsigned long long)s_hf_n[1], (unsigned long long)s_hf_n[2], (unsigned long long)s_hf_n[3]);
}
void hle_rating_norm(EEContext* c) {
    const u32 tab = c->r[4].UW[0], attr = c->r[5].UW[0];
    if (s_in_pa_fill && attr == 1 && tab && (!s_hr_factor.empty() || !s_bat_kb.empty())) {
        const u32 vt = ee_rd<u32>(tab + 32);
        if (ee_rd<u32>(vt + 44) == 0x001a10e0) {
            s_hf_n[1]++;
            // in a game the ratings come from the game's own copy, row = the player's slot in his club's record:
            // game state = fill state - 20; its +8 -> +20: two clubs of 3684 bytes, keys at club + 1000 + 52 * slot;
            // the club at bat is number game + 80 (checked: Ruth with a factor of 20 hit 178 home runs, the other club 16)
            u32 key = 0; const u32 row = c->r[6].UW[0];
            if (s_fill_state) {
                const u32 a = ee_rd<u32>(s_fill_state + 56), gm = a ? ee_rd<u32>(a + 20) : 0;
                const u32 side = ee_rd<u32>(s_fill_state - 20 + 80);
                if (gm >= 0x100000 && gm < 0x2000000 && side < 2) {
                    const u32 club = gm + side * 3684;
                    if (row < ee_rd<u8>(club + 996)) key = ee_rd<u32>(club + 1000 + 52 * row);
                }
            }
            if (key) s_hf_n[2]++;
            static int nth = 0; if (s_hf_reads == 0) nth = 0;
            static const int pick = std::getenv("MVP_HRF_PICK") ? std::atoi(std::getenv("MVP_HRF_PICK")) : 0;   // reads: batter, next batter, batter
            if (s_hf_reads++ == pick) {
                auto it = s_hr_factor.find(key); g_pa_hr_factor = it == s_hr_factor.end() ? 1.0f : it->second.hr; g_pa_hit_factor = it == s_hr_factor.end() ? 1.0f : it->second.hit;
                auto kb = s_bat_kb.find(key); g_pa_bat_k = kb == s_bat_kb.end() ? 1.0f : kb->second.first; g_pa_bat_bb = kb == s_bat_kb.end() ? 1.0f : kb->second.second;
                if (it != s_hr_factor.end()) s_hf_n[3]++;
            }
            static int shown = 0; if (std::getenv("MVP_TRACE_HRF") && shown < 30) { shown++; rlog("hrf", "fill %llu tab %08x row %u key %08x", (unsigned long long)s_hf_n[0], tab, c->r[6].UW[0], key); }
        }
    }
    f_00182f10_orig(c);
    if (std::getenv("MVP_TRACE_ATTR") && tab) {             // debug: which split-batting attributes the simulation reads
        static u64 n[64]; static u64 tot = 0; const u32 vt = ee_rd<u32>(tab + 32);
        if (ee_rd<u32>(vt + 44) == 0x001a10e0 && attr < 64) { n[attr]++; if (++tot % 200000 == 0) { std::string a; for (int i = 0; i < 32; i++) a += " " + std::to_string(n[i]); rlog("attr", "%s", a.c_str()); } }
    }
    if (attr == 13 && g_sim_calibrate) { c->f[0].f = run_draw(c, c->f[0].f); return; }
    if (g_contact_spread == 1.0f && g_power_spread == 1.0f && g_contact_shift == 0.0f && g_power_shift == 0.0f) return;
    if (attr > 1 || !tab) return;
    const u32 vt = ee_rd<u32>(tab + 32);
    if (ee_rd<u32>(vt + 44) != 0x001a10e0) return;          // the split batting tables (lrattrib) only
    static double mean[2] = {0, 0}; static u64 n[2] = {0, 0};
    const float v = c->f[0].f;
    n[attr]++; mean[attr] += (v - mean[attr]) / (double)std::min<u64>(n[attr], 20000);
    const float k = attr == 0 ? g_contact_spread : g_power_spread;
    float r = (float)mean[attr] + (v - (float)mean[attr]) * k + (attr == 0 ? g_contact_shift : g_power_shift);
    c->f[0].f = r < 0 ? 0 : r > 1 ? 1 : r;
}

// ---------------------------------------------------------------- league environment of simulated games
// One simulated plate appearance (func_007169d0, about four calls per plate appearance): func_00713558 fills twelve
// floats at the state's start from the batter's and pitcher's ratings, func_007140e8 moves mass by the count, and
// func_00716ef8 draws the outcome from them in this order: +40 intentional walk, +36 hit by pitch, +32 walk, +4
// single, +8 double, +12 triple, +16 home run, +44 (unused, 0), +20 strikeout, +24 / +28 outs in play; +0 is the
// sum of the hits. The game's own numbers give every era the same league (1977 or 2027: about .280, 19% strikeouts).
// Here each outcome is scaled so the league plays like the real major leagues of the season being played
// (mods/league_rates.txt from Lahman, tools/eras/league_rates.py; a pack's rates.txt wins), the hitter-pitcher
// differences kept: bucket * target / league mean of that bucket * correction, outs in play taking the rest. The
// correction follows what the season's statistics actually show (some walks and home runs come from elsewhere in
// the engine): every ~15000 plate appearances in the database's current-season batting lines (all levels) it
// moves toward target / observed. sim.txt "calibrate 0" (or MVP_SIM=calibrate=0) leaves the game's own numbers.
// MVP_TRACE_PA=1 prints the running means and the corrections.
std::map<int, std::array<float, 7>> g_rates;                 // year -> k bb(+ibb) hbp 1b 2b 3b hr per plate appearance
int g_sim_calibrate = 1;
float g_dp_shift = 0.6f, g_sf_shift = 0.85f;                                     // share of ground outs made fly outs when a double play is on
float g_sim_spread[7] = {3.2f, 1.25f, 1.0f, 1.0f, 1.15f, 1.3f, 1.5f};   // k bb hbp 1b 2b 3b hr (sim.txt spread_k ...)
int league_rates_load(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return 0;
    char ln[256]; int n = 0;
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '#') continue;
        int y; std::array<float, 7> r;
        if (sscanf(ln, "%d %f %f %f %f %f %f %f", &y, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6]) == 8 && y > 1800 && y < 2200) { g_rates[y] = r; n++; }
    }
    fclose(f);
    return n;
}
static const std::array<float, 7>* rates_for(int y) {
    if (g_rates.empty()) return nullptr;
    auto it = g_rates.lower_bound(y);
    if (it == g_rates.end()) return &std::prev(it)->second;
    return &it->second;
}
void f_007140e8_orig(EEContext* c);
void run_check(u32 game);
void hle_pa_probs(EEContext* c) {
    const u32 st = c->r[4].UW[0];
    if (std::getenv("MVP_TRACE_HRF")) { static int k = 0; if (k++ < 12) rlog("hrf", "probs state %08x ra %08x", st, c->r[31].UW[0]); }
    f_007140e8_orig(c);
    static const bool trace_runs_ = std::getenv("MVP_TRACE_RUNS") != nullptr;
    if (st && trace_runs_) { const u32 a = ee_rd<u32>(st + 56); if (a) run_check(ee_rd<u32>(a + 20)); }
    static int trace = -1; if (trace < 0) trace = std::getenv("MVP_TRACE_PA") ? 1 : 0;
    if (!st || (!g_sim_calibrate && !trace)) return;
    auto rf = [&](int off) { u32 x = ee_rd<u32>(st + off); float f; memcpy(&f, &x, 4); return f; };
    auto wf = [&](int off, float f) { u32 x; memcpy(&x, &f, 4); ee_wr<u32>(st + off, x); };
    // groups: k, bb (walk + intentional), hbp, 1b, 2b, 3b, hr
    static const int kOff[7] = {20, 32, 36, 4, 8, 12, 16};
    float v[7];
    for (int k = 0; k < 7; k++) { v[k] = rf(kOff[k]) + (k == 1 ? rf(40) : 0.0f); if (!(v[k] > 0)) v[k] = 0; }   // (a bucket can come out slightly negative)
    for (int k = 0; k < 7; k++) if (!(v[k] <= 1.0f)) return;   // now and then the state holds garbage (1e29 strikeout buckets): left alone
    static double mean[7], meanpow[7] = {1, 1, 1, 1, 1, 1, 1}; static u64 n = 0;
    static double corr[7] = {1.25, 0.65, 1.05, 1.02, 0.98, 0.85, 0.58};   // where 1977 and 2027 seasons settle (walks and home runs also come from other paths)
    n++;
    const double w = 1.0 / (double)std::min<u64>(n, 30000);
    for (int k = 0; k < 7; k++) {
        mean[k] += (v[k] - mean[k]) * w;
        if (mean[k] > 1e-5) { const double pw = std::pow(v[k] / mean[k], (double)g_sim_spread[k]); if (std::isfinite(pw)) meanpow[k] += (pw - meanpow[k]) * w; }
    }
    const int year = g_last_date_year ? g_last_date_year : 2005 + g_year_shift;
    const std::array<float, 7>* tg = g_sim_calibrate ? rates_for(year) : nullptr;
    if (tg && n > 200) {
        float out[7], sum = 0;
        for (int k = 0; k < 7; k++) {
            // the spread: (bucket / mean)^spread, so the best and worst hitters and pitchers stand further apart than
            // the game's own numbers put them (1977: strikeout rates of regulars 11-16% instead of 7-20%)
            double o = mean[k] > 1e-5 && meanpow[k] > 1e-6 ? (*tg)[k] * corr[k] * std::pow(v[k] / mean[k], (double)g_sim_spread[k]) / meanpow[k] : v[k];
            if (!std::isfinite(o) || o < 0) o = v[k];
            if (k <= 1) {                                          // the pitcher's strikeouts and walks (pitch_factor.txt)
                extern std::unordered_map<u32, std::pair<float, float>> g_pitch_factor_by_state;
                auto pf = g_pitch_factor_by_state.find(st); if (pf != g_pitch_factor_by_state.end()) o *= k == 0 ? pf->second.first : pf->second.second;
                extern std::unordered_map<u32, std::pair<float, float>> g_bat_kb_by_state;
                auto bf = g_bat_kb_by_state.find(st); if (bf != g_bat_kb_by_state.end()) o *= k == 0 ? bf->second.first : bf->second.second;
            }
            if (k >= 3 && k <= 5) {                               // and his hits in play
                extern std::unordered_map<u32, std::pair<float, float>> g_hr_factor_by_state;
                auto hf = g_hr_factor_by_state.find(st); if (hf != g_hr_factor_by_state.end()) o *= hf->second.second;
            }
            if (k == 6) {
                extern std::unordered_map<u32, std::pair<float, float>> g_hr_factor_by_state;
                auto hf = g_hr_factor_by_state.find(st); const float fac = hf == g_hr_factor_by_state.end() ? 1.0f : hf->second.first;
                o *= fac;                                         // the batter's home-run factor (era packs)
                static const bool tr = std::getenv("MVP_TRACE_HRF") != nullptr;
                if (tr) { static double sa = 0, sb = 0; static u64 na = 0, nb = 0; if (fac > 2.5f) { sb += o; nb++; } else { sa += o; na++; }
                    if ((na + nb) % 100000 == 0) rlog("hrf", "hr bucket: factor>2.5 %llu calls mean %.4f | rest %llu mean %.4f (corr %.3f)", (unsigned long long)nb, nb ? sb / nb : 0, (unsigned long long)na, na ? sa / na : 0, corr[6]); }
            }
            o = std::min(o, k == 0 ? 0.6 : 0.4);
            out[k] = (float)o; sum += out[k];
        }
        const float o24 = rf(24), o28 = rf(28), rest = 1.0f - sum - rf(44);
        if (rest > 0.05f && o24 + o28 > 0) {
            const float s = rest / (o24 + o28);
            const float bb0 = rf(32), ibb0 = rf(40);
            const float split = bb0 + ibb0 > 0 ? bb0 / (bb0 + ibb0) : 1.0f;
            wf(20, out[0]); wf(32, out[1] * split); wf(40, out[1] * (1 - split)); wf(36, out[2]);
            wf(4, out[3]); wf(8, out[4]); wf(12, out[5]); wf(16, out[6]); wf(0, out[3] + out[4] + out[5] + out[6]);
            // double plays: with a runner on first and fewer than two outs the game turns about half its ground outs
            // into two (1.6 extra outs a game, real leagues 0.8); part of those ground balls are made fly balls instead
            float g24 = o24 * s, g28 = o28 * s;
            const u32 gm = ee_rd<u32>(ee_rd<u32>(st + 56) + 20);
            if (gm && ee_rd<u8>(gm + 7380) != 0xff && ee_rd<u8>(gm + 7369) < 2) { const float mv = g24 * g_dp_shift; g24 -= mv; g28 += mv; }
            // a runner on third, fewer than two outs, first base open: he scores on 67% of the game's ground outs but
            // 29% of its fly outs (no sacrifice flies to speak of; real leagues about 55% on outs in play together)
            else if (gm && ee_rd<u8>(gm + 7380 + 24) != 0xff && ee_rd<u8>(gm + 7369) < 2) { const float mv = g28 * g_sf_shift; g28 -= mv; g24 += mv; }
            wf(24, g24); wf(28, g28);
        }
    }
    // the correction: the season's batting lines so far (current-season split tables, slots 416 / 420)
    if (tg && n % 5000 == 0) {
        const u32 db = ee_rd<u32>(0x7d66f8);
        if (!db) return;
        static const int kAt[9] = {5, 6, 7, 10, 2, 3, 4, 0, 11};   // so bb ibb hbp 1b 2b 3b hr tpa (lrbatstat order)
        double tot[9] = {0};
        EEContext saved = *c;
        for (int slot : {416, 420}) {
            const u32 tab = ee_rd<u32>(db + slot); if (!tab || tab >= 0x2000000) continue;
            const u32 idx = ee_rd<u32>(tab + 16); if (!idx || idx >= 0x2000000) continue;
            const u32 ents = ee_rd<u32>(idx), rows = ee_rd<u32>(idx + 4);
            const u32 vt = ee_rd<u32>(tab + 32);
            const s16 adj = ee_rd<s16>(vt + 40); const u32 fn = ee_rd<u32>(vt + 44);
            if (fn != 0x00181740 || rows > 20000) continue;
            for (u32 i = 0; i < rows; i++) {
                const u32 row = ee_rd<u32>(ents + 12 * i + 4);
                for (int a = 0; a < 9; a++) {
                    c->r[4].UD[0] = tab + adj; c->r[5].UD[0] = (u32)kAt[a]; c->r[6].UD[0] = row;
                    extern void ee_call(EEContext*, u32); ee_call(c, fn);
                    tot[a] += c->r[2].SW[0];
                }
            }
        }
        *c = saved;
        static double prev[9], win[9];
        if (tot[8] < prev[8]) for (int a = 0; a < 9; a++) { prev[a] = 0; win[a] = 0; }   // a new season (or a loaded one)
        for (int a = 0; a < 9; a++) { win[a] += tot[a] - prev[a]; prev[a] = tot[a]; }
        if (win[8] >= 15000) {
            const double pa = win[8];
            const double obs[7] = {win[0] / pa, (win[1] + win[2]) / pa, win[3] / pa, win[4] / pa, win[5] / pa, win[6] / pa, win[7] / pa};
            for (int k = 0; k < 7; k++) {
                if (obs[k] <= 0 || (*tg)[k] <= 0) continue;
                double f = std::pow((*tg)[k] / obs[k], 0.8);
                f = f < 0.75 ? 0.75 : f > 1.33 ? 1.33 : f;
                corr[k] = std::min(4.0, std::max(0.25, corr[k] * f));
            }
            if (trace) fprintf(stderr, "sim %d: %.0f PA  k %.3f/%.3f bb %.3f/%.3f hbp %.3f/%.3f 1b %.3f/%.3f 2b %.3f/%.3f 3b %.4f/%.4f hr %.4f/%.4f  corr %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                               year, pa, obs[0], (*tg)[0], obs[1], (*tg)[1], obs[2], (*tg)[2], obs[3], (*tg)[3], obs[4], (*tg)[4], obs[5], (*tg)[5], obs[6], (*tg)[6],
                               corr[0], corr[1], corr[2], corr[3], corr[4], corr[5], corr[6]);
            for (int a = 0; a < 9; a++) win[a] = 0;
        }
    }
    if (trace && n % 50000 == 0) {
        fprintf(stderr, "pa %llu (%d): raw means k %.4f bb %.4f hbp %.4f 1b %.4f 2b %.4f 3b %.4f hr %.4f\n", (unsigned long long)n, year,
                mean[0], mean[1], mean[2], mean[3], mean[4], mean[5], mean[6]);
    }
}

// ---------------------------------------------------------------- base running in simulated games (MVP_TRACE_RUNS=1)
// The runner on base b sits at game + 7380 + 12 * b (a byte: index, 0xff = empty), outs at game + 7369, the half
// at game + 7376. Each outcome handler notes the bases before; the next plate appearance's first step (hle_pa_probs)
// sees what became of them. Counted: runners scoring from second on a single, first to third, scoring from first
// on a double, sacrifice flies, double plays.
struct RunSnap { int type; u8 b[3]; u8 outs, half, inning; };
static std::map<u32, RunSnap> s_run_pending;
static u64 s_run_cnt[64];
static int trace_runs() { static int t = -1; if (t < 0) t = std::getenv("MVP_TRACE_RUNS") ? 1 : 0; return t; }
static u32 game_of_obj(u32 obj) { const u32 a = ee_rd<u32>(obj + 8); return a ? ee_rd<u32>(a + 20) : 0; }
static void run_note(u32 game, int type) {
    if (!game) return;
    RunSnap s; s.type = type;
    for (int b = 0; b < 3; b++) s.b[b] = ee_rd<u8>(game + 7380 + 12 * b);
    s.outs = ee_rd<u8>(game + 7369); s.half = ee_rd<u8>(game + 7376); s.inning = ee_rd<u8>(game + 7368);
    s_run_pending[game] = s;
}
void run_check(u32 game) {                                  // at the next plate appearance of that game
    auto it = s_run_pending.find(game); if (it == s_run_pending.end()) return;
    const RunSnap s = it->second; s_run_pending.erase(it);
    if (ee_rd<u8>(game + 7376) != s.half || ee_rd<u8>(game + 7368) != s.inning) {        // the inning ended
        const int need = 3 - s.outs - (s.type >= 2 ? 1 : 0);                                  // outs the play made beyond the batter's
        s_run_cnt[30 + s.type]++; if (need > 0) s_run_cnt[34 + s.type] += need;
        return;
    }
    u8 now[3]; for (int b = 0; b < 3; b++) now[b] = ee_rd<u8>(game + 7380 + 12 * b);
    const int douts = ee_rd<u8>(game + 7369) - s.outs;
    auto where = [&](u8 r) { for (int b = 2; b >= 0; b--) if (now[b] == r) return b; return -1; };
    // 0 single: [0] r2 chances [1] r2 scored [2] r1 chances [3] r1 to 3rd
    if (s.type == 0) {
        if (s.b[1] != 0xff) { s_run_cnt[0]++; if (where(s.b[1]) < 0 && douts == 0) s_run_cnt[1]++; }
        if (s.b[0] != 0xff && s.b[1] == 0xff) { s_run_cnt[2]++; if (where(s.b[0]) == 2) s_run_cnt[3]++; }
    }
    // 1 double: [4] r1 chances [5] r1 scored
    if (s.type == 1 && s.b[0] != 0xff) { s_run_cnt[4]++; if (where(s.b[0]) < 0 && douts == 0) s_run_cnt[5]++; }
    // 2/3 outs in play (+24 / +28): [6+2k] r3 with < 2 outs chances, [7+2k] scored; [10+2k] r1 alone < 2 outs chances, [11+2k] double play
    if (s.type == 2 || s.type == 3) {
        const int k = s.type - 2;
        if (s.b[2] != 0xff && s.outs < 2) { s_run_cnt[6 + 2 * k]++; if (where(s.b[2]) < 0 && douts == 1) s_run_cnt[7 + 2 * k]++; }
        if (s.b[0] != 0xff && s.outs < 2) { s_run_cnt[10 + 2 * k]++; if (douts >= 2) s_run_cnt[11 + 2 * k]++; }
        s_run_cnt[14 + k]++;
    }
    { const int extra = douts - (s.type >= 2 ? 1 : 0); s_run_cnt[20 + s.type]++; if (extra > 0) s_run_cnt[24 + s.type] += extra; }
    static u64 n = 0;
    if (++n % 20000 == 0) {
        fprintf(stderr, "runs: inning-ending extra outs: 1B %llu 2B %llu out24 %llu out28 %llu (of %llu %llu %llu %llu)\n",
                (unsigned long long)s_run_cnt[34], (unsigned long long)s_run_cnt[35], (unsigned long long)s_run_cnt[36], (unsigned long long)s_run_cnt[37],
                (unsigned long long)s_run_cnt[30], (unsigned long long)s_run_cnt[31], (unsigned long long)s_run_cnt[32], (unsigned long long)s_run_cnt[33]);
        fprintf(stderr, "runs: extra outs per event: 1B %.3f 2B %.3f out24 %.3f out28 %.3f (events %llu %llu %llu %llu)\n",
                s_run_cnt[20] ? (double)s_run_cnt[24] / s_run_cnt[20] : 0, s_run_cnt[21] ? (double)s_run_cnt[25] / s_run_cnt[21] : 0,
                s_run_cnt[22] ? (double)s_run_cnt[26] / s_run_cnt[22] : 0, s_run_cnt[23] ? (double)s_run_cnt[27] / s_run_cnt[23] : 0,
                (unsigned long long)s_run_cnt[20], (unsigned long long)s_run_cnt[21], (unsigned long long)s_run_cnt[22], (unsigned long long)s_run_cnt[23]);
        auto pc = [&](int a, int b) { return s_run_cnt[a] ? 100.0 * s_run_cnt[b] / s_run_cnt[a] : 0.0; };
        fprintf(stderr, "runs: 1B r2 scores %.0f%% (%llu), r1 to 3rd %.0f%%; 2B r1 scores %.0f%%; out24 r3 scores %.0f%% dp %.0f%%; out28 r3 scores %.0f%% dp %.0f%%; outs %llu / %llu\n",
                pc(0, 1), (unsigned long long)s_run_cnt[0], pc(2, 3), pc(4, 5), pc(6, 7), pc(10, 11), pc(8, 9), pc(12, 13),
                (unsigned long long)s_run_cnt[14], (unsigned long long)s_run_cnt[15]);
    }
}
void f_007186f8_orig(EEContext* c); void f_007189c8_orig(EEContext* c); void f_00717eb8_orig(EEContext* c); void f_00717b88_orig(EEContext* c);
void hle_sim_single(EEContext* c) { if (trace_runs()) run_note(game_of_obj(c->r[4].UW[0]), 0); f_007186f8_orig(c); }
void hle_sim_double(EEContext* c) { if (trace_runs()) run_note(game_of_obj(c->r[4].UW[0]), 1); f_007189c8_orig(c); }
// Reached on error. A simulated ground ball was always an out or a hit, and only 5 % of simulated runs were unearned
// (real: 28 % in 1910, 11 % in 1977, 8 % today). The share of ground outs turned into an error follows the real
// errors a game of the season (0.08 x errors a team game: 7 % in 1977, 5 % today, 14 % in 1910). With "Simulated games: real league" such a ground
// out may instead put the batter on first with every forced runner moving up, as the walk does (func_007142f0 on
// the bases at +204; play type 13 at +280) but without the walk's statistics event, and with the batter marked as
// not having reached on his own (runner record +4 = 0: the runs he scores are unearned, func_007126f0).
// sim.txt: error_rate = share of ground outs.
float g_error_rate = -1.0f;
static u64 s_roe_count = 0;
void f_007142f0(EEContext* c);
static double wp_rand();
void hle_sim_out24(EEContext* c) {
    const u32 s = c->r[4].UW[0];
    if (trace_runs()) run_note(game_of_obj(s), 2);
    if (g_sim_calibrate && s >= 0x100000 && s < 0x2000000) {
        const int year = g_last_date_year ? g_last_date_year : 2005 + g_year_shift;
        static const int ey[] = {1910, 1927, 1941, 1951, 1961, 1969, 1977, 1986, 1993, 1999, 2004, 2012, 2019};      // errors a team game (Lahman)
        static const float eg[] = {1.78f, 1.46f, 1.18f, 0.97f, 0.93f, 0.87f, 0.88f, 0.82f, 0.77f, 0.72f, 0.65f, 0.62f, 0.60f};
        float e = year <= ey[0] ? eg[0] : eg[12];
        for (int i = 0; i < 12; i++) if (year >= ey[i] && year < ey[i + 1]) e = eg[i] + (eg[i + 1] - eg[i]) * (year - ey[i]) / float(ey[i + 1] - ey[i]);
        const double q = g_error_rate >= 0 ? g_error_rate : 0.08 * e;
        const u32 b0 = ee_rd<u32>(s + 204);
        if (b0 >= 0x100000 && b0 < 0x2000000 && wp_rand() < q) {
            GPR128 sr[32]; std::memcpy(sr, c->r, sizeof sr);
            ee_wr<u32>(s + 280, 13);
            c->r[4].UD[0] = s + 204; f_007142f0(c);
            ee_wr<u32>(b0 + 4, 0);
            std::memcpy(c->r, sr, sizeof sr);
            if (++s_roe_count <= 3) rlog("sim", "reached on error");
            return;
        }
    }
    f_00717eb8_orig(c);
}
void hle_sim_out28(EEContext* c) { if (trace_runs()) run_note(game_of_obj(c->r[4].UW[0]), 3); f_00717b88_orig(c); }

// Front-end attribute reads, func_0018c308(db, attribute, player key): the keys of the last reads are kept, so a
// screenshot can say whose picture it is (tools/headshots.py: the Edit Player screen reads only that player).
u32 g_attr_keys[4096]; u32 g_attr_key_n = 0, g_attr_mark = 0;
void f_0018c308_orig(EEContext* c);
u32 g_last_pkey = 0;                                         // player whose attributes were read last
void hle_db_attr_get(EEContext* c) {
    g_attr_keys[g_attr_key_n++ & 4095] = g_last_pkey = c->r[6].UW[0];
    f_0018c308_orig(c);
}

// ---------------------------------------------------------------- portraits
// The game's player portraits (DATA/FRONTEND/PORTRAIT.BIG) are 128 x 128 pictures named after the player's audio
// id ("%04d.ssh", players with photo < 2) or composed from generic heads (face, hair colour, facial hair). A
// portraits mod (mods/portraits-*/portraits.txt: "<player key in hex> <picture number>" per line, and the pictures
// as new members of PORTRAIT.BIG) gives players their own. The three portrait builders all end in one of two
// loaders: func_001db260(.., .., id) loads "%04d.ssh", func_001db1f0 builds the generic name. Before either,
// the builder reads the player's attributes: from the database (func_0018c308, key in a2) in the menus, or from
// the team's table in a game (func_0039f158: the batter/pitcher slot of a card), so the last key read is his.
#include <unordered_map>
static std::unordered_map<u32, u32> s_portrait;             // player key -> picture number
static bool s_portrait_log = false;
int portraits_load(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return 0;
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned key = 0, id = 0;
        if (line[0] == '#' || sscanf(line, "%x %u", &key, &id) != 2) continue;
        s_portrait[key] = id; n++;
    }
    fclose(f);
    s_portrait_log = std::getenv("MVP_PORTRAIT_LOG") != nullptr;
    return n;
}
void f_0039f158_orig(EEContext* c);
void hle_game_attr_get(EEContext* c) {                      // a0 = card slot: team = .. + *(*a0 + 1740) * 3684, index = *(*a0 + 1744)
    const u32 o = ee_rd<u32>(c->r[4].UW[0]);
    if (o) {
        const u32 team = ee_rd<u32>(ee_rd<u32>(ee_rd<u32>(o + 1020) + 1780) + 20) + ee_rd<u32>(o + 1740) * 3684;
        const u32 idx = ee_rd<u8>(o + 1744);
        if (idx < 64) g_last_pkey = ee_rd<u32>(team + 1000 + 52 * idx);
    }
    f_0039f158_orig(c);
}
void f_001db260_orig(EEContext* c);
void f_001db1f0_orig(EEContext* c);
static bool portrait_of(u32& id) {
    if (s_portrait.empty()) return false;
    auto it = s_portrait.find(g_last_pkey);
    if (s_portrait_log) fprintf(stderr, "portrait: key %08x -> %d\n", g_last_pkey, it == s_portrait.end() ? -1 : (int)it->second);
    if (it == s_portrait.end()) return false;
    id = it->second; return true;
}
void hle_portrait_photo(EEContext* c) {                     // a2 = the audio id the game would show
    u32 id; if (portrait_of(id)) c->r[6].UD[0] = id;
    f_001db260_orig(c);
}
void hle_portrait_generic(EEContext* c) {                   // same a0, a1 as the photo loader
    u32 id; if (portrait_of(id)) { c->r[6].UD[0] = id; f_001db260_orig(c); return; }
    f_001db1f0_orig(c);
}

// Draft classes. func_00676a58 makes the rookies of a draft class; func_0018c488(db, attribute, key, value) and
// func_0018c500(db, attribute, key, string) write a player's attributes. MVP_TRACE_ROOKIE=1 logs what they write.
static int s_trace_rookie = -1, s_in_rookie = 0;
static bool trace_rookie() { if (s_trace_rookie < 0) s_trace_rookie = std::getenv("MVP_TRACE_ROOKIE") != nullptr; return s_trace_rookie; }
void f_00676a58_orig(EEContext* c);
void hle_make_rookies(EEContext* c) {
    if (trace_rookie()) fprintf(stderr, "rookie: make a0=%08x ra=%08x\n", c->r[4].UW[0], c->r[31].UW[0]);
    s_in_rookie++; f_00676a58_orig(c); s_in_rookie--;
    if (trace_rookie()) fprintf(stderr, "rookie: done\n");
}
void f_0018c488_orig(EEContext* c);
void hle_db_attr_set(EEContext* c) {
    if (trace_rookie() && s_in_rookie) fprintf(stderr, "rookie: set attr %d key %08x = %d (ra %08x)\n", c->r[5].SW[0], c->r[6].UW[0], c->r[7].SW[0], c->r[31].UW[0]);
    f_0018c488_orig(c);
}
void f_0018c500_orig(EEContext* c);
void hle_db_str_set(EEContext* c) {
    if (trace_rookie() && s_in_rookie) {
        std::string v; const u32 p = c->r[7].UW[0];
        for (u32 i = 0; p && i < 40; i++) { char ch = (char)ee_rd<u8>(p + i); if (!ch) break; v += ch; }
        fprintf(stderr, "rookie: set str %d key %08x = '%s'\n", c->r[5].SW[0], c->r[6].UW[0], v.c_str());
    }
    f_0018c500_orig(c);
}

// ---------------------------------------------------------------- draft classes from a league pack
// mods/<pack>/draft/<year>.csv (tools/leaguepack.py: the columns of players.csv): the players a club can draft
// after the <year> season. The game makes each rookie of its class with func_006f0630(generator, key, ..) into a
// free key; afterwards the next entry of the pack's class of the same kind (pitcher or not: the game has already
// made the pitcher record or not) overwrites him: names, birthday, body, ratings, splits and pitches, through the
// game's own setters: func_0018c488 (player attributes), func_0018c770 (splits: 0 vs left-handers, 1 vs right),
// func_0018c8f8 (pitches), func_0018c500 (first / last name). Attribute numbers are the game's own (the names in
// the executable's lists: playerattrib_audioid = 0, ..., lrattrib_contact = 0, ..., pitchattrib_stamina = 0, ...).
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
struct DraftEntry { std::map<std::string, std::string> v; bool pitcher; };
static std::map<int, std::vector<DraftEntry>> s_draft;       // year -> class
static int s_draft_year = 0; static size_t s_draft_next[2]; static bool s_in_class = false;
static const char* const kAttr[] = {"audioid", "birthday", "height", "weight", "jerseynum", "primary_position", "secondary_position", "photo",
    "bats", "throws", "ditty", "hidden", "bunting", "speed", "throwstrength", "throwaccuracy", "fielding", "range", "durability", "platediscipline",
    "stealing_aggressive", "baserunning", "starpower", "topprospect", "salary", "contract_length", "offercount", "personality", "playedtoday",
    "energylevel", "injurytype", "injuryduration", "swingtype", "battingstance", "catchermask", "hairstyle", "skin_tone", "haircolour",
    "fieldglovecolour", "batcolour", "facialhair", "elbowguard", "shinguard", "wristband", "face", "face2004style", "bodytype", "socks",
    "batglove", "boneprofile"};
static const char* const kLR[] = {"contact", "power", "hit_ul", "hit_cl", "hit_ll", "hit_um", "hit_cm", "hit_lm", "hit_ur", "hit_cr", "hit_lr",
    "chasefb", "chaseslowbreak", "chasehardbreak", "takefb", "takeslowbreak", "takehardbreak", "missfb", "missslowbreak", "misshardbreak",
    "lf_pct", "cf_pct", "rf_pct", "hr_pct", "fb_pct", "ld_pct", "gb_pct"};
static const char* const kPitch[] = {"stamina", "pickoff", "fastball_movement", "fastball_description", "fastball_control", "fastball_velocity",
    "pitch2_type", "pitch2_movement", "pitch2_description", "pitch2_control", "pitch2_velocity", "pitch3_type", "pitch3_movement",
    "pitch3_description", "pitch3_control", "pitch3_velocity", "pitch4_type", "pitch4_movement", "pitch4_description", "pitch4_control",
    "pitch4_velocity", "pitch5_type", "pitch5_movement", "pitch5_description", "pitch5_control", "pitch5_velocity", "pitcher_delivery"};
static std::vector<std::string> csv_split(const std::string& l) {
    std::vector<std::string> out; std::string cur; bool q = false;
    for (char ch : l) { if (ch == '"') q = !q; else if (ch == ',' && !q) { out.push_back(cur); cur.clear(); } else if (ch != '\r') cur += ch; }
    out.push_back(cur); return out;
}
int draft_classes_load(const char* dir) {
    namespace fs = std::filesystem; std::error_code ec; int n = 0;
    for (auto it = fs::directory_iterator(std::string(dir) + "/draft", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string f = it->path().filename().string(); int year = std::atoi(f.c_str());
        if (year < 1871 || f.size() < 8 || f.substr(f.size() - 4) != ".csv") continue;
        std::ifstream in(it->path()); std::string line; std::vector<std::string> head;
        while (std::getline(in, line)) {
            auto cells = csv_split(line);
            if (head.empty()) { head = cells; continue; }
            DraftEntry e;
            for (size_t i = 0; i < head.size() && i < cells.size(); i++) if (!cells[i].empty()) e.v[head[i]] = cells[i];
            const std::string pos = e.v.count("primary_position") ? e.v["primary_position"] : "1";
            e.pitcher = pos == "0" || pos == "10";
            s_draft[year].push_back(std::move(e)); n++;
        }
    }
    return n;
}
static int days_from_date(const std::string& iso, int shift) {   // "YYYY-MM-DD" -> days since 1947-12-31 in the game's calendar
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return std::atoi(iso.c_str());
    y -= shift; if (m == 2 && d == 29) d = 28;
    auto days = [](int y, int m, int d) { if (m < 3) { y--; m += 12; } return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d; };
    return days(y, m, d) - days(1947, 12, 31);
}
static void guest_call(EEContext* c, void (*fn)(EEContext*), u32 a0, u32 a1, u32 a2, u32 a3, u32 t0 = 0) {
    c->r[4].UD[0] = a0; c->r[5].UD[0] = a1; c->r[6].UD[0] = a2; c->r[7].UD[0] = a3; c->r[8].UD[0] = t0; fn(c);
}
void f_006f0630_orig(EEContext* c);
void f_0018c770(EEContext* c); void f_0018c8f8(EEContext* c); void f_0018c308(EEContext* c);
void f_0018c488(EEContext* c); void f_0018c500(EEContext* c);
void f_006edc28_orig(EEContext* c);
void hle_draft_class(EEContext* c) {                        // the class of the year that just ended
    // A class is made a season ahead: the one made in a season's playoffs (checked: October 2027) is drafted after the
    // next season, the one made with a new dynasty after its first. Month as the game keeps it (0 = January, checked
    // against the calendar when the log shows it).
    // A new dynasty makes two classes before its calendar exists (the dates seen then are not the season's):
    // the first is drafted after the first season, the second after the next.
    static int s_made_at_start = 0;
    const int first = 2005 + g_year_shift;
    s_draft_year = g_last_date_year ? g_last_date_year : g_max_date_year;
    if (g_last_date_month >= 7) s_draft_year++;
    if (g_year_shift && (s_draft_year == 2005 || s_draft_year < first)) {
        s_draft_year = s_made_at_start == first ? first + 1 : first;
        s_made_at_start = s_draft_year;
    } else s_made_at_start = 0;
    s_draft_next[0] = s_draft_next[1] = 0;
    if (trace_rookie() || !s_draft.empty()) fprintf(stderr, "draft: class of %d (last date %d/%d, latest %d): %zu players in the pack\n", s_draft_year, g_last_date_month, g_last_date_year, g_max_date_year,
                                                      s_draft.count(s_draft_year) ? s_draft[s_draft_year].size() : (size_t)0);
    s_in_class = true;
    f_006edc28_orig(c);
    s_in_class = false;
}
void hle_rookie(EEContext* c) {                             // a0 = generator, a1 = the player to remake (0: a new one), returns his key
    // A class (func_006edc28) remakes its members that are still undrafted (a1 = their key) and then adds new ones
    // (a1 = 0, the key comes back in v0) until the database holds 3250 players; outside a class the same call fills
    // the free-agent pool (func_0067bd18), which is left alone.
    u32 key = c->r[5].UW[0];
    f_006f0630_orig(c);
    if (!s_in_class) return;
    if (!key) key = c->r[2].UW[0];
    auto it = s_draft.find(s_draft_year);
    if (it == s_draft.end() || !key) return;
    const u32 db = ee_rd<u32>(0x7d66f8);
    if (!db) return;
    EEContext saved = *c;
    guest_call(c, f_0018c308, db, 5, key, 0);                // primary position of what the game made
    const int pos = c->r[2].SW[0];
    const int kind = (pos == 0 || pos == 10) ? 1 : 0;
    size_t& nx = s_draft_next[kind]; const auto& cls = it->second;
    while (nx < cls.size() && cls[nx].pitcher != (kind == 1)) nx++;
    if (nx >= cls.size()) {
        // the pack's class is used up for this kind: what the game made stays, as a filler below the real players
        // (otherwise its made-up top prospects head the draft board): modest contact / power, or command and speed
        u32 h = key * 2654435761u;
        auto rnd = [&](int lo, int hi) { h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; return lo + (int)(h % (u32)(hi - lo + 1)); };
        if (kind == 0) {
            for (int side = 0; side < 2; side++) {
                guest_call(c, f_0018c770, db, 0, key, (u32)side, (u32)rnd(22, 48));
                guest_call(c, f_0018c770, db, 1, key, (u32)side, (u32)rnd(15, 42));
            }
        } else {
            guest_call(c, f_0018c8f8, db, 4, key, (u32)rnd(35, 55));        // fastball control
            guest_call(c, f_0018c8f8, db, 5, key, (u32)rnd(84, 90));        // fastball velocity
            for (int a : {9, 14, 19, 24}) guest_call(c, f_0018c8f8, db, (u32)a, key, (u32)rnd(30, 50));   // the other pitches' control
        }
        guest_call(c, f_0018c488, db, 23, key, 0);          // not a top prospect
        guest_call(c, f_0018c488, db, 22, key, 0);          // and no star in the making (the rookie generator's star level: how far he develops)
        if (trace_rookie()) fprintf(stderr, "draft: %08x filler (%s)\n", key, kind ? "P" : "H");
        *c = saved; return;
    }
    const DraftEntry& e = cls[nx++];
    auto get = [&](const std::string& k) -> const std::string* {     // (a value that is not a number, like "-" for no pitch: left as the game made it)
        auto f = e.v.find(k); if (f == e.v.end()) return nullptr;
        const std::string& x = f->second;
        if (k != "first" && k != "last" && k != "birthday" && !(std::isdigit((unsigned char)x[0]) || (x[0] == '-' && x.size() > 1 && std::isdigit((unsigned char)x[1])))) return nullptr;
        return &x; };
    for (int a = 0; a < (int)(sizeof kAttr / sizeof *kAttr); a++) {
        const std::string* v = get(kAttr[a]);
        if (!v || a == 11 || (a >= 26 && a <= 31)) continue;   // hidden, and the season's own state (offers, energy, injuries)
        int val = a == 1 ? days_from_date(*v, g_year_shift) : std::atoi(v->c_str());
        if (a == 5 && (val == 0 || val == 10) != (kind == 1)) continue;
        guest_call(c, f_0018c488, db, (u32)a, key, (u32)val);
    }
    for (int side = 0; side < 2; side++)
        for (int a = 0; a < (int)(sizeof kLR / sizeof *kLR); a++)
            if (const std::string* v = get(std::string(side ? "vsR_" : "vsL_") + kLR[a])) guest_call(c, f_0018c770, db, (u32)a, key, (u32)side, (u32)std::atoi(v->c_str()));
    if (kind == 1)
        for (int a = 0; a < (int)(sizeof kPitch / sizeof *kPitch); a++)
            if (const std::string* v = get(std::string("pitch_") + kPitch[a])) guest_call(c, f_0018c8f8, db, (u32)a, key, (u32)std::atoi(v->c_str()));
    // names: written into scratch space below the stack pointer, then the setter copies them
    const u32 scratch = (saved.r[29].UW[0] - 1024) & ~15u;
    for (int i = 0; i < 2; i++) {
        const std::string* v = get(i ? "last" : "first");
        if (!v) continue;
        std::string nm = v->substr(0, 30);
        for (size_t k = 0; k <= nm.size(); k++) ee_wr<u8>(scratch + 64 * i + (u32)k, k < nm.size() ? (u8)nm[k] : 0);
        c->r[29].UW[0] = scratch - 256;
        guest_call(c, f_0018c500, db, (u32)i, key, scratch + 64 * i);
    }
    if (trace_rookie()) fprintf(stderr, "draft: %08x <- %s %s (%s)\n", key, get("first") ? get("first")->c_str() : "?", get("last") ? get("last")->c_str() : "?", kind ? "P" : "H");
    *c = saved;
}

// ---------------------------------------------------------------- database dump (.once "DUMPDB path")
// Every table of the game's database as it is now (a dynasty's season so far): for each table slot of the database
// object (*0x7d66f8 + 372 ...), its rows (the index at table + 16: 12-byte entries key, row, -; count at + 4) and the
// values the table's own getter gives (vtable at table + 32: this-adjust at + 40, function at + 44), attribute 0 ..
// the count given for the slot. Output: "slot key value value ..." lines.
#include <functional>
void kernel_queue_native(std::function<void(EEContext*)> fn);
static const struct { int off; int nattr; } kDbSlots[] = {
    {388, 55}, {392, 27}, {396, 27}, {400, 4}, {404, 12}, {408, 12}, {412, 4}, {416, 12}, {420, 12}, {424, 28}, {428, 14}, {432, 11}, {436, 11}, {440, 9}, {444, 14}, {448, 14}, {452, 14}, {456, 28}, {460, 14}, {464, 14}, {468, 14}, {472, 14}, {476, 14}, {480, 14}};
void db_dump_request(const std::string& path) {
    kernel_queue_native([path](EEContext* c) {
        FILE* f = fopen(path.c_str(), "w"); if (!f) return;
        const u32 db = ee_rd<u32>(0x7d66f8);
        fprintf(f, "# db %08x\n", db);
        if (!db) { fclose(f); return; }
        const char* only = std::getenv("MVP_DUMPDB_PROBE");
        // every table the database holds (slots 300..700; the known ones with their attribute counts, others 16)
        std::vector<std::pair<int,int>> slots;
        for (int off = 300; off < 700; off += 4) {
            int na = 16;
            for (const auto& k : kDbSlots) if (k.off == off) na = k.nattr;
            slots.push_back({off, na});
        }
        for (const auto& sp : slots) {
            const struct { int off; int nattr; } sl{sp.first, sp.second};
            const u32 tab = ee_rd<u32>(db + sl.off); if (!tab || tab >= 0x2000000 || (tab & 3)) continue;
            { const u32 vt0 = ee_rd<u32>(tab + 32); if (!vt0 || vt0 >= 0x2000000 || (vt0 & 3)) continue;
              const u32 fn0 = ee_rd<u32>(vt0 + 44); if (fn0 < 0x100000 || fn0 >= 0x1000000 || (fn0 & 3)) continue; }
            const u32 idx = ee_rd<u32>(tab + 16); if (!idx || idx >= 0x2000000) continue;
            const u32 ents = ee_rd<u32>(idx), n = ee_rd<u32>(idx + 4);
            const u32 vt = ee_rd<u32>(tab + 32);
            const s16 adj = ee_rd<s16>(vt + 40); const u32 fn = ee_rd<u32>(vt + 44);
            fprintf(f, "# slot %d table %08x rows %u getter %08x\n", sl.off, tab, n, fn);
            if (n > 20000) continue;
            const int na = only ? 1 : sl.nattr;
            for (u32 i = 0; i < n; i++) {
                const u32 key = ee_rd<u32>(ents + 12 * i), row = ee_rd<u32>(ents + 12 * i + 4);
                fprintf(f, "%d %08x", sl.off, key);
                for (int a = 0; a < na; a++) {
                    c->r[4].UD[0] = tab + adj; c->r[5].UD[0] = (u32)a; c->r[6].UD[0] = row; c->r[29].UD[0] = 0x0007fff0;
                    extern void ee_call(EEContext*, u32); ee_call(c, fn);
                    fprintf(f, " %d", c->r[2].SW[0]);
                }
                fputc('\n', f);
            }
        }
        fclose(f);
        fprintf(stderr, "dumpdb: %s\n", path.c_str());
    });
}

// tty write(buf, len) used by the game's printf: forward to the host console.
void hle_tty_write(EEContext* c) {
    u32 buf = c->r[4].UW[0], len = c->r[5].UW[0];
    std::string s;
    for (u32 i = 0; i < len && i < 4096; i++) s += (char)ee_rd<u8>(buf + i);
    fprintf(stderr, "[game] %s", s.c_str());
    if (!s.empty() && s.back() != '\n') fputc('\n', stderr);
    c->r[2].SD[0] = (s32)len;
}

// Movie clip "audio stream finished?" (0x501CD8). Streaming status comes from the IOP SND driver,
// which is not emulated yet, so report finished (movies are blank without the IPU anyway).
void hle_movie_audio_done(EEContext* c) { c->r[2].UD[0] = 1; }

// ---------------------------------------------------------------- wild pitches and passed balls in simulated games
// The simulation never throws a wild pitch or lets a pitch get by (pstats' wp stays 0), and simulated seasons
// scored about 8 % fewer runs than the real ones whose rates they match (1977: 4.10 runs a nine innings against
// 4.47). With "Simulated games: real league", before a plate appearance's outcome (func_00716ef8, a0 = the
// simulated game: +204 the bases, three runner records and +24 the scorer) a pitch may get away while runners are
// on: every runner moves up one base, a runner on third scores (func_007126f0: the game's own run event, earned
// or not as it decides). Real rate: about 0.45 wild pitches and passed balls a team game (0.6 before 1920), about
// 16 plate appearances a team game with someone on: 2.7 % (3.6 %) of those. sim.txt: wild_pitch = rate.
float g_wild_pitch = -1.0f;
void f_00716ef8_orig(EEContext* c); void f_007126f0(EEContext* c); void f_00192e18(EEContext* c); void f_00192e00(EEContext* c);
static u64 s_wp_seed = 0x9e3779b97f4a7c15ull;
static double wp_rand() { s_wp_seed ^= s_wp_seed << 13; s_wp_seed ^= s_wp_seed >> 7; s_wp_seed ^= s_wp_seed << 17; return (s_wp_seed >> 11) * (1.0 / 9007199254740992.0); }
static u64 s_wp_count = 0;
void hle_sim_pa(EEContext* c) {
    const u32 s = c->r[4].UW[0];
    static const bool trace_s = std::getenv("MVP_TRACE_SIMOBJ") != nullptr;
    if (trace_s) { static int n = 0; if (n++ < 5) fprintf(stderr, "[simobj] %08x game %08x bases %08x %08x %08x\n", s, game_of_obj(s), ee_rd<u32>(s + 204), ee_rd<u32>(s + 208), ee_rd<u32>(s + 212)); }
    if (g_sim_calibrate && s >= 0x100000 && s < 0x2000000) {
        const u32 b0 = ee_rd<u32>(s + 204), b1 = ee_rd<u32>(s + 208), b2 = ee_rd<u32>(s + 212), scorer = ee_rd<u32>(s + 228);
        auto ok = [](u32 p) { return p >= 0x100000 && p < 0x2000000; };
        if (ok(b0) && ok(b1) && ok(b2) && ok(scorer)) {
            const bool on1 = ee_rd<u8>(b0) != 0xff, on2 = ee_rd<u8>(b1) != 0xff, on3 = ee_rd<u8>(b2) != 0xff;
            const int year = g_last_date_year ? g_last_date_year : 2005 + g_year_shift;
            const double p = g_wild_pitch >= 0 ? g_wild_pitch : (year < 1920 ? 0.036 : 0.027);
            if ((on1 || on2 || on3) && wp_rand() < p) {
                GPR128 sr[32]; std::memcpy(sr, c->r, sizeof sr);
                auto call = [&](void (*fn)(EEContext*), u32 a0, u32 a1, u32 a2, u32 a3) { c->r[4].UD[0] = a0; c->r[5].UD[0] = a1; c->r[6].UD[0] = a2; c->r[7].UD[0] = a3; fn(c); };
                auto move = [&](u32 from, u32 to) { call(f_00192e18, to, ee_rd<u8>(from), ee_rd<u8>(from + 1), ee_rd<u32>(from + 4)); call(f_00192e00, from, 0, 0, 0); };
                if (on3) { call(f_007126f0, scorer, ee_rd<u8>(b2), 0, 0); call(f_00192e00, b2, 0, 0, 0); }
                if (on2) move(b1, b2);
                if (on1) move(b0, b1);
                std::memcpy(c->r, sr, sizeof sr);
                if (++s_wp_count <= 3 || std::getenv("MVP_TRACE_WP")) rlog("sim", "wild pitch / passed ball: runners %d%d%d move up", on1, on2, on3);
            }
        }
    }
    f_00716ef8_orig(c);
    static const bool trh = std::getenv("MVP_TRACE_HRF") != nullptr;
    if (trh) { extern float g_pa_hr_factor; static u64 n = 0, h = 0, ends = 0, all = 0; all++;
        const u32 out = ee_rd<u32>(s + 4);
        if (g_pa_hr_factor > 2.5f) { n++; if (out == 4) h++; if (out != 0) ends++; }
        if (all % 100000 == 0) rlog("hrf", "draws with factor>2.5: %llu, outcome set %llu, home runs %llu", (unsigned long long)n, (unsigned long long)ends, (unsigned long long)h); }
}

// Games a club plays in a season, by league (func_00196a48: 0, 1 the majors 162; AAA 144; the rest 140): the
// standings count games left from it (clinching, elimination, the races the season's end waits for). A league
// pack with a shorter schedule (tools/eras/era_schedule.py: 154 games before 1961) says so in its sim.txt
// (season_games 154), or every race stays open after the last day.
int g_season_games = 0;
void f_00196a48_orig(EEContext* c);
void hle_season_games(EEContext* c) {
    if (g_season_games > 0 && c->r[4].UW[0] < 2) { c->r[2].UD[0] = (u64)g_season_games; return; }
    f_00196a48_orig(c);
}

// Database key lookups (func_004b8568(index, &a, &b, key): a binary search over the index's 12-byte entries {key, a,
// b}, sorted by key; returns 1 and the first match's a, b). A simulated season makes some 600 000 of them a second
// (each player's records, func_00191480 from the daily player loop func_00700120): 10 % of the main thread. A host
// cache remembers where a key was found per index and checks that entry still holds the key (the index may grow or
// be re-sorted: then the game's own search runs again). Only first-of-run positions are cached.
void f_004b8568_orig(EEContext* c);
#include <unordered_map>
static std::unordered_map<u64, u32> s_key_pos;
void hle_db_lookup(EEContext* c) {
    const u32 idx = c->r[4].UW[0], pa = c->r[5].UW[0], pb = c->r[6].UW[0], key = c->r[7].UW[0];
    static const bool off = std::getenv("MVP_NO_DBCACHE") != nullptr;      // debug
    if (off) { f_004b8568_orig(c); return; }
    const u32 ents = ee_rd<u32>(idx), n = ee_rd<u32>(idx + 4);
    const u64 ck = ((u64)idx << 32) | key;
    auto it = s_key_pos.find(ck);
    if (it != s_key_pos.end() && it->second < n && ents >= 0x100000 && ents < 0x2000000) {
        const u32 p = it->second, e = ents + 12 * p;
        if (ee_rd<u32>(e) == key && (p == 0 || ee_rd<u32>(e - 12) != key) && (p + 1 >= n || ee_rd<u32>(e + 12) != key)) {   // a key of its own: the search finds just that one
            ee_wr<u32>(pa, ee_rd<u32>(e + 4)); ee_wr<u32>(pb, ee_rd<u32>(e + 8)); c->r[2].UD[0] = 1; return;
        }
    }
    f_004b8568_orig(c);
    if (c->r[2].UW[0] == 1 && n && ents >= 0x100000 && ents + 12 * n < 0x2000000) {
        u32 lo = 0, hi = n;                                   // first entry >= key (the index is sorted by key, signed)
        while (lo < hi) { const u32 m = (lo + hi) / 2; if ((s32)ee_rd<u32>(ents + 12 * m) < (s32)key) lo = m + 1; else hi = m; }
        if (lo < n && ee_rd<u32>(ents + 12 * lo) == key && (lo + 1 >= n || ee_rd<u32>(ents + 12 * lo + 12) != key) && ee_rd<u32>(ents + 12 * lo + 4) == ee_rd<u32>(pa) && ee_rd<u32>(ents + 12 * lo + 8) == ee_rd<u32>(pb)) {
            if (s_key_pos.size() > 400000) s_key_pos.clear();
            s_key_pos[ck] = lo;
        }
    }
}

// 0x1a2d80: reverse lookup in a {key, a, b} index -- returns the key of the first entry whose a
// (a2 == 0) or b (a2 != 0) equals a1, else 0. The guest does it as a linear scan calling a getter
// per entry; called ~200k times per sim day, so scan natively (same result, same order).
extern void f_001a2d80_orig(EEContext* c);
void hle_rev_lookup(EEContext* c) {
    static const bool off = std::getenv("MVP_NO_REVLOOKUP") != nullptr;   // debug
    if (off) { f_001a2d80_orig(c); return; }
    const u32 tab = ee_rd<u32>(c->r[4].UW[0]);
    const u32 ents = ee_rd<u32>(tab), n = ee_rd<u32>(tab + 4);
    const s64 want = c->r[5].SD[0];                                       // compared as the guest does: 64-bit register
    if (ents < 0x100000 || (u64)ents + 12ull * n > 0x2000000ull) { f_001a2d80_orig(c); return; }
    if ((s64)(s32)want != want) { c->r[2].SD[0] = 0; return; }            // no sign-extended word can equal it
    const u8* p = g_ram + ents + (c->r[6].SD[0] != 0 ? 8 : 4);
    s32 r = 0;
    for (u32 i = 0; i < n; i++, p += 12) {
        s32 v; std::memcpy(&v, p, 4);
        if ((s64)v == want) { r = ee_rd<s32>(ents + 12 * i); break; }
    }
    c->r[2].SD[0] = (s64)r;                                               // LW: sign-extended
}

// An era pack's uniforms follow the dynasty: in a 1977 dynasty that reaches 1984 the clubs wear what they wore in
// 1984 (mods/<pack>/uniform_years.txt, written by tools/eras/era_uniforms.py: club, first and last season, home
// and road slots of the game's own wardrobe in MODELS.BIG). The season is the year of the first game in the big
// league schedule; the slots of u<club>a/b and f<club>a/b are refilled from the disc when it changes.
#include "iop.h"
#include <fstream>
struct EraUni { int club, lo, hi; char home, road; };
struct EraPark { int club, lo, hi; std::string park, code, name; };   // the club's 2005 park, the season's park
static std::vector<EraPark> s_era_park;
static std::map<int, std::vector<std::string>> s_park_files;          // club -> its 2005 park's files
static std::map<int, std::string> s_park_2005;                       // club -> its 2005 park's name in the menus
static std::string park_file(const std::string& code) {               // an old park's first file (every variant gets it)
    for (const std::string& f : iso_list(("DATA/STADIUM/" + code).c_str())) if (f.size() > 4 && f.compare(f.size() - 4, 4, ".BIG") == 0) return f;
    return "";
}
static std::map<std::pair<int, std::string>, std::string> s_era_logo;   // (art, "city|nickname") -> "@file"
static std::vector<EraUni> s_era_uni;
static int s_era_uni_year = 0;
int era_teams_setup(const char* pack_dir);
int era_uniforms_setup(const char* pack_dir) {
    if (std::getenv("MVP_NO_ERAUNI")) return 0;                           // debug
    era_teams_setup(pack_dir);
    std::ifstream f(std::string(pack_dir) + "/uniform_years.txt"); std::string line;
    std::map<int, std::string> slots;                                     // club -> every slot it can wear
    while (std::getline(f, line)) {
        EraUni e; char h, r;
        if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%d %d %d %c %c", &e.club, &e.lo, &e.hi, &h, &r) != 5) continue;
        e.home = h; e.road = r; s_era_uni.push_back(e); slots[e.club] += h; slots[e.club] += r;
    }
    std::map<std::string, std::vector<std::string>> sw;
    char nm[32];
    for (int club = 0; club < 30; club++) slots[club];                    // every club: stand-ins become big-league clubs
    for (auto& [club, sl] : slots) {
        std::string all = sl + "ab";
        for (char kind : {'u', 'f'}) for (char dst : {'a', 'b'}) {
            std::snprintf(nm, sizeof nm, "%c%03d%c.ssh", kind, club, dst); std::vector<std::string>& v = sw[nm];
            for (char s : all) { char c[32]; std::snprintf(c, sizeof c, "%c%03d%c.ssh", kind, club, s); if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c); }
        }
    }
    const int n = iso_big_switchable("DATA/MODELS.BIG", sw);
    // ballparks (park_years.txt, tools/eras/era_parks.py: club, its 2005 park, seasons, the park then, its name)
    std::ifstream pf(std::string(pack_dir) + "/park_years.txt");
    while (std::getline(pf, line)) {
        if (line.size() > 2 && line[0] == '=') { int club; char nm[160]; if (std::sscanf(line.c_str(), "= %d %159[^\n]", &club, nm) == 2) s_park_2005[club] = nm; continue; }
        EraPark e; char park[16], code[16], name[160];
        if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%d %15s %d %d %15s %159[^\n]", &e.club, park, &e.lo, &e.hi, code, name) != 6) continue;
        e.park = park; e.code = code; e.name = name; s_era_park.push_back(e);
    }
    std::map<int, std::vector<std::string>> cands;
    for (const EraPark& e : s_era_park) { const std::string f = park_file(e.code); if (!f.empty()) cands[e.club].push_back(f);
        if (!s_park_files.count(e.club)) for (const std::string& d : iso_list(("DATA/STADIUM/" + e.park).c_str())) if (d.size() > 4 && d.compare(d.size() - 4, 4, ".BIG") == 0) s_park_files[e.club].push_back(d); }
    for (auto& [club, files] : s_park_files) for (const std::string& d : files) iso_file_switchable(d.c_str(), cands[club]);
    {   // the parks' names in the menus: the pack's (its season) or the 2005 one, with room for the others
        extern int g_era_year; extern int loc_track(const std::string&, const std::vector<std::string>&);
        for (auto& [club, nm] : s_park_2005) {
            std::vector<std::string> v{nm};
            for (const EraPark& e : s_era_park) if (e.club == club && e.lo <= g_era_year && g_era_year <= e.hi) v.push_back(e.name);
            loc_track("park" + std::to_string(club), v);
        }
    }
    // logos for the names a club takes (mods/era-logos/logos.txt, tools/eras/era_logo_years.py: "art file city|nickname")
    std::ifstream lf("mods/era-logos/logos.txt"); std::map<std::string, std::vector<std::string>> lsw;
    while (std::getline(lf, line)) {
        int art; char file[200], name[200];
        if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%d %199s %199[^\n]", &art, file, name) != 3 || art < 1 || art > 30) continue;
        const std::string path = std::string("@mods/era-logos/") + file;
        s_era_logo[{art, name}] = path;
        std::snprintf(nm, sizeof nm, "l%03d.ssh", art - 1); std::vector<std::string>& v = lsw[nm];
        if (v.empty()) v.push_back(nm);
        v.push_back(path);
    }
    if (!lsw.empty()) iso_big_switchable("DATA/FRONTEND/LOGOS.BIG", lsw);
    return n;
}
extern u32 g_sim_today;
static int season_from_schedule() {
    const u32 cal = ee_rd<u32>(0x7d66f8);
    if (cal < 0x100000 || cal >= 0x2000000) return 0;
    const u32 sc = ee_rd<u32>(cal + 12);
    if (sc < 0x100000 || sc >= 0x2000000) return 0;
    const u32 cnt = ee_rd<u32>(sc + 4), rec = ee_rd<u32>(sc + 8);
    if (!cnt || cnt > 20000 || rec < 0x100000 || rec >= 0x2000000) return 0;
    static int boot_day = -1;                                             // the placeholder schedule the game makes at boot: no dynasty yet
    if (boot_day < 0) boot_day = ee_rd<u16>(rec);
    if (ee_rd<u16>(rec) == boot_day) return 0;
    auto days = [](long y, long m, long d) { if (m < 3) { y--; m += 12; } return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d; };
    const long dn = (long)ee_rd<u16>(rec) + days(1947, 12, 31);           // the game's day number (days_from_date)
    long y = 1948; while (y < 2130 && days(y + 1, 1, 1) <= dn) y++;
    y += g_year_shift;
    static int last_dbg = -1; static u32 last_t = 1; if (std::getenv("MVP_ERADEBUG") && ((int)ee_rd<u16>(rec) != last_dbg || g_sim_today / 50 != last_t)) { last_t = g_sim_today / 50; last_dbg = ee_rd<u16>(rec); rlog("era", "schedule %08x: %u games, first day %u -> %ld (today %u)", sc, cnt, last_dbg, y, g_sim_today); }
    return (y >= 1871 && y <= 2200) ? (int)y : 0;
}
// Club names follow the dynasty too (mods/<pack>/team_years.txt, tools/eras/era_teamnames.py: slot, seasons,
// abbreviation|city|nickname): when the season changes, a big-league club whose entry differs between the old and
// the new season gets the new one in the dynasty's database (team table at database + 84; strings 1 nickname, 2 abbreviation, 3 city; number 2 the art id),
// through the table's own string setter, so the save keeps it. The Braves go to Milwaukee in 1953 and so on.
struct TeamYears { int art, lo, hi; std::string v; };
static std::vector<TeamYears> s_team_years;
static int s_dyn_year = 0;                                            // the last season read off a dynasty's schedule
static const std::string* team_entry(int art, int y) {
    for (const TeamYears& t : s_team_years) if (t.art == art && t.lo <= y && y <= t.hi) return &t.v;
    return nullptr;
}
int era_teams_setup(const char* pack_dir) {
    std::ifstream f(std::string(pack_dir) + "/team_years.txt"); std::string line;
    while (std::getline(f, line)) {
        TeamYears t; char rest[200];
        if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%d %d %d %199[^\n]", &t.art, &t.lo, &t.hi, rest) != 4) continue;
        t.v = rest; s_team_years.push_back(t);
    }
    extern int g_era_year;                                                // the menus' club names as the pack's season has them
    if (!s_team_years.empty() && g_era_year) {
        std::string nick[31], full[31];
        for (int art = 1; art <= 30; art++) if (const std::string* e = team_entry(art, g_era_year)) {
            const size_t a = e->find('|'), b = e->find('|', a + 1), c = e->find('|', b + 1);
            const std::string city = e->substr(a + 1, b - a - 1), nk = e->substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
            nick[art] = nk; full[art] = city + " " + nk;
        }
        extern int loc_set_club_names(const std::string*, const std::string*); loc_set_club_names(nick, full);
    }
    return (int)s_team_years.size();
}
static void era_teams_apply(int from, int to) {
    kernel_queue_native([from, to](EEContext* c) {
        const u32 db = ee_rd<u32>(0x7d66f8);
        if (!db) return;
        auto ok = [](u32 a) { return a >= 0x100000 && a < 0x2000000 && !(a & 3); };
        auto call = [&](u32 fn, u32 a0, u32 a1, u32 a2, u32 a3, u32 t0) {
            c->r[4].UD[0] = a0; c->r[5].UD[0] = a1; c->r[6].UD[0] = a2; c->r[7].UD[0] = a3; c->r[8].UD[0] = t0; c->r[29].UD[0] = 0x0007fff0;
            extern void ee_call(EEContext*, u32); ee_call(c, fn); return c->r[2].SW[0]; };
        // team.dat's table (its rows carry the art ids 1 .. 30 in attribute 6)
        u32 tab = 0, ents = 0, n = 0;
        for (int off : {84}) {                                              // func_0018d588 & co.: team.dat is the table at +84
            const u32 t = ee_rd<u32>(db + off); if (!ok(t)) continue;
            const u32 vt = ee_rd<u32>(t + 32), idx = ee_rd<u32>(t + 16); if (!ok(vt) || !ok(idx)) continue;
            const u32 fn = ee_rd<u32>(vt + 44), sfn = ee_rd<u32>(vt + 76); if (!ok(fn) || !ok(sfn) || fn >= 0x1000000 || sfn >= 0x1000000) continue;
            const u32 e = ee_rd<u32>(idx), cnt = ee_rd<u32>(idx + 4); if (!ok(e) || cnt < 30 || cnt > 400) continue;
            const s16 adj = ee_rd<s16>(vt + 40);
            int arts = 0;                                                // rows with an art id of a big-league slot (int attribute 2)
            for (u32 i = 0; i < cnt; i++) { const int a = call(fn, t + adj, 2, ee_rd<u32>(e + 12 * i + 4), 0, 0); arts += a >= 1 && a <= 30; }
            if (arts >= 16) { tab = t; ents = e; n = cnt; }
            else fprintf(stderr, "[era] table at +%d: %u rows, %d with a big-league art id\n", off, cnt, arts);
        }
        if (!tab) { fprintf(stderr, "[era] season %d: club names not changed (no team table found)\n", to); return; }
        const u32 vt = ee_rd<u32>(tab + 32); const s16 gadj = ee_rd<s16>(vt + 40), sadj = ee_rd<s16>(vt + 72);
        const u32 get = ee_rd<u32>(vt + 44), set = ee_rd<u32>(vt + 76);
        for (u32 i = 0; i < n; i++) {
            const u32 row = ee_rd<u32>(ents + 12 * i + 4);
            const int art = call(get, tab + gadj, 2, row, 0, 0);
            if (art < 1 || art > 30) continue;
            const std::string *a = team_entry(art, from), *b = team_entry(art, to);
            if (!b || (a && *a == *b)) continue;
            std::string parts[3]; size_t p = 0;
            for (int k = 0; k < 3; k++) { size_t q = b->find('|', p); parts[k] = b->substr(p, q == std::string::npos ? std::string::npos : q - p); p = q == std::string::npos ? b->size() : q + 1; }
            static const int attr[3] = {2, 3, 1};                         // string attributes: abbreviation, city, nickname
            for (int k = 0; k < 3; k++) {
                const u32 buf = 0x7e000 + 64 * k; std::string v = parts[k].substr(0, 40);
                for (size_t j = 0; j <= v.size(); j++) ee_wr<u8>(buf + (u32)j, j < v.size() ? (u8)v[j] : 0);
                call(set, tab + sadj, (u32)attr[k], row, buf, 0);
            }
            extern int loc_ram_set_club(int, const std::string&, const std::string&);
            const int locs = loc_ram_set_club(art, parts[2], parts[1] + " " + parts[2]);
            fprintf(stderr, "[era] season %d: club %d is now the %s %s (%s; menus %d)\n", to, art, parts[1].c_str(), parts[2].c_str(), parts[0].c_str(), locs);
        }
    });
}
// How long starters last in simulated games, by season (era packs with "2026 rules" off for the season, i.e. before
// 2015), set so complete games, the aces' innings and the league ERA come out near the real seasons (Lahman, AL + NL):
//  - drain: func_0071af18's tiring per batter (g_sim_drain, the game's 1.562 + random 0..1);
//  - pull: func_007199d0's stamina left below which the starter goes (g_sim_pull, the game's 0.3);
//  - keep / hold: chance per club and game that the manager lets a starter with a quarter of his stamina left bat
//    (no pinch hitter) / stay in (no pitching change) (hle_sim_ph_*, hle_sim_pitch_change);
//  - four-man rotations before 1975 (hle_rotation_next).
// Full simulated seasons (complete games / top innings / ERA, real in brackets):
//   1910  61.2 % / 310-321 / 3.15   (62.1 % / 318-370 / 2.77)     1927  49.3 % / 302-308 / 4.01   (48.4 % / 299-309 / 4.02)
//   1941  45.4 % / 299-301 / 3.82   (43.6 % / 288-343 / 3.89)     1951  39.4 % / 293-302 / 4.01   (37.9 % / 279-315 / 4.04)
//   1961  27.6 % / 296-314 / 4.00   (26.0 % / 264-283 / 4.03)     1969  23.5 % / 290-303 / 3.62   (25.2 % / 314-325 / 3.61)
//   1977  20.8 % / 267 top / 3.98   (21.6 % / 299-330 / 4.00)     1986  14.8 % / pace ~260 / 3.95 (13.8 % / 267-275 / 3.97)
//   1993   7.3 % / pace ~238 / 4.11 (8.2 % / 256-267 / 4.19)
// Without keep / hold the old eras stopped near 30 % (1951) and 52 % (1910): pinch hitters for tiring pitchers and
// late pitching changes, not tiring itself (lower drain changed nothing there). The game's own values gave 1977 3.7 %.
// Steal attempts by season (era packs with the CPU tuning on): func_00714f10's 0.3728 (g_sim_steal) scaled so the
// simulated seasons steal as the real ones did. The simulation stole about 1.0-1.5 % of plate appearances in every
// era (1910 1.2 %, 1951 0.9 %, 1986 1.4 %, 2012 1.05 %), the real leagues 0.9 % (1940s-60s) to 2.1 % (1986) and
// 3.5 % (1910). Attempts go with the value (success stays), but the game gives them to its fastest runners (attempt
// chance 0.3728 * (stealing_aggressive / 15)^2): at the 1910 total the leaders stole 150-170 (real 81, 70, 65), and
// in the slow 1950s it already gave Jethroe 73 (real 35). So each pack's value is the geometric mean of what its
// total and its top three needed (1910 the leaders': its totals count the extra bases the scorers called steals).
bool g_steal_by_season = false;                              // an era pack with the CPU tuning on (ee_runtime.cpp)
void sim_steal_for(int y) {
    static const struct { int y; float s; } T[] = {{1910, 0.50f}, {1927, 0.33f}, {1941, 0.25f}, {1951, 0.28f}, {1961, 0.30f},
        {1969, 0.39f}, {1977, 0.50f}, {1986, 0.75f}, {1993, 0.52f}, {1999, 0.51f}, {2004, 0.44f}, {2012, 0.50f}, {2019, 0.36f},
        {2022, 0.36f}, {2023, 0.55f}};
    extern float g_sim_steal;
    const int N = (int)(sizeof T / sizeof T[0]);
    float v = y <= T[0].y ? T[0].s : T[N - 1].s;
    for (int i = 0; i + 1 < N; i++) if (y > T[i].y && y <= T[i + 1].y) { const float t = (float)(y - T[i].y) / (float)(T[i + 1].y - T[i].y); v = T[i].s + t * (T[i + 1].s - T[i].s); break; }
    if (const char* e = std::getenv("MVP_STEAL")) v = (float)std::atof(e);      // debug
    g_sim_steal = v;
    // relievers' tiring (hle_sim_tire): what the relief appearances a start needed (real / simulated: 1969 1.60 / 1.40,
    // 1977 1.53 / 1.38, 1999 2.56 / 2.11, 2019 3.41 / 2.34)
    extern float g_sim_relief;
    g_sim_relief = y < 1970 ? 1.0f : y < 1985 ? 1.1f : y < 1995 ? 1.2f : y < 2005 ? 1.7f : y < 2015 ? 1.8f : 3.2f;
    if (const char* e = std::getenv("MVP_RELIEF")) g_sim_relief = (float)std::atof(e);   // debug
    extern float g_sim_spent; g_sim_spent = g_sim_relief > 1.25f ? 0.25f : -1.0f;
    if (const char* e = std::getenv("MVP_SPENT")) g_sim_spent = (float)std::atof(e);     // debug
}
bool g_sim_workload_by_season = false;
float g_sim_ph_keep = 0;                                     // chance a manager lets a fresh pitcher bat (hle_sim_ph_*)
int g_sim_rotation = 5;                                      // 4: the four-man rotation of the old eras (hle_rotation_next)
static void sim_workload_table(int y);
extern float g_sim_drain, g_sim_pull, g_sim_hold;
void sim_workload_for(int y) {
    sim_workload_table(y);
    if (const char* e = std::getenv("MVP_DRAIN")) g_sim_drain = (float)std::atof(e);      // debug
    if (const char* e = std::getenv("MVP_PULL")) g_sim_pull = (float)std::atof(e);        // debug
    if (const char* e = std::getenv("MVP_PH_KEEP")) g_sim_ph_keep = (float)std::atof(e);  // debug
    if (const char* e = std::getenv("MVP_HOLD")) g_sim_hold = (float)std::atof(e);        // debug
}
static void sim_workload_table(int y) {
    extern float g_sim_drain, g_sim_pull;
    g_sim_rotation = y < 1975 ? 4 : 5;
    if (const char* e = std::getenv("MVP_ROTATION")) g_sim_rotation = std::atoi(e);   // debug
    static const struct { int y; float drain, pull, keep, hold; } T[] = {
        {1910, 0.50f, 0.04f, 1.00f, 0.25f}, {1927, 0.62f, 0.06f, 0.90f, 0.02f}, {1941, 0.70f, 0.07f, 0.75f, 0.0f},
        {1950, 0.75f, 0.08f, 0.60f, 0.0f}, {1965, 1.10f, 0.13f, 0.15f, 0.0f}, {1977, 1.10f, 0.15f, 0.0f, 0.0f},
        {1986, 1.23f, 0.18f, 0.0f, 0.0f}, {1993, 1.40f, 0.23f, 0.0f, 0.0f}, {2000, 1.562f, 0.30f, 0.0f, 0.0f}};
    if (y >= 2000) { g_sim_drain = g_sim_pull = -1.0f; g_sim_ph_keep = g_sim_hold = 0; return; }   // the game's own
    if (y <= T[0].y) { g_sim_drain = T[0].drain; g_sim_pull = T[0].pull; g_sim_ph_keep = T[0].keep; g_sim_hold = T[0].hold; return; }
    const int N = (int)(sizeof T / sizeof T[0]);
    for (int i = 0; i + 1 < N; i++) if (y <= T[i + 1].y) {
        const float t = (float)(y - T[i].y) / (float)(T[i + 1].y - T[i].y);
        g_sim_drain = T[i].drain + t * (T[i + 1].drain - T[i].drain); g_sim_pull = T[i].pull + t * (T[i + 1].pull - T[i].pull);
        g_sim_ph_keep = T[i].keep + t * (T[i + 1].keep - T[i].keep); g_sim_hold = T[i].hold + t * (T[i + 1].hold - T[i].hold); return;
    }
}
void era_uniforms_poll() {
    if (s_era_uni.empty() && s_team_years.empty()) return;
    { extern void loc_ram_refresh(); loc_ram_refresh(); }
    extern int g_era_year;
    int y = season_from_schedule();
    if (y && y != s_dyn_year) {
        if (s_dyn_year && !s_team_years.empty()) era_teams_apply(s_dyn_year, y);
        else if (const char* e = std::getenv("MVP_TEAMS_TO")) era_teams_apply(y, std::atoi(e));   // debug: rename as for that season
        s_dyn_year = y;
        if (g_sim_workload_by_season) sim_workload_for(y);
        if (g_steal_by_season) sim_steal_for(y);
    }
    if (!y) y = g_era_year;                                               // exhibitions before any dynasty: the pack's season
    if (const char* e = std::getenv("MVP_UNIFORM_YEAR")) y = std::atoi(e);  // debug
    if (!y || y == s_era_uni_year) return;
    s_era_uni_year = y; int changed = 0, parknames = 0;
    std::map<int, std::pair<char, char>> pick;
    for (const EraUni& e : s_era_uni) if (e.lo <= y && y <= e.hi) pick[e.club] = {e.home, e.road};
    char m[32], c[32];
    for (int club = 0; club < 30; club++) {
        // the pack's own season: its uniforms and logos as built; a minor-league stand-in: the pack's plain set
        const std::string* te = team_entry(club + 1, y);
        const bool own = y == g_era_year, standin = te && te->size() > 2 && te->compare(te->size() - 2, 2, "|S") == 0;
        for (char kind : {'u', 'f'}) for (int k = 0; k < 2; k++) {
            std::snprintf(m, sizeof m, "%c%03d%c.ssh", kind, club, k ? 'b' : 'a');
            auto pk = pick.find(club);
            const char slot = pk != pick.end() ? (k ? pk->second.second : pk->second.first) : (k ? 'b' : 'a');
            std::snprintf(c, sizeof c, "%c%03d%c.ssh", kind, club, slot);
            changed += iso_big_switch("DATA/MODELS.BIG", m, own || standin ? "" : c);
        }
        std::snprintf(m, sizeof m, "l%03d.ssh", club);
        std::string logo = m;                                               // a name of the 2005 club's: its logo
        if (own || standin || !te) logo = "";
        else { size_t p1 = te->find('|'); std::string cn = te->substr(p1 + 1); if (cn.size() > 2 && cn.compare(cn.size() - 2, 2, "|S") == 0) cn.resize(cn.size() - 2);
               auto it = s_era_logo.find({club + 1, cn}); if (it != s_era_logo.end()) logo = it->second; }
        changed += iso_big_switch("DATA/FRONTEND/LOGOS.BIG", m, logo.c_str());
        // the park: the pack's own season as built; else the old park of that season, or the 2005 one
        auto pfs = s_park_files.find(club);
        if (pfs != s_park_files.end()) {
            std::string to = own ? "" : "=";
            for (const EraPark& e : s_era_park) if (e.club == club && e.lo <= y && y <= e.hi && !own) { const std::string f = park_file(e.code); if (!f.empty()) to = f; }
            for (const std::string& d : pfs->second) changed += iso_file_switch(d.c_str(), to.c_str());
            std::string nm = s_park_2005.count(club) ? s_park_2005[club] : "";
            for (const EraPark& e : s_era_park) if (e.club == club && e.lo <= y && y <= e.hi) nm = e.name;
            extern int loc_ram_set(const std::string&, const std::string&);
            if (!nm.empty()) parknames += loc_ram_set("park" + std::to_string(club), nm);
        }
    }
    rlog("era", "season %d: uniforms, logos and parks of that year (%d files changed, %d park names)", y, changed, parknames);
}

// The rotation (part of a club's 480-byte record): +0 the pitchers' keys, +20 the slots (bytes, indexes into
// those; +24 how many, 5 from SP1-SP5), +32 the slot whose turn it is. After every game func_001b5d98 moves both
// clubs on one slot (func_00749510: (+32 + 1) % slots), whatever the rest (func_006e3028 only picks the slot to
// open a schedule with: the first of three who is back to 90 % of his stamina), so the five go round in turn and
// nobody makes more than 31-33 starts. Before 1975 (g_sim_rotation 4) clubs went with four: when the turn comes
// to the one of the five with the least stamina (the spot starter) and the next one will be rested by tomorrow
// (90 % of his stamina, attribute 27 of 0, after a day's rest: + 15 % of full + 10, as func_0074e690 projects),
// the turn passes to the next one, twice in a row: he takes every third turn of his (about 12 starts a season, the
// four 36-38; 1961 without that: the four 41 starts, 300+ innings), or any when the four are tired.
void f_00749510_orig(EEContext* c);
void hle_rotation_next(EEContext* c) {
    const u32 rot = c->r[4].UW[0];
    f_00749510_orig(c);
    if (g_sim_rotation != 4 || ee_rd<u32>(rot + 24) != 5) return;
    const u32 list = ee_rd<u32>(rot), slots = ee_rd<u32>(rot + 20), cnt = ee_rd<u32>(rot + 4), turn = ee_rd<u32>(rot + 32);
    if (turn >= 5 || list < 0x100000 || slots < 0x100000) return;
    GPR128 sr[32]; FPR sf[32]; std::memcpy(sr, c->r, sizeof sr); std::memcpy(sf, c->f, sizeof sf);
    extern void ee_call(EEContext*, u32);
    const u32 db = ee_rd<u32>(0x7d66f8);
    u32 key[5]; int full[5], now[5]; bool ok = true;
    for (int i = 0; i < 5 && ok; i++) {
        const u8 sl = ee_rd<u8>(slots + i);
        if (sl == 255 || sl >= cnt) { ok = false; break; }
        key[i] = ee_rd<u32>(list + 4 * sl);
        c->r[4].UD[0] = db; c->r[5].UD[0] = 0; c->r[6].UD[0] = key[i]; ee_call(c, 0x18c828); full[i] = (int)c->r[2].SW[0];
        c->r[4].UD[0] = db; c->r[5].UD[0] = 27; c->r[6].UD[0] = key[i]; ee_call(c, 0x18c828); now[i] = (int)c->r[2].SW[0];
        if (full[i] <= 0) ok = false;
    }
    std::memcpy(c->r, sr, sizeof sr); std::memcpy(c->f, sf, sizeof sf);
    if (!ok) return;
    int spot = 0; for (int i = 1; i < 5; i++) if (full[i] < full[spot]) spot = i;
    if ((int)turn != spot) return;
    const int nx = (turn + 1) % 5;
    int next = now[nx] + (int)((float)full[nx] * 0.15f) + 10; if (next > full[nx]) next = full[nx];
    static std::map<u32, int> skips;                          // a club's turns in a row without its spot starter
    if ((float)next / (float)full[nx] >= 0.9f && skips[rot] < 2) {
        skips[rot]++;
        ee_wr<u32>(rot + 32, nx);
        static int told = 0; if (++told <= 3) rlog("sim", "four-man rotation: spot starter skipped");
    } else skips[rot] = 0;
}

// How simulated starters leave, and the managers of the complete-game eras. func_00717980 asks before every batter,
// for both clubs: func_007199d0 for the club number game + 80 (the club at bat during a plate appearance, as the
// home-run factors showed; called "change" below) and func_007196c0 for the other one ("pinch hitter" below; which of
// the two is the pinch hitter and which the pitching change is not settled, the stamina read is each one's own club)
// ( its rule sets func_00751dc0 / 00751f38 / 00751fc8 by inning, score, stamina
// left and a random number say 1, func_0071a050 makes the change). A club's first change takes its starter out.
// Traced (MVP_TRACE_SUBS) in 1951: 87 % of starters went for pinch hitters in the 6th-8th inning with 10-40 % of
// their stamina left (game + 80 + club * 24: now +24, full +28, as func_00752270 reads it); in 1910, once those
// batted, for the fielding club's late changes with 20-70 % left. Complete games came out at 30 % (real 38 %) and 52 %
// (real 62 %). Here, while a club's starter is still in and has a quarter of his stamina or more:
//  - a pinch hitter for him (rule sets' yes) becomes a no, for the whole game with chance g_sim_ph_keep;
//  - for the change, he counts as fresh (full stamina for the decision), for the whole game with chance g_sim_hold.
// One roll each per club and game (asked before every batter, a no re-rolled each time becomes a yes a little later:
// that moved the pinch hits for pitchers only from 5.0 % to 4.8 % of batters). Both by season (sim_workload_for).
u64 g_dbg_rule_yes, g_dbg_changes, g_dbg_asked, g_dbg_rest, g_dbg_avail;
void f_007199d0_orig(EEContext* c); void f_007196c0_orig(EEContext* c);
void f_00751dc0_orig(EEContext* c); void f_00751f38_orig(EEContext* c); void f_00751fc8_orig(EEContext* c);
float g_sim_hold = 0;
float g_sim_one_inning = 0;                                   // chance a reliever goes out at a new inning (2026 rules)
int g_sim_bf_cap = 0;                                         // batters a starter faces at most (0: no cap; 2026 rules)
float g_sim_relief = 1.0f;
float g_sim_closer = 1.6f;                                     // the closer's (hle_sim_tire)                                     // relievers' tiring times this (hle_sim_tire)
bool g_opt_holdlate = false;                                  // patches.txt holdlate, on during a held starter's call
u32 g_sim_games = 0;
struct StarterState { u8 inning; bool gone, keep_ph, hold; u8 slot; u16 bf; u8 rel_slot = 255, rel_inn = 0, rel_roll = 0; };
// slot: the starter's (club + 380 at the first look); bf: batters he faced; rel_*: the reliever in, his first inning,
// the inning last rolled for (hle_sim_hit_for_p)
static u32 era_rng() { static u32 r = 0x9e3779b9u; r = r * 1664525u + 1013904223u; return r >> 8; }
// the club's state in the game (a game's block is used again by later games: an earlier inning than last time is
// a new game)
static StarterState* starter_state(u32 game, u32 team, u32* st_out = nullptr) {
    const u32 st = ee_rd<u32>(ee_rd<u32>(game + 8) + 20); if (st < 0x100000 || team > 1) return nullptr;
    if (st_out) *st_out = st;
    static std::map<u64, StarterState> m; const u64 k = ((u64)st << 1) | team; const u8 inn = ee_rd<u8>(st + 7368);   // (by the game's club records: one engine block serves games in turn)
    auto it = m.find(k);
    if (it == m.end() || inn < it->second.inning) {
        extern u32 g_sim_games; g_sim_games++;                // (both clubs, all levels)
        static const bool dbg = std::getenv("MVP_TRACE_NEWGAME") != nullptr;
        if (dbg) { static u32 h[16][16], n; if (it != m.end()) h[std::min<int>(it->second.inning, 15)][std::min<int>(inn, 15)]++; else h[15][15]++;
            if (++n % 2000 == 0) { std::string a; for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) if (h[i][j]) a += " " + std::to_string(i) + ">" + std::to_string(j) + ":" + std::to_string(h[i][j]); rlog("newgame", "%s", a.c_str()); } }
        m[k] = {inn, false, era_rng() / 16777216.0f < g_sim_ph_keep, era_rng() / 16777216.0f < g_sim_hold, ee_rd<u8>(st + team * 3684 + 380), 0}; it = m.find(k);
    }
    it->second.inning = inn; return &it->second;
}
static float stamina_left(u32 game, u32 team) {
    const u32 b = game + 80 + team * 24; float now, full; u32 w = ee_rd<u32>(b + 24); std::memcpy(&now, &w, 4); w = ee_rd<u32>(b + 28); std::memcpy(&full, &w, 4);
    return full > 0 ? now / full : 0.0f;
}
static u32 batting_club(u32 st) { return ee_rd<u32>(st + 7376) != 0 ? 0 : 1; }
static const float s_fresh = std::getenv("MVP_PH_RATIO") ? (float)std::atof(std::getenv("MVP_PH_RATIO")) : 0.25f;   // debug
// debug: the inning and stamina left when starters go, by kind (0 change, 1 pinch hitter)
static u64 s_start_out[2][16], s_start_ratio[2][11], s_starts, s_sub_n[2][2];
static void starter_gone(u32 game, u32 team, int kind, StarterState* s) {
    if (!s || s->gone) return;
    s->gone = true;
    static const bool on = std::getenv("MVP_TRACE_SUBS") != nullptr; if (!on) return;
    s_starts++; s_start_out[kind][std::min<int>(s->inning, 15)]++;
    s_start_ratio[kind][std::max(0, std::min(10, (int)(stamina_left(game, team) * 10)))]++;
    if (s_starts % 1000 == 0) { std::string a;
        for (int kd = 0; kd < 2; kd++) { a += kd ? " | hit for: inn" : "change: inn"; for (int i = 0; i < 16; i++) a += " " + std::to_string(s_start_out[kd][i]);
            a += " ratio"; for (int i = 0; i < 11; i++) a += " " + std::to_string(s_start_ratio[kd][i]); }
        rlog("starters", "%llu starters out; %s | changes %llu pinch hitters %llu", (unsigned long long)s_starts, a.c_str(),
             (unsigned long long)s_sub_n[0][1], (unsigned long long)s_sub_n[1][1]); }
}
void hle_sim_pitch_change(EEContext* c) {
    const u32 g = c->r[4].UW[0]; const u32 team = ee_rd<u32>(g + 80);
    StarterState* s = g_sim_hold > 0 || g_sim_ph_keep > 0 || g_sim_relief > 1.0f || std::getenv("MVP_TRACE_SUBS") ? starter_state(g, team) : nullptr;
    // hold: the game decides as usual (its forced changes too), but sees him at full stamina and without its late
    // rule (patches.txt holdlate: from the 7th inning a club not ahead changes); his own stamina comes back unless he
    // was changed. (Not asking func_007199d0 at all instead left a club's game record with garbage lists the next
    // set-up cleared past RAM: two 1910 seasons crashed, "write outside RAM", ra 0x5ae9f4; see hle_club_setup.)
    const u32 sb = g + 80 + team * 24;
    if (s && !s->gone && s->hold && stamina_left(g, team) >= s_fresh) {
        const u32 now = ee_rd<u32>(sb + 24), full = ee_rd<u32>(sb + 28);
        ee_wr<u32>(sb + 24, full);
        g_opt_holdlate = true; f_007199d0_orig(c); g_opt_holdlate = false;
        if (c->r[2].UW[0] == 1) { s_sub_n[0][1]++; starter_gone(g, team, 0, s); }
        else ee_wr<u32>(sb + 24, now);
        return;
    }
    f_007199d0_orig(c);
    if (c->r[2].UW[0] == 1) { s_sub_n[0][1]++; starter_gone(g, team, 0, s); }
}
void hle_sim_hit_for_p(EEContext* c) {
    const u32 g = c->r[4].UW[0];
    u32 st = 0; StarterState* s = nullptr;
    if (g_sim_hold > 0 || g_sim_ph_keep > 0 || g_sim_relief > 1.0f || std::getenv("MVP_TRACE_SUBS")) { const u32 s0 = ee_rd<u32>(ee_rd<u32>(g + 8) + 20); if (s0 >= 0x100000) s = starter_state(g, batting_club(s0), &st); }
    // today's hook: a starter who has faced g_sim_bf_cap batters is seen as spent (a quarter of his stamina or less:
    // the rule sets take him out, any reliever comes); his stamina comes back if he stays
    u32 sb = 0, now = 0;
    if (s && st && !s->gone && g_sim_bf_cap > 0 && s->bf >= g_sim_bf_cap) {
        const u32 team = batting_club(st);
        if (ee_rd<u8>(st + team * 3684 + 380) == s->slot) {
            sb = g + 80 + team * 24; now = ee_rd<u32>(sb + 24); float full, v; u32 w = ee_rd<u32>(sb + 28); std::memcpy(&full, &w, 4);
            float cur; std::memcpy(&cur, &now, 4); v = std::min(cur, full * 0.15f); std::memcpy(&w, &v, 4); ee_wr<u32>(sb + 24, w);
        }
    }
    // today's one-inning relievers: a reliever into a new inning is seen as spent with chance g_sim_one_inning (one
    // roll an inning; relievers went 1.20 innings an appearance, real 2025 0.98)
    if (!sb && s && st && s->gone && g_sim_one_inning > 0) {
        const u32 team = batting_club(st); const u8 cur = ee_rd<u8>(st + team * 3684 + 380);
        if (cur != s->slot) {
            if (cur != s->rel_slot) { s->rel_slot = cur; s->rel_inn = s->rel_roll = s->inning; }
            else if (s->inning > s->rel_inn && s->inning != s->rel_roll) {
                s->rel_roll = s->inning;
                if (era_rng() / 16777216.0f < g_sim_one_inning) {
                    sb = g + 80 + team * 24; now = ee_rd<u32>(sb + 24); float full, v; u32 w = ee_rd<u32>(sb + 28); std::memcpy(&full, &w, 4);
                    float cv; std::memcpy(&cv, &now, 4); v = std::min(cv, full * 0.15f); std::memcpy(&w, &v, 4); ee_wr<u32>(sb + 24, w);
                }
            }
        }
    }
    g_dbg_asked++; f_007196c0_orig(c);
    if (sb && c->r[2].UW[0] != 1) ee_wr<u32>(sb + 24, now);
    if (c->r[2].UW[0] == 1) g_dbg_changes++;
    if (c->r[2].UW[0] == 1) { s_sub_n[1][1]++; if (st) starter_gone(g, batting_club(st), 1, s); }
}
static void ph_filter(EEContext* c, u32 game) {
    if (c->r[2].UW[0] == 1) g_dbg_rule_yes++;
    if (c->r[2].UW[0] != 1 || g_sim_ph_keep <= 0) return;
    const u32 s0 = ee_rd<u32>(ee_rd<u32>(game + 8) + 20); if (s0 < 0x100000) return;
    const u32 team = batting_club(s0);
    StarterState* s = starter_state(game, team);
    if (s && !s->gone && s->keep_ph && stamina_left(game, team) >= s_fresh) c->r[2].UD[0] = 0;
}
void hle_sim_ph_a(EEContext* c) { const u32 g = c->r[4].UW[0]; f_00751dc0_orig(c); ph_filter(c, g); }
void hle_sim_ph_b(EEContext* c) { const u32 g = c->r[4].UW[0]; f_00751f38_orig(c); ph_filter(c, g); }
void hle_sim_ph_c(EEContext* c) { const u32 g = c->r[4].UW[0]; f_00751fc8_orig(c); ph_filter(c, g); }

// Debug (MVP_TRACE_FORCED): func_00717278, asked before every simulated outcome draw (func_00716ef8), returns 1
// when it has made the outcome itself (a home run, func_007190c8, or func_00751d08) for a trailing club.
void f_00717278_orig(EEContext* c);
void hle_sim_forced(EEContext* c) {
    const u32 s = c->r[4].UW[0];
    f_00717278_orig(c);
    static const bool tr = std::getenv("MVP_TRACE_FORCED") != nullptr; if (!tr) return;
    static u64 n = 0, yes = 0, hr = 0; n++;
    if (c->r[2].UW[0] == 1) { yes++; if (ee_rd<u32>(s + 4) == 4) hr++; }
    if (n % 20000 == 0) rlog("forced", "asked %llu made %llu (home runs %llu); flag %u", (unsigned long long)n, (unsigned long long)yes, (unsigned long long)hr,
                              ee_rd<u32>(ee_rd<u32>(ee_rd<u32>(s + 8) + 12) + 140));
}

// A club's game set-up (func_001aeae8) first clears five byte lists of its record (+0, +20, +40, +60, +80: pointer,
// count, capacity at +8). A list whose pointer and capacity run past RAM crashed two 1910 seasons on their first
// days ("write outside RAM" 0x2000000, ra 0x5ae9f4). Logged and made empty instead.
void f_001aeae8_orig(EEContext* c);
void hle_club_setup(EEContext* c) {
    const u32 r = c->r[4].UW[0];
    if (r >= 0x100000 && r < 0x2000000)
        for (int i = 0; i < 5; i++) {
            const u32 l = r + 20 * i, ptr = ee_rd<u32>(l), cap = ee_rd<u32>(l + 8);
            if (cap && ((u64)ptr + cap > 0x2000000ull || ptr < 0x100000)) {
                rlog("sim", "club set-up: list %d of %08x bad (pointer %08x count %u capacity %u, ra %08x): emptied", i, r, ptr, ee_rd<u32>(l + 4), cap, c->r[31].UW[0]);
                ee_wr<u32>(l + 4, 0); ee_wr<u32>(l + 8, 0);
            }
        }
    f_001aeae8_orig(c);
}

// Relievers' tiring (func_0071af18: the pitching club is game + 84, its stamina game + 80 + club * 24 + 24, less
// g_sim_drain + random(0..1) a batter). Today's bullpens go an inning at a time: a simulated 2026 had its relievers
// at 1.56 innings an appearance (real 2025 0.98), its busiest at 91-97 games and 130-157 innings (real top 80 games,
// 74 innings), 2.1 relief appearances a start (real 3.3). Once a club's starter is out (StarterState.gone) its pitcher
// tires g_sim_relief times as fast (by season, sim_relief_for).
void f_0071af18_orig(EEContext* c);
void hle_sim_tire(EEContext* c) {
    const u32 g = c->r[4].UW[0];
    if (g_sim_relief <= 1.0f || g < 0x100000 || g >= 0x2000000) { f_0071af18_orig(c); return; }
    const u32 team = ee_rd<u32>(g + 84);
    StarterState* s = team < 2 ? starter_state(g, team) : nullptr;
    const u32 b = g + 80 + team * 24 + 24;
    float before; u32 w = ee_rd<u32>(b); std::memcpy(&before, &w, 4);
    f_0071af18_orig(c);
    if (!s) return;
    const u32 cl = ee_rd<u32>(g + 12) + team * 3684; const u8 slot = ee_rd<u8>(cl + 380);
    static const bool dbg = std::getenv("MVP_TRACE_TIRE") != nullptr;
    if (dbg) { static u64 n[4]; n[(s->gone ? 2 : 0) + (slot != s->slot ? 1 : 0)]++;
        if ((n[0] + n[1] + n[2] + n[3]) % 20000 == 0) { extern u64 g_dbg_rule_yes, g_dbg_changes, g_dbg_asked;
            extern u32 g_sim_games; extern u64 g_dbg_rest, g_dbg_avail; extern size_t relief_keys(); extern u32 g_sim_today; rlog("tire", "today %u games %u keys %zu avail %llu rest-no %llu", g_sim_today, g_sim_games, relief_keys(), (unsigned long long)g_dbg_avail, (unsigned long long)g_dbg_rest);
            rlog("tire", "in/starter %llu in/other %llu gone/starter %llu gone/other %llu | asked %llu rule yes %llu changes %llu",
            (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3], (unsigned long long)g_dbg_asked, (unsigned long long)g_dbg_rule_yes, (unsigned long long)g_dbg_changes); } }
    if (slot == s->slot) { s->bf++; return; }          // the starter (s->gone alone also caught him: a club's first change)
    { extern void relief_day(u32); relief_day(ee_rd<u32>(cl + 1000 + 52 * slot)); }
    float after; w = ee_rd<u32>(b); std::memcpy(&after, &w, 4);
    if (!(before > after)) return;
    // the closer (group 4: club + 80 list, + 84 count) less so: he finishes his inning (at 3.2 for all, top saves 27,
    // real 2025 42)
    float f = g_sim_relief;
    { const u32 rec = ee_rd<u32>(ee_rd<u32>(g + 8) + 20) + team * 3684, n = ee_rd<u32>(rec + 84), lst = ee_rd<u32>(rec + 80);
      if (n && n < 8 && lst >= 0x100000 && lst < 0x2000000) for (u32 i = 0; i < n; i++) if (ee_rd<u8>(lst + i) == slot) { f = std::min(f, g_sim_closer); break; } }
    float v = before - (before - after) * f; if (v < 0) v = 0;
    std::memcpy(&w, &v, 4); ee_wr<u32>(b, w);
    // and the database's (attribute 27, which carries to the next days: func_0019c6c8(club + 3188, 27, slot, value)
    // as func_0071af18 does, club = game + 12 -> + club * 3684, slot byte at club + 380)
    GPR128 sr[32]; FPR sf[32]; std::memcpy(sr, c->r, sizeof sr); std::memcpy(sf, c->f, sizeof sf);
    extern void ee_call(EEContext*, u32);
    c->r[4].UD[0] = cl + 3188; c->r[5].UD[0] = 27; c->r[6].UD[0] = slot; c->r[7].SD[0] = (s64)(s32)v; ee_call(c, 0x19c6c8);
    std::memcpy(c->r, sr, sizeof sr); std::memcpy(c->f, sf, sizeof sf);
}

// A simulated club's reliever pick (func_0071a558: a0 game, a1 group, a2 out list, a3 club, t0 1 = tired ones too).
// func_0071a050 first looks by the inning and score (closer, setup man, the others) for a rested man (func_007520a8:
// stamina left 85 %+), and a starter gets no more: with none rested he stays in. Then (a reliever pitching, or his
// starter spent) groups 1 2 3 0 4 (1 2 3 4 0 late), rested, then tired ones too; group 0 is the rotation, whose men
// are back to 90 % after a day. With today's relievers tiring (hle_sim_tire) the rotation came up first (starters
// 5.7 relief appearances each, real 2025 0.2, and 230 innings) and with no reliever back to 85 % starters stayed in
// (complete games 3 % of starts, real 0.6 %); taking tired ones in the first look had relievers in 157 games.
// From 1995: the rotation is asked only for a pitcher hurt or thrown out, the closer before it; a starter with less
// than g_sim_spent of his stamina left gets any reliever (patches.txt); and a reliever with less than g_sim_floor
// left is not used at all (taking any tired one: relievers in 136 games, 4.2 relief appearances a start).
// (g_sim_rested, debug: what the first look counts as rested, 0: the game's 85 %; 50 % had relievers in 120 games.)
float g_sim_rested = 0.0f, g_sim_spent = -1.0f, g_sim_floor = 0.5f;
static bool s_first_look = false, s_tired_pass = false;
bool g_sim_keep_closer = true;
static bool s_relax = false;                                   // hle_sim_change's second try
void f_0071a558_orig(EEContext* c); void f_007520a8_orig(EEContext* c);
void hle_sim_pick(EEContext* c) {
    const bool on = g_sim_relief > 1.25f;
    if (on && c->r[5].UW[0] == 0) {
        const u32 g = c->r[4].UW[0], club = c->r[7].UW[0];
        const u32 cl = ee_rd<u32>(ee_rd<u32>(g + 8) + 20) + club * 3684; const u8 slot = ee_rd<u8>(cl + 380);
        const u32 st = slot < 255 ? ee_rd<u32>(cl + 1004 + 52 * slot) : 0;
        if (st != 1 && st != 3) return;
        const u32 a2 = c->r[6].UW[0]; const u32 before = ee_rd<u32>(a2 + 4);
        GPR128 sr[32]; std::memcpy(sr, c->r, sizeof sr);
        c->r[5].UD[0] = 4; c->r[8].UD[0] = 1; f_0071a558_orig(c);   // the closer first
        const u32 after = ee_rd<u32>(a2 + 4); std::memcpy(c->r, sr, sizeof sr);
        if (after != before) return;
    }
    const u32 ra = c->r[31].UW[0];
    const bool first = ra == 0x71a20c || ra == 0x71a230 || ra == 0x71a24c || ra == 0x71a270 || ra == 0x71a290;
    // the closer (group 4) is kept for the ninth and later, or the first look's save spots (closers pitched 75 games,
    // 95 innings, with 25-31 saves: real 2025 top 42, closers about 65 games and innings)
    if (on && c->r[5].UW[0] == 4 && !first && g_sim_keep_closer) {
        const u32 st = ee_rd<u32>(ee_rd<u32>(c->r[4].UW[0] + 8) + 20);
        if (st >= 0x100000 && st < 0x2000000 && ee_rd<u8>(st + 7368) < 8) return;
    }
    s_first_look = on && g_sim_rested > 0 && first;
    s_tired_pass = on && c->r[8].UW[0] == 1;
    f_0071a558_orig(c);
    s_first_look = s_tired_pass = false;
}
// Rest days: a reliever who pitched on g_sim_win of the g_sim_win_days days before (2 of 3) is not used (the day:
// the calendar's, hle_tiebreak_check; counting games begun / 30 instead, the minor leagues' made days 3 times too short). Not on the third day in a row alone, or not on 3 of
// 5: the busiest relievers pitched in 93-95 games (real 2019 / 2025 top 83 / 81).
struct ReliefDays { u32 last, mask; };                   // mask bit i: pitched on day last - i
static std::unordered_map<u32, ReliefDays> s_relief_days;
size_t relief_keys() { return s_relief_days.size(); }
static u32 sim_day() { extern u32 g_sim_today, g_sim_games; return g_sim_today ? g_sim_today : g_sim_games / 90; }   // (the calendar's day; games begun count the minor leagues too)
void relief_day(u32 key) {
    if (g_sim_relief <= 1.25f) return;
    const u32 d = sim_day(); ReliefDays& r = s_relief_days[key];
    const u32 gap = d - r.last; r.mask = (r.mask && gap < 32 ? r.mask << gap : 0) | 1; r.last = d;
}
int g_sim_max_run = 2, g_sim_win = 2, g_sim_win_days = 3;
void hle_sim_avail(EEContext* c) {          // func_007520a8(game, club, slot, f12 rested if this much left): 0 rested, 1 tired, 2 not
    if (g_sim_relief > 1.25f && g_sim_max_run > 0 && !s_relax) { g_dbg_avail++;
        const u32 g = c->r[4].UW[0], club = c->r[5].UW[0], slot = c->r[6].UW[0] & 0xff;
        const u32 cl = ee_rd<u32>(ee_rd<u32>(g + 8) + 20) + club * 3684;
        if (slot < 255) { auto it = s_relief_days.find(ee_rd<u32>(cl + 1000 + 52 * slot));
            if (it != s_relief_days.end()) {
                const u32 gap = sim_day() - it->second.last;
                const u32 m = gap == 0 ? it->second.mask : gap < 32 ? it->second.mask << gap : 0;   // bit i: i days ago
                const u32 run = (m >> 1) & ((1u << g_sim_max_run) - 1);
                if (run == (1u << g_sim_max_run) - 1 || (g_sim_win > 0 && __builtin_popcount((m >> 1) & ((1u << g_sim_win_days) - 1)) >= g_sim_win)) { extern u64 g_dbg_rest; g_dbg_rest++; c->r[2].UD[0] = 2; return; }
            } }
    }
    if (s_first_look) c->f[12].f = g_sim_rested;
    if (s_tired_pass && g_sim_floor > 0 && !s_relax) {
        GPR128 sr[32]; std::memcpy(sr, c->r, sizeof sr); const float f12 = c->f[12].f;
        c->f[12].f = g_sim_floor; f_007520a8_orig(c);
        const u32 v = c->r[2].UW[0]; std::memcpy(c->r, sr, sizeof sr); c->f[12].f = f12;
        if (v == 1) { c->r[2].UD[0] = 2; return; }
    }
    f_007520a8_orig(c);
}

// A simulated pitching change (func_0071a050: returns 1 when made). When none is made for a starter with less than
// g_sim_spent of his stamina left (no reliever rested, none past the rest days and the 50 % floor), a second try
// without those (complete games 1.4 % of starts, real 2025 0.6 %: the starters left in for want of an arm).
void f_0071a050_orig(EEContext* c);
u64 g_dbg_relax[2];
void hle_sim_change(EEContext* c) {
    GPR128 sr[32]; std::memcpy(sr, c->r, sizeof sr);
    f_0071a050_orig(c);
    if (c->r[2].UW[0] == 1 || g_sim_relief <= 2.0f || g_sim_spent <= 0) return;      // (from 2015)
    const u32 g = sr[4].UW[0];
    const u32 team = ee_rd<u32>(g + 84); if (team > 1) return;
    if (stamina_left(g, team) >= g_sim_spent) return;
    StarterState* s = starter_state(g, team); const u32 st = ee_rd<u32>(ee_rd<u32>(g + 8) + 20);
    if (!s || ee_rd<u8>(st + team * 3684 + 380) != s->slot) return;     // starters only (for relievers too: relievers
                                                                        // in 134 games, 4.2 relief appearances a start)
    std::memcpy(c->r, sr, sizeof sr);
    s_relax = true; f_0071a050_orig(c); s_relax = false;
    g_dbg_relax[c->r[2].UW[0] == 1]++;
}

// Loading: the game's disc module (func_005a6628 and its neighbours, 0x5a6628-0x5a7c00) waits between polls of the
// drive with func_005a65c0(hsyncs): a sema and an alarm, 480 hsyncs (30 ms) after every read, often twice; 7200
// while it retries. A real drive needed that long; ours answers at once (a read is done when the call returns),
// so the waits were pure delay: 40 directory reads at boot took 2.5 s. They are made 4 hsyncs (a quarter of a
// millisecond: the thread still blocks and the others run). MVP_CD_DELAY=0 keeps the game's.
void f_005a65c0_orig(EEContext* c);
u64 g_cd_delays_cut = 0;
void hle_cd_delay(EEContext* c) {
    static const bool keep = std::getenv("MVP_CD_DELAY") && std::getenv("MVP_CD_DELAY")[0] == '0';
    const u32 ra = c->r[31].UW[0];
    if (!keep && ra >= 0x5a6628 && ra < 0x5a7c00 && c->r[4].UW[0] > 4) { static u64 saved = 0; saved += c->r[4].UW[0] - 4; c->r[4].UD[0] = 4; if (++g_cd_delays_cut % 100 == 0) rlog("cd", "%llu drive waits shortened: %.1f s saved", (unsigned long long)g_cd_delays_cut, saved * 63.56e-6); }
    f_005a65c0_orig(c);
}

// Loading: the boot movies. func_001ce958(player, movie, ...) starts a movie by its number in the table at 0x7d78e0
// (9 easports, 11 mvpintro, 19-56 the player cameos). At boot func_0066b100 plays the EA logo, a cameo and the
// minute-long intro. With intro movies off (the default, settings intro_movies=0) its movies are asked to stop instead (movie -1: the player's finished callback
// runs, as when one ends), and the game goes straight on to the title screen.
void f_001ce958_orig(EEContext* c);
void hle_movie_start(EEContext* c) {
    const int id = (int)c->r[5].UW[0];
    if (std::getenv("MVP_TRACE_MOVIE")) rlog("movie", "start %d (ra %08x)", id, c->r[31].UW[0]);
    if (!g_settings.intro_movies && id >= 0 && c->r[31].UW[0] == 0x66b3f4) {         // the boot sequence (func_0066b100)
        rlog("movie", "skipped boot movie %d", id); c->r[5].SD[0] = -1; }
    f_001ce958_orig(c);
}
