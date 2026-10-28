// push constants of the GS pipelines (gs.vert / gs.frag), packed into 7 words so they fit the 128 bytes every
// Vulkan driver must allow (vulkan.cpp GsPC fills them)
layout(push_constant) uniform P {
    uint first;          // first vertex of the batch (flat colour: the last vertex of each primitive, GL / GS rule)
    uint f0;             // tme fst iip fge tfx:2 tcc bil hw wrapx:2 wrapy:2 apass fba ba:2 bb:2 bc:2 bd:2 bmode:2 date:2 colclamp per:2
    uint f1;             // atst+1:4 aref:8 fix:8 hsx:5 hsy:5 zcode:2
    uint ts;             // texture size: x | y << 16
    uint rx, ry;         // region clamp / repeat values: lo | hi << 16
    uint fog;            // fog colour rgb
} pc;
struct U {
    int tme, fst, iip, fge, tfx, tcc, bil, hw;
    int tsx, tsy, wrapx, wrapy, rx0, rx1, ry0, ry1;
    float hsx, hsy; int atst, aref;
    int apass, fba, ba, bb, bc, bd, fix, bmode;
    int date, colclamp; float zmax; float fogr, fogg, fogb;
    uint per;
};
int bits(uint w, int o, int n) { return int(bitfieldExtract(w, o, n)); }
U unpack() {
    U u;
    u.tme = bits(pc.f0, 0, 1); u.fst = bits(pc.f0, 1, 1); u.iip = bits(pc.f0, 2, 1); u.fge = bits(pc.f0, 3, 1);
    u.tfx = bits(pc.f0, 4, 2); u.tcc = bits(pc.f0, 6, 1); u.bil = bits(pc.f0, 7, 1); u.hw = bits(pc.f0, 8, 1);
    u.wrapx = bits(pc.f0, 9, 2); u.wrapy = bits(pc.f0, 11, 2); u.apass = bits(pc.f0, 13, 1); u.fba = bits(pc.f0, 14, 1);
    u.ba = bits(pc.f0, 15, 2); u.bb = bits(pc.f0, 17, 2); u.bc = bits(pc.f0, 19, 2); u.bd = bits(pc.f0, 21, 2);
    u.bmode = bits(pc.f0, 23, 2); u.date = bits(pc.f0, 25, 2); u.colclamp = bits(pc.f0, 27, 1); u.per = uint(bits(pc.f0, 28, 2));
    u.atst = bits(pc.f1, 0, 4) - 1; u.aref = bits(pc.f1, 4, 8); u.fix = bits(pc.f1, 12, 8);
    u.hsx = float(bits(pc.f1, 20, 5)); u.hsy = float(bits(pc.f1, 25, 5));
    int zc = bits(pc.f1, 30, 2); u.zmax = zc == 2 ? 65535.0 : zc == 1 ? 16777215.0 : 4294967295.0;
    u.tsx = bits(pc.ts, 0, 16); u.tsy = bits(pc.ts, 16, 16);
    u.rx0 = bits(pc.rx, 0, 16); u.rx1 = bits(pc.rx, 16, 16); u.ry0 = bits(pc.ry, 0, 16); u.ry1 = bits(pc.ry, 16, 16);
    u.fogr = float(bits(pc.fog, 0, 8)); u.fogg = float(bits(pc.fog, 8, 8)); u.fogb = float(bits(pc.fog, 16, 8));
    return u;
}
