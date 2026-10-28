// High-level emulation of the PS2 EE kernel: threads, semaphores, interrupts, alarms.
// Guest threads are cooperative host coroutines (ucontext on macOS/Linux, fibers on Windows).
#if defined(__APPLE__)
#define _XOPEN_SOURCE 600
#endif
#include "ee_ops.h"
#include "ee_runtime.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <csignal>
#endif

bool g_host_timing = std::getenv("MVP_TIMING") != nullptr;
u64 host_time_ns() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (u64)duration_cast<nanoseconds>(steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- threads
enum ThStatus { TS_FREE = 0, TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_DORMANT = 16 };
enum WaitType { WT_NONE, WT_SLEEP, WT_SEMA };

struct Thread {
    int status = TS_FREE;
    int prio = 0, init_prio = 0;
    u32 entry = 0, stack = 0, stack_size = 0, gp = 0, attr = 0, option = 0;
    WaitType wait = WT_NONE;
    int wait_id = -1;
    int wakeup_count = 0;
    EEContext* ctx = nullptr;
    u32 arg = 0;
    bool started = false;
#ifdef _WIN32
    void* fiber = nullptr;
#else
    ucontext_t uc;
    void* host_stack = nullptr;
#endif
};
struct Sema { bool used = false; int count = 0, max = 0, init = 0; u32 attr = 0, option = 0; std::deque<int> waiters; };

static const int MAX_THREADS = 256, MAX_SEMAS = 256;
static Thread s_th[MAX_THREADS];
static Sema s_sema[MAX_SEMAS];
static int s_cur = 1;
static const size_t HOST_STACK = 16 * 1024 * 1024;   // recompiled code recurses deeply
#ifdef _WIN32
static void* s_main_fiber;
#else
static ucontext_t s_sched_uc;
#endif

EEContext* kernel_current() { return s_th[s_cur].ctx; }

static EEContext* new_ctx() {
    void* p = nullptr;
#ifdef _WIN32
    p = _aligned_malloc(sizeof(EEContext), 64);
#else
    if (posix_memalign(&p, 64, sizeof(EEContext)) != 0) p = nullptr;
#endif
    if (!p) rfatal("oom");
    std::memset(p, 0, sizeof(EEContext));
    EEContext* c = (EEContext*)p;
    c->vf[0][3] = 1.0f;
    return c;
}

// ---------------------------------------------------------------- interrupts / alarms
struct IntcHandler { int cause; u32 fn; u32 arg; int id; bool enabled; };
static std::vector<IntcHandler> s_intc, s_dmac;
static int s_next_handler_id = 1;
static u32 s_intc_mask = 0, s_dmac_mask = 0;
struct Alarm { int id; u64 due_ns; u32 fn; u32 arg; u16 ticks; bool used; int owner = 0; u64 set_at = 0; };   // owner: the thread that set it, set_at: s_yields then
static std::vector<Alarm> s_alarms;
static int s_next_alarm = 1;
static u64 s_yields = 0;            // polling points passed by running threads (guest progress, not host time)
static bool s_idle = false;         // no thread is running: the scheduler is waiting for an event
static u64 s_next_vblank_ns = 0;
static u64 s_vblank_count = 0;
static bool s_in_interrupt = false;
static EEContext* s_int_ctx = nullptr;
EEContext* kernel_active_ctx() { return s_in_interrupt ? s_int_ctx : kernel_current(); }
static const u32 INT_STACK_TOP = 0x0007fff0;
static u64 VBLANK_NS = 16683333;                        // NTSC 59.94 Hz (settings fps_limit; MVP_TURBO=n divides it)
static int s_turbo = 1;
static u64 s_vblank_late_ns = 0;                        // how late the last vblank was delivered
bool g_fast_forward = false;                            // frontend: hold Tab = run unthrottled
void kernel_set_refresh(int hz) {                       // 0 = unlimited
    if (hz <= 0) VBLANK_NS = 1000000;                   // 1000 Hz: effectively unthrottled
    else if (hz == 60) VBLANK_NS = 16683333;            // exact NTSC rate for "normal speed"
    else VBLANK_NS = (u64)(1e9 / hz);
    VBLANK_NS /= s_turbo;
}
int kernel_turbo() { return s_turbo; }
u64 kernel_vblank_ns() { return g_fast_forward ? 1000000 : VBLANK_NS; }
u64 kernel_vblank_late_ns() { return s_vblank_late_ns; }
static const double HSYNC_NS = 63555.6;

static void run_guest_handler(u32 fn, u32 a0, u32 a1, u32 a2, u32 gp_from) {
    EEContext* c = s_int_ctx;
    EEContext* cur = kernel_current();
    c->r[4].SD[0] = (s32)a0; c->r[5].SD[0] = (s32)a1; c->r[6].SD[0] = (s32)a2;
    c->r[29].UD[0] = INT_STACK_TOP;
    c->r[28] = cur ? cur->r[28] : c->r[28];
    (void)gp_from;
    c->r[31].UD[0] = 0;
    bool was = s_in_interrupt; s_in_interrupt = true;
    ee_call(c, fn);
    s_in_interrupt = was;
}

// Native work that calls guest functions (database dumps): run between guest code, on the interrupt context and
// stack, with interrupts held off. Requested from any thread; done on the EE thread at the next event check.
#include <atomic>
#include <functional>
#include <mutex>
static std::mutex s_native_mx; static std::vector<std::function<void(EEContext*)>> s_native_q; static std::atomic<bool> s_native_any{false};
void kernel_queue_native(std::function<void(EEContext*)> fn) { std::lock_guard<std::mutex> lk(s_native_mx); s_native_q.push_back(std::move(fn)); s_native_any = true; }
static void run_native_queue() {
    if (!s_native_any || s_in_interrupt) return;
    std::vector<std::function<void(EEContext*)>> q;
    { std::lock_guard<std::mutex> lk(s_native_mx); q.swap(s_native_q); s_native_any = false; }
    EEContext* cur = kernel_current();
    for (auto& fn : q) {
        EEContext* c = s_int_ctx;
        c->r[29].UD[0] = INT_STACK_TOP; if (cur) c->r[28] = cur->r[28]; c->r[31].UD[0] = 0;
        bool was = s_in_interrupt; s_in_interrupt = true; fn(c); s_in_interrupt = was;
    }
}

// Interrupts raised while a handler is running are latched and delivered after it returns
// (the EE runs handlers with interrupts disabled).
static u32 s_pend_intc = 0, s_pend_dmac = 0;
static void dispatch_pending();
bool kernel_in_interrupt() { return s_in_interrupt; }
void kernel_raise_intc(int cause) {
    if (s_in_interrupt) { s_pend_intc |= 1u << cause; return; }
    for (size_t i = 0; i < s_intc.size(); i++) {
        IntcHandler h = s_intc[i];
        if (h.cause == cause && h.enabled && (s_intc_mask & (1u << cause))) run_guest_handler(h.fn, cause, h.arg, 0, 0);
    }
    dispatch_pending();
}
void kernel_raise_dmac(int ch) {
    if (s_in_interrupt) { s_pend_dmac |= 1u << ch; return; }
    for (size_t i = 0; i < s_dmac.size(); i++) {
        IntcHandler h = s_dmac[i];
        if (h.cause == ch && h.enabled && (s_dmac_mask & (1u << ch))) run_guest_handler(h.fn, ch, h.arg, 0, 0);
    }
    dispatch_pending();
}
static void dispatch_pending() {
    while (!s_in_interrupt && (s_pend_intc || s_pend_dmac)) {
        if (s_pend_dmac) { int ch = __builtin_ctz(s_pend_dmac); s_pend_dmac &= ~(1u << ch); kernel_raise_dmac(ch); continue; }
        int ca = __builtin_ctz(s_pend_intc); s_pend_intc &= ~(1u << ca); kernel_raise_intc(ca);
    }
}
u64 kernel_vblank_count() { return s_vblank_count; }
void hw_vblank_start(); void hw_vblank_end();

static void schedule();

// kill -USR1 <pid>: print what every guest thread is waiting for (to look into a hang without a debugger)
static volatile std::sig_atomic_t s_dump_request = 0;
void kernel_dump();

// Deliver due timer events. Returns true if something happened.
static bool deliver_events(bool idle) {
    if (s_dump_request) { s_dump_request = 0; kernel_dump(); extern void iop_dump(); iop_dump(); }
    run_native_queue();
    { extern void gt_poll_events(); gt_poll_events(); }   // graphics thread: DMA/GS completion, VIF1 interrupts
    u64 now = host_time_ns();
    bool any = false;
    if (idle) {
        // nothing runnable: jump time forward to the next event
        u64 next = s_next_vblank_ns;
        for (auto& a : s_alarms) if (a.used && a.due_ns < next) next = a.due_ns;
        if (next > now) {
            u64 wait = next - now;
            if (wait > 1000000) wait = 1000000;  // sleep a little to keep real-time pacing
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
            now = host_time_ns();
        }
    }
    if (now >= s_next_vblank_ns) {
        { extern void gt_wait_frames(); gt_wait_frames(); now = host_time_ns(); }
        // absolute schedule (no drift); if we fall more than 4 frames behind, resync instead of bursting
        u64 period = kernel_vblank_ns();
        s_vblank_late_ns = now - s_next_vblank_ns;
        s_next_vblank_ns += period;
        if (now > s_next_vblank_ns + 4 * period || g_fast_forward) s_next_vblank_ns = now + period;
        s_vblank_count++;
        { extern void dbg_symtab_check(u64); dbg_symtab_check(s_vblank_count); }
        hw_vblank_start();
        kernel_raise_intc(2);                 // INTC_VBLANK_S
        hw_vblank_end();
        kernel_raise_intc(3);                 // INTC_VBLANK_E
        any = true;
    }
    {
        extern bool iop_deliver();
        extern bool hw_poll_timers();
        extern bool snd_poll();
        while (iop_deliver()) any = true;
        if (snd_poll()) any = true;
        if (hw_poll_timers()) any = true;
    }
    for (size_t i = 0; i < s_alarms.size(); i++) {          // by index and by value: a handler may set new alarms
        Alarm a = s_alarms[i];
        if (!a.used || now < a.due_ns) continue;
        // The game's delay is SetAlarm followed by SleepThread, with a handler that wakes the thread. If the host
        // stalls between those two calls (a busy machine), the alarm would go off before the thread sleeps - on a
        // PS2 that takes far longer than the two calls ever do. libkernel then loses the wake-up (its helper thread
        // is not running in this game) and the thread sleeps for ever. So an alarm waits for the thread that set it
        // to block, unless that thread keeps running (it may be polling for the handler instead of sleeping).
        static const bool no_guard = std::getenv("MVP_TEST_NO_ALARM_GUARD") != nullptr;      // the old behaviour, to show the hang
        if (!no_guard && !idle && a.owner == s_cur && s_yields - a.set_at < 8) continue;
        s_alarms[i].used = false;
        run_guest_handler(a.fn, a.id, a.ticks, a.arg, 0);
        any = true;
    }
    return any;
}

// ---------------------------------------------------------------- scheduler
static int pick_next() {
    int best = -1;
    // round robin among equal priority: start after current
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (s_cur + k) % MAX_THREADS;
        Thread& t = s_th[i];
        if (t.status == TS_READY || t.status == TS_RUN) {
            if (best < 0 || t.prio < s_th[best].prio) best = i;
        }
    }
    return best;
}

static void switch_to(int next) {
    extern bool g_eie; g_eie = true;
    if (next == s_cur) { s_th[s_cur].status = TS_RUN; return; }
    int prev = s_cur;
    if (s_th[prev].status == TS_RUN) s_th[prev].status = TS_READY;
    s_cur = next;
    s_th[next].status = TS_RUN;
#ifdef _WIN32
    SwitchToFiber(s_th[next].fiber);
#else
    swapcontext(&s_th[prev].uc, &s_th[next].uc);
#endif
}

static void schedule() {
    if (s_in_interrupt) return;               // switch happens when the interrupt returns to thread code
    for (int spins = 0;; spins++) {
        int n = pick_next();
        if (n >= 0) { switch_to(n); return; }
        s_idle = true; deliver_events(true); s_idle = false;
        if (spins > 600 * 60) {               // ~10 minutes of nothing runnable
            rfatal("deadlock: no runnable guest threads");
        }
    }
}

// Called from syscalls and polling points: deliver interrupts, maybe preempt.
static int s_poll_counter = 0;
extern bool g_eie;
void kernel_yield(EEContext* c) {
    (void)c;
    if (s_in_interrupt || !g_eie) return;                 // interrupts disabled (di): no delivery, no preemption
    s_yields++;
    if (deliver_events(false)) {
        int n = pick_next();
        if (n >= 0 && n != s_cur && s_th[n].prio < s_th[s_cur].prio) switch_to(n);
    }
}
void ee_poll(EEContext* c) { if (++s_poll_counter >= 64) { s_poll_counter = 0; kernel_yield(c); } }
// The game spins in a few loops until an interrupt handler clears a flag (EAGL waits like that for the frame's
// drawing to finish). Spinning there burns a core for nothing: it takes time from the graphics thread on small
// machines and heats up laptops. When nothing is due, sleep until the graphics thread reports something or
// until the next timer, a quarter of a millisecond at most.
void ee_wait_poll(EEContext* c) {
    (void)c;
    static const bool spin = std::getenv("MVP_SPIN") != nullptr;      // old behaviour, for comparison
    if (spin) { ee_poll(c); return; }
    if (s_in_interrupt || !g_eie) return;
    s_yields++;
    if (deliver_events(false)) {
        int n = pick_next();
        if (n >= 0 && n != s_cur && s_th[n].prio < s_th[s_cur].prio) switch_to(n);
        return;
    }
    u64 now = host_time_ns(), next = s_next_vblank_ns;
    for (auto& a : s_alarms) if (a.used && a.due_ns < next) next = a.due_ns;
    // Sleep through the bulk of the wait, then spin the last stretch: a sleep that overshoots the vblank by a few
    // hundred microseconds makes a frame that was just in time miss it, and costs a whole vblank.
    static const u64 margin = std::getenv("MVP_WAIT_MARGIN") ? (u64)std::atoll(std::getenv("MVP_WAIT_MARGIN")) * 1000 : 400000;   // microseconds
    if (next <= now + margin + 50000) return;
    extern void gt_wait_event(u64 ns);
    gt_wait_event(std::min<u64>(next - now - margin, 250000));
}

static void thread_exit_current();

#ifdef _WIN32
static void __stdcall thread_trampoline(void*) {
#else
static void thread_trampoline() {
#endif
    Thread& t = s_th[s_cur];
    EEContext* c = t.ctx;
    ee_call(c, t.entry);
    rlog("thread", "thread %d returned from entry %08x", s_cur, t.entry);
    thread_exit_current();
}

static void thread_setup_host(int id) {
    Thread& t = s_th[id];
#ifdef _WIN32
    t.fiber = CreateFiber(HOST_STACK, thread_trampoline, nullptr);
#else
    if (!t.host_stack) t.host_stack = std::malloc(HOST_STACK);
    getcontext(&t.uc);
    t.uc.uc_stack.ss_sp = t.host_stack;
    t.uc.uc_stack.ss_size = HOST_STACK;
    t.uc.uc_link = &s_sched_uc;
    makecontext(&t.uc, (void (*)())thread_trampoline, 0);
#endif
}

static void thread_exit_current() {
    Thread& t = s_th[s_cur];
    t.status = TS_DORMANT;
    t.started = false;
    schedule();
    rfatal("exited thread resumed");
}

// ---------------------------------------------------------------- syscall helpers
#define A0 c->r[4].UW[0]
#define A1 c->r[5].UW[0]
#define A2 c->r[6].UW[0]
#define A3 c->r[7].UW[0]
#define T0 c->r[8].UW[0]
#define RET(v) (c->r[2].SD[0] = (s64)(s32)(v))

static u32 rd32(u32 a) { return ee_rd<u32>(a); }
static void wr32(u32 a, u32 v) { ee_wr<u32>(a, v); }

static int alloc_thread() { for (int i = 1; i < MAX_THREADS; i++) if (s_th[i].status == TS_FREE) return i; return -1; }

static void wake(int id) {
    Thread& t = s_th[id];
    t.wait = WT_NONE; t.wait_id = -1;
    if (t.status == TS_WAIT) t.status = TS_READY;
    else if (t.status == (TS_WAIT | TS_SUSPEND)) t.status = TS_SUSPEND;
}

static u32 s_heap_end = 0;
static u32 s_gs_imr = 0xff00;
u32 kernel_gs_imr() { return s_gs_imr; }

void ee_syscall(EEContext* c, u32 pc) {
    s32 num = c->r[3].SW[0];
    if (num < 0) num = -num;
    switch (num) {
    case 0x02: rlog("syscall", "SetGsCrt(interlace=%d, mode=%d, field=%d)", A0, A1, A2); break;
    case 0x04: rfatal("guest called Exit(%d)", A0);
    case 0x10: case 0x12: {                       // AddIntcHandler / AddDmacHandler(cause, handler, next, arg)
        IntcHandler h{(int)A0, A1, A3, s_next_handler_id++, true};
        (num == 0x10 ? s_intc : s_dmac).push_back(h);
        rlog("syscall", "Add%sHandler(cause=%d, fn=%08x, arg=%08x) -> %d", num == 0x10 ? "Intc" : "Dmac", A0, A1, A3, h.id);
        RET(h.id); break;
    }
    case 0x11: case 0x13: {
        auto& v = num == 0x11 ? s_intc : s_dmac;
        for (size_t i = 0; i < v.size(); i++) if (v[i].cause == (int)A0 && v[i].id == (int)A1) { v.erase(v.begin() + i); break; }
        RET(0); break;
    }
    case 0x14: s_intc_mask |= 1u << A0; RET(1); break;        // _EnableIntc
    case 0x15: s_intc_mask &= ~(1u << A0); RET(1); break;
    case 0x16: s_dmac_mask |= 1u << A0; RET(1); break;
    case 0x17: s_dmac_mask &= ~(1u << A0); RET(1); break;
    case 0x1a: s_intc_mask |= 1u << A0; RET(1); break;         // iEnableIntc
    case 0x1b: s_intc_mask &= ~(1u << A0); RET(1); break;
    case 0x1c: s_dmac_mask |= 1u << A0; RET(1); break;
    case 0x1d: s_dmac_mask &= ~(1u << A0); RET(1); break;
    case 0x18: case 0xfc: case 0xfe: {             // SetAlarm(time, handler, arg) (0xfc/0xfe: libkernel's patched alarm)
        Alarm a{s_next_alarm++, host_time_ns() + (u64)(A0 * HSYNC_NS), A1, A2, (u16)A0, true, s_in_interrupt ? 0 : s_cur, s_yields};
        static const bool stall = std::getenv("MVP_TEST_ALARM_STALL") != nullptr;            // test: the host stalls right here
        if (stall && !s_in_interrupt) std::this_thread::sleep_for(std::chrono::nanoseconds((u64)(A0 * HSYNC_NS) + 300000));
        bool placed = false;
        for (auto& slot : s_alarms) if (!slot.used) { slot = a; placed = true; break; }     // (the list used to grow for ever)
        if (!placed) s_alarms.push_back(a);
        RET(a.id); break;
    }
    case 0x19: case 0x1f: case 0xfd: case 0xff:    // ReleaseAlarm
        for (auto& a : s_alarms) if (a.id == (int)A0) a.used = false;
        RET(0); break;
    case 0x20: {                                   // CreateThread(param*)
        int id = alloc_thread();
        if (id < 0) { RET(-1); break; }
        Thread& t = s_th[id];
        u32 p = A0;
        t = Thread();
        t.entry = rd32(p + 4); t.stack = rd32(p + 8); t.stack_size = rd32(p + 12); t.gp = rd32(p + 16);
        t.init_prio = t.prio = rd32(p + 20); t.attr = rd32(p + 28); t.option = rd32(p + 32);
        t.status = TS_DORMANT;
        t.ctx = new_ctx();
        rlog("thread", "CreateThread -> %d entry=%08x stack=%08x+%x prio=%d", id, t.entry, t.stack, t.stack_size, t.prio);
        RET(id); break;
    }
    case 0x21: {                                   // DeleteThread
        int id = A0; if (id > 0 && id < MAX_THREADS && s_th[id].status == TS_DORMANT) { s_th[id].status = TS_FREE; RET(0); } else RET(-1);
        break;
    }
    case 0x22: {                                   // StartThread(id, arg)
        int id = A0; Thread& t = s_th[id];
        if (id <= 0 || id >= MAX_THREADS || t.status != TS_DORMANT) { RET(-1); break; }
        EEContext* tc = t.ctx;
        std::memset(tc->r, 0, sizeof tc->r);
        tc->r[4].UD[0] = A1;
        tc->r[29].UD[0] = t.stack + t.stack_size - 16;
        tc->r[28].UD[0] = t.gp;
        t.prio = t.init_prio;
        t.status = TS_READY; t.started = true;
        thread_setup_host(id);
        rlog("thread", "StartThread(%d, arg=%08x)", id, A1);
        RET(0);
        if (t.prio < s_th[s_cur].prio) schedule();
        break;
    }
    case 0x23: case 0x24:                          // ExitThread / ExitDeleteThread
        thread_exit_current(); break;
    case 0x25: case 0x26: {                        // TerminateThread
        int id = A0; if (id > 0 && id < MAX_THREADS) { s_th[id].status = TS_DORMANT; s_th[id].started = false; }
        RET(0); break;
    }
    case 0x29: case 0x2a: {                        // ChangeThreadPriority(id, prio)
        int id = A0 ? (int)A0 : s_cur; int old = s_th[id].prio; s_th[id].prio = A1;
        RET(old);
        if (num == 0x29) schedule();
        break;
    }
    case 0x2b: case 0x2c:                          // RotateThreadReadyQueue(prio)
        RET(0); if (num == 0x2b) { kernel_yield(c); schedule(); } break;
    case 0x2d: case 0x2e: {                        // ReleaseWaitThread
        int id = A0; if (s_th[id].status & TS_WAIT) { wake(id); RET(0); if (num == 0x2d) schedule(); } else RET(-1);
        break;
    }
    case 0x2f: { static const bool old = std::getenv("MVP_TEST_NO_ALARM_GUARD") != nullptr; RET(s_idle && s_in_interrupt && !old ? 0 : s_cur); break; }   // GetThreadId (0: the idle thread - every thread is waiting; libkernel's
                                                                   // iWakeupThread drops a wake-up aimed at "the running thread" in this game)
    case 0x30: case 0x31: {                        // ReferThreadStatus(id, info*)
        int id = A0 ? (int)A0 : s_cur; Thread& t = s_th[id]; u32 p = A1;
        if (p) {
            wr32(p + 0, t.status); wr32(p + 4, t.entry); wr32(p + 8, t.stack); wr32(p + 12, t.stack_size);
            wr32(p + 16, t.gp); wr32(p + 20, t.init_prio); wr32(p + 24, t.prio); wr32(p + 28, t.attr);
            wr32(p + 32, t.option); wr32(p + 36, t.wait == WT_SLEEP ? 1 : t.wait == WT_SEMA ? 2 : 0);
            wr32(p + 40, t.wait_id); wr32(p + 44, t.wakeup_count);
        }
        RET(t.status); break;
    }
    case 0x32: {                                   // SleepThread
        Thread& t = s_th[s_cur];
        if (t.wakeup_count > 0) { t.wakeup_count--; RET(s_cur); break; }
        t.status = TS_WAIT; t.wait = WT_SLEEP;
        schedule();
        RET(s_cur); break;
    }
    case 0x33: case 0x34: {                        // WakeupThread
        int id = A0; Thread& t = s_th[id];
        if (t.status & TS_WAIT && t.wait == WT_SLEEP) { wake(id); if (num == 0x33) schedule(); }
        else t.wakeup_count++;
        RET(0); break;
    }
    case 0x35: case 0x36: { int id = A0 ? (int)A0 : s_cur; int n = s_th[id].wakeup_count; s_th[id].wakeup_count = 0; RET(n); break; }
    case 0x37: case 0x38: { int id = A0; s_th[id].status |= TS_SUSPEND; if (s_th[id].status == (TS_RUN | TS_SUSPEND) || id == s_cur) { s_th[id].status = TS_SUSPEND; schedule(); } RET(0); break; }
    case 0x39: case 0x3a: { int id = A0; s_th[id].status &= ~TS_SUSPEND; if (s_th[id].status == 0) s_th[id].status = TS_READY; RET(0); if (num == 0x39) schedule(); break; }
    case 0x3c: {                                   // SetupThread(gp, stack, stack_size, args, root)
        u32 stack = A1, size = A2;
        u32 top = (stack == 0xffffffffu) ? 0x02000000u : stack + size;
        RET(top - 0x2a0);
        rlog("syscall", "SetupThread(gp=%08x, stack=%08x, size=%x) -> sp=%08x", A0, A1, A2, top - 0x2a0);
        break;
    }
    case 0x3d: {                                   // SetupHeap(start, size)
        s_heap_end = (A1 == 0xffffffffu) ? 0x02000000u - 0x10000 : A0 + A1;
        rlog("syscall", "SetupHeap(%08x, %x) -> %08x", A0, A1, s_heap_end);
        RET(s_heap_end); break;
    }
    case 0x3e: RET(s_heap_end); break;             // EndOfHeap
    case 0x40: {                                   // CreateSema(sema_t*)
        int id = -1; for (int i = 1; i < MAX_SEMAS; i++) if (!s_sema[i].used) { id = i; break; }
        if (id < 0) { RET(-1); break; }
        Sema& s = s_sema[id]; u32 p = A0;
        s = Sema(); s.used = true;
        s.max = rd32(p + 4); s.init = rd32(p + 8); s.count = s.init; s.attr = rd32(p + 16); s.option = rd32(p + 20);
        RET(id); break;
    }
    case 0x41: { int id = A0; for (int w : s_sema[id].waiters) wake(w); s_sema[id] = Sema(); RET(id); break; }
    case 0x42: case 0x43: {                        // SignalSema
        int id = A0; Sema& s = s_sema[id];
        if (!s.used) { RET(-1); break; }
        if (!s.waiters.empty()) { int w = s.waiters.front(); s.waiters.pop_front(); wake(w); RET(id); if (num == 0x42) schedule(); }
        else { s.count++; RET(id); }
        break;
    }
    case 0x44: {                                   // WaitSema
        int id = A0; Sema& s = s_sema[id];
        if (!s.used) { RET(-1); break; }
        if (s.count > 0) { s.count--; RET(id); break; }
        s.waiters.push_back(s_cur);
        s_th[s_cur].status = TS_WAIT; s_th[s_cur].wait = WT_SEMA; s_th[s_cur].wait_id = id;
        schedule();
        RET(id); break;
    }
    case 0x45: case 0x46: {                        // PollSema
        int id = A0; Sema& s = s_sema[id];
        if (s.used && s.count > 0) { s.count--; RET(id); } else RET(-419);
        break;
    }
    case 0x47: case 0x48: {                        // ReferSemaStatus
        int id = A0; Sema& s = s_sema[id]; u32 p = A1;
        if (p) { wr32(p, s.count); wr32(p + 4, s.max); wr32(p + 8, s.init); wr32(p + 12, (u32)s.waiters.size()); wr32(p + 16, s.attr); wr32(p + 20, s.option); }
        RET(id); break;
    }
    case 0x64: case 0x68: RET(0); break;           // FlushCache / iFlushCache
    case 0x70: RET(s_gs_imr); break;               // GsGetIMR
    case 0x71: s_gs_imr = A0; RET(0); break;       // GsPutIMR
    case 0x73: RET(0); break;                      // SetVSyncFlag
    case 0x74: RET(0); break;                      // SetSyscall
    case 0x7c: RET(0); break;                      // Deci2Call
    case 0x7e: RET(0); break;                      // MachineType
    case 0x7f: RET(0x02000000); break;             // GetMemorySize
    default: {
        extern bool sif_syscall(EEContext* c, int num);
        if (sif_syscall(c, num)) break;
        rlog("syscall", "UNIMPLEMENTED syscall 0x%x at pc=%08x (a0=%08x a1=%08x a2=%08x)", num, pc, A0, A1, A2);
        RET(0);
    }
    }
    kernel_yield(c);
}

void kernel_dump() {
    fprintf(stderr, "threads (current %d):\n", s_cur);
    for (int i = 1; i < MAX_THREADS; i++) {
        Thread& t = s_th[i];
        if (t.status == TS_FREE) continue;
        fprintf(stderr, "  #%d status=%d prio=%d entry=%08x wait=%s id=%d ra=%08x\n", i, t.status, t.prio, t.entry,
                t.wait == WT_SLEEP ? "sleep" : t.wait == WT_SEMA ? "sema" : "-", t.wait_id, t.ctx ? t.ctx->r[31].UW[0] : 0);
        if (t.ctx && t.ctx->shadow_n > 0) {
            fprintf(stderr, "     stack:");
            for (int k = 0; k < t.ctx->shadow_n && k < 1024; k++) fprintf(stderr, " %08x", t.ctx->shadow[k]);
            fprintf(stderr, "\n");
        }
    }
    for (int i = 1; i < MAX_SEMAS; i++) if (s_sema[i].used && !s_sema[i].waiters.empty())
        fprintf(stderr, "  sema %d count=%d waiters=%zu\n", i, s_sema[i].count, s_sema[i].waiters.size());
}

// ---------------------------------------------------------------- boot
void kernel_run_main(u32 entry) {
    s_int_ctx = new_ctx();
    int id = 1;
    Thread& t = s_th[id];
    t = Thread();
    t.status = TS_RUN; t.prio = t.init_prio = 0; t.entry = entry; t.started = true;
    t.ctx = new_ctx();
    s_cur = id;
    if (const char* t = std::getenv("MVP_TURBO")) { int n = std::atoi(t); if (n > 1) { s_turbo = n; VBLANK_NS /= n; } }
    s_next_vblank_ns = host_time_ns() + VBLANK_NS;
#ifndef _WIN32
    std::signal(SIGUSR1, [](int) { s_dump_request = 1; });
#endif
    thread_setup_host(id);
#ifdef _WIN32
    s_main_fiber = ConvertThreadToFiber(nullptr);
    SwitchToFiber(t.fiber);
#else
    swapcontext(&s_sched_uc, &t.uc);
#endif
}
