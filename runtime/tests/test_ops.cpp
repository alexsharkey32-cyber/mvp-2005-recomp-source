// Unit tests for instruction semantics in ee_ops.h. Build: make test
#include "ee_ops.h"
#include <cstdio>
#include <cstdlib>
u8* g_ram;
u8* ee_ptr_slow(u32) { return nullptr; }
u64 ee_hw_read(u32, int) { return 0; }
void ee_hw_write(u32, u64, int) {}
void ee_hw_read128(u32, GPR128*) {}
void ee_hw_write128(u32, const GPR128*) {}
static int fails = 0;
#define CHECK(expr, want) do { unsigned long long _g = (unsigned long long)(expr), _w = (unsigned long long)(want); \
    if (_g != _w) { printf("FAIL %s: got %016llx want %016llx\n", #expr, _g, _w); fails++; } } while (0)
int main() {
    static EEContext ctx; EEContext* c = &ctx;
    g_ram = (u8*)calloc(1, 0x10000);
    for (int i = 0; i < 16; i++) g_ram[0x100 + i] = 0x10 + i;     // 10 11 12 ... 1f
    // unaligned 32-bit load pair (lwl a+3 / lwr a) at address 0x101 -> bytes 11 12 13 14
    c->r[4].UD[0] = 0x100;
    c->r[8].UD[0] = 0xdeadbeefcafef00dull;
    LWL(8, 4, 4); LWR(8, 4, 1);
    CHECK(c->r[8].UD[0], 0x14131211ull);
    // unaligned 64-bit (ldl a+7 / ldr a) at 0x103 -> 13..1a
    LDL(9, 4, 10); LDR(9, 4, 3);
    CHECK(c->r[9].UD[0], 0x1a19181716151413ull);
    // unaligned 64-bit store back at 0x109
    c->r[10].UD[0] = 0x8877665544332211ull;
    SDL(10, 4, 16); SDR(10, 4, 9);
    CHECK(ee_rd<u64>(0x108), 0x7766554433221118ull);
    CHECK(g_ram[0x110], 0x88);
    // unaligned 32-bit store at 0x102
    c->r[11].UD[0] = 0xaabbccdd;
    SWL(11, 4, 5); SWR(11, 4, 2);
    CHECK(ee_rd<u32>(0x100), 0xccdd1110u);
    CHECK(ee_rd<u32>(0x104), 0x1716aabbu);   // bytes 102..105 = dd cc bb aa
    // 32-bit ops sign-extend
    c->r[5].UD[0] = 0x7fffffff; ADDIU(6, 5, 1);
    CHECK(c->r[6].UD[0], 0xffffffff80000000ull);
    LUI(7, 0x8000); CHECK(c->r[7].UD[0], 0xffffffff80000000ull);
    // mult/div incl. PS2 divide-by-zero behaviour
    c->r[5].SD[0] = -7; c->r[6].SD[0] = 2; DIV(0, 5, 6);
    CHECK(c->lo.SD[0], -3); CHECK(c->hi.SD[0], -1);
    c->r[6].SD[0] = 0; DIV(0, 5, 6);
    CHECK(c->lo.SD[0], 1); CHECK(c->hi.SD[0], -7);
    c->r[5].UD[0] = 0x10000; c->r[6].UD[0] = 0x10000; MULT(3, 5, 6);
    CHECK(c->lo.UD[0], 0); CHECK(c->hi.UD[0], 1); CHECK(c->r[3].UD[0], 0);
    // shifts
    c->r[5].UD[0] = 0x80000000u; SRA(6, 5, 4); CHECK(c->r[6].UD[0], 0xfffffffff8000000ull);
    DSLL32(6, 5, 0); CHECK(c->r[6].UD[0], 0x8000000000000000ull);
    // 128-bit
    c->r[5].UD[0] = 1; c->r[5].UD[1] = 2; c->r[6].UD[0] = 3; c->r[6].UD[1] = 4;
    PCPYLD(7, 5, 6); CHECK(c->r[7].UD[0], 3); CHECK(c->r[7].UD[1], 1);
    PCPYUD(7, 5, 6); CHECK(c->r[7].UD[0], 2); CHECK(c->r[7].UD[1], 4);
    // funnel shift used by the game's unaligned memcpy: MTSAB + QFSRV
    for (int i = 0; i < 16; i++) { c->r[8].UB[i] = (u8)i; c->r[9].UB[i] = (u8)(16 + i); }
    c->r[2].UD[0] = 3; MTSAB(2, 0); QFSRV(10, 9, 8);
    CHECK(c->r[10].UB[0], 3); CHECK(c->r[10].UB[12], 15); CHECK(c->r[10].UB[13], 16); CHECK(c->r[10].UB[15], 18);
    // FPU convert clamps
    c->f[1].f = 3e9f; CVT_W_S(2, 1, 0); CHECK((u32)c->f[2].u, 0x7fffffffu);
    c->f[1].f = -2.75f; CVT_W_S(2, 1, 0); CHECK((u32)c->f[2].s, (u32)-2);
    printf(fails ? "%d FAILED\n" : "all instruction tests passed\n", fails);
    return fails != 0;
}
