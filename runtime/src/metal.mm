// Metal renderer + presenter (macOS, Apple GPUs). Built only on Apple (Makefile: *.mm, -fobjc-arc).
//
// Renderer (GpuBackend): draws the GS primitives batched by gs_gpu.cpp into the main target. Apple GPUs can read
// the pixel being drawn over in the fragment shader (framebuffer fetch), so the GS blend formula, FBMSK (bit by
// bit), and the destination alpha test are all done exactly in the shader, with no target copies and no
// fixed-function blending. One render pipeline covers every GS state; depth tests are depth-stencil states.
//
// Presenter: game picture -> [FXAA] -> resample -> sharpen/colour/CRT -> CAMetalLayer drawable, then the UI;
// the same passes and maths as the OpenGL presenter (present_gl.cpp). The renderer and presenter share one
// device and one command queue, so the presenter's reads are ordered after the renderer's draws.
#if defined(__APPLE__) && defined(MVP_SDL)
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "gs_gpu.h"
#include "present.h"
#include "ee_runtime.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <atomic>
#include <deque>
#include <unordered_map>

static id<MTLDevice> s_dev = nil;
static id<MTLCommandQueue> s_queue = nil;
// command buffers are numbered as they are committed; s_completed = the last one the GPU has finished
// (one queue: they finish in order). Freed textures are reused once everything that could use them is done.
static u64 s_serial = 0;
static std::atomic<u64> s_completed{0};
static void commit(id<MTLCommandBuffer> cb) {
    const u64 n = ++s_serial;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done_cb) { (void)done_cb; u64 c = s_completed.load(); while (c < n && !s_completed.compare_exchange_weak(c, n)) {} }];
    [cb commit];
}

static id<MTLLibrary> make_library(const char* src, const char* what) {
    NSError* err = nil;
    MTLCompileOptions* opt = [MTLCompileOptions new];
    opt.fastMathEnabled = NO;
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:opt error:&err];
    if (!lib) fprintf(stderr, "[metal] %s shaders: %s\n", what, err ? [[err localizedDescription] UTF8String] : "?");
    return lib;
}
static id<MTLRenderPipelineState> make_pipe(id<MTLLibrary> lib, const char* vs, const char* fs, MTLPixelFormat color, MTLPixelFormat depth,
                                            bool blend, MTLColorWriteMask mask = MTLColorWriteMaskAll, MTLPixelFormat color1 = MTLPixelFormatInvalid) {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:vs]];
    d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:fs]];
    if (!d.vertexFunction || !d.fragmentFunction) { fprintf(stderr, "[metal] missing function %s/%s\n", vs, fs); return nil; }
    d.colorAttachments[0].pixelFormat = color;
    d.colorAttachments[0].writeMask = mask;
    if (color1 != MTLPixelFormatInvalid) d.colorAttachments[1].pixelFormat = color1;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    d.depthAttachmentPixelFormat = depth;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [s_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) fprintf(stderr, "[metal] pipeline %s/%s: %s\n", vs, fs, err ? [[err localizedDescription] UTF8String] : "?");
    return p;
}

// ================================================================ renderer
static const char* kGsMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut {
    float4 pos [[position]];
    float psize [[point_size]];
    float4 col [[center_no_perspective]];
    float4 fcol [[flat]];
    float3 stq [[center_no_perspective]];
    float2 uv [[center_no_perspective]];
    float fog [[center_no_perspective]];
    float z [[center_no_perspective]];
    float4 clampv [[flat]];
};
// base instance = the batch's first vertex | vertices per primitive << 28 (flat colour = the last vertex of each
// primitive, like GL / the GS); passing it there saves a buffer binding per draw
vertex VOut gs_vs(uint vid [[vertex_id]], uint inst [[base_instance]], const device float* vb [[buffer(0)]]) {
    const device float* d = vb + vid * 17;
    const uint first = inst & 0x0fffffffu, per = inst >> 28;
    uint last = first + ((vid - first) / per) * per + (per - 1);
    const device float* l = vb + last * 17;
    VOut o;
    o.pos = float4(d[0], -d[1], 0.0, 1.0);          // row 0 of the target = GS y 0 (as the GL target's memory)
    o.psize = 1.0;
    o.z = d[2];
    o.col = float4(d[3], d[4], d[5], d[6]);
    o.fcol = float4(l[3], l[4], l[5], l[6]);
    o.stq = float3(d[7], d[8], d[9]);
    o.uv = float2(d[10], d[11]);
    o.fog = d[12];
    o.clampv = float4(d[13], d[14], d[15], d[16]);
    return o;
}
struct U {
    int tme, fst, iip, fge, tfx, tcc, bil, hw;
    int tsx, tsy, wrapx, wrapy, rx0, rx1, ry0, ry1;
    float hsx, hsy; int atst, aref;
    int apass, fba, abe, ba, bb, bc, bd, fix;
    int date, colclamp; uint fbmsk; float zmax;
    float fogr, fogg, fogb; uint xmask;
};
int wrapc(int c, int size, int mode, int mn, int mx) {
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return c < mn ? mn : (c > mx ? mx : c);
    return (c & mn) | mx;
}
int4 fetch(texture2d<float> t, constant U& u, int x, int y) {
    x = wrapc(x, u.tsx, u.wrapx, u.rx0, u.rx1);
    y = wrapc(y, u.tsy, u.wrapy, u.ry0, u.ry1);
    if (x < 0 || y < 0 || x >= u.tsx || y >= u.tsy) return int4(0);
    return int4(t.read(uint2(x, y)) * 255.0 + 0.5);
}
int4 lerpc(int4 a, int4 b, int f) { return a + (((b - a) * f) >> 8); }
int blendsel(int sel, int cs, int cd) { return sel == 0 ? cs : (sel == 1 ? cd : 0); }
struct FOut { float4 c [[color(0)]]; float depth [[depth(any)]]; };
fragment FOut gs_fs(VOut in [[stage_in]], float4 dstc [[color(0)]], texture2d<float> tex [[texture(0)]], sampler hws [[sampler(0)]],
                    constant U& u [[buffer(0)]]) {
    int4 c = int4(u.iip != 0 ? in.col : in.fcol);
    int r = c.r, g = c.g, b = c.b, a = c.a;
    if (u.tme != 0) {
        float tu, tv;
        if (u.fst != 0) { tu = in.uv.x * u.hsx; tv = in.uv.y * u.hsy; }
        else { float iq = in.stq.z != 0.0 ? 1.0 / in.stq.z : 0.0; tu = in.stq.x * iq * float(u.tsx); tv = in.stq.y * iq * float(u.tsy); }
        if (in.clampv.x <= in.clampv.z) tu = clamp(tu, in.clampv.x * u.hsx, in.clampv.z * u.hsx);
        if (in.clampv.y <= in.clampv.w) tv = clamp(tv, in.clampv.y * u.hsy, in.clampv.w * u.hsy);
        int4 t;
        if (u.hw != 0) t = int4(tex.sample(hws, float2(tu, tv) / float2(u.tsx, u.tsy)) * 255.0 + 0.5);
        else if (u.bil != 0) {
            tu -= 0.5; tv -= 0.5;
            float fu = floor(tu), fv = floor(tv);
            int iu = int(fu), iv = int(fv);
            int wu = int((tu - fu) * 256.0), wv = int((tv - fv) * 256.0);
            t = lerpc(lerpc(fetch(tex, u, iu, iv), fetch(tex, u, iu + 1, iv), wu), lerpc(fetch(tex, u, iu, iv + 1), fetch(tex, u, iu + 1, iv + 1), wu), wv);
        } else t = fetch(tex, u, int(floor(tu)), int(floor(tv)));
        if (u.tfx == 0) { r = (t.r * r) >> 7; g = (t.g * g) >> 7; b = (t.b * b) >> 7; if (u.tcc != 0) a = (t.a * a) >> 7; }
        else if (u.tfx == 1) { r = t.r; g = t.g; b = t.b; if (u.tcc != 0) a = t.a; }
        else if (u.tfx == 2) { r = ((t.r * r) >> 7) + a; g = ((t.g * g) >> 7) + a; b = ((t.b * b) >> 7) + a; if (u.tcc != 0) a = t.a + a; }
        else { r = ((t.r * r) >> 7) + a; g = ((t.g * g) >> 7) + a; b = ((t.b * b) >> 7) + a; if (u.tcc != 0) a = t.a; }
        r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255); a = clamp(a, 0, 255);
    }
    if (u.fge != 0) {
        int f = int(in.fog); int3 fc = int3(int(u.fogr), int(u.fogg), int(u.fogb));
        r = (r * f + fc.r * (255 - f)) >> 8; g = (g * f + fc.g * (255 - f)) >> 8; b = (b * f + fc.b * (255 - f)) >> 8;
    }
    if (u.atst >= 0) {
        bool pass;
        if (u.atst == 0) pass = false; else if (u.atst == 1) pass = true;
        else if (u.atst == 2) pass = a < u.aref; else if (u.atst == 3) pass = a <= u.aref;
        else if (u.atst == 4) pass = a == u.aref; else if (u.atst == 5) pass = a >= u.aref;
        else if (u.atst == 6) pass = a > u.aref; else pass = a != u.aref;
        if (pass != (u.apass != 0)) discard_fragment();
    }
    int4 dst = int4(dstc * 255.0 + 0.5);
    if (u.date != 0 && ((dst.a >> 7) & 1) != u.date - 1) discard_fragment();
    int oa = a & 255;
    if (u.fba != 0) oa |= 128;
    if (u.abe != 0) {
        int cc = u.bc == 0 ? a : (u.bc == 1 ? dst.a : u.fix);
        int3 cs = int3(r, g, b), cd = dst.rgb, o;
        for (int k = 0; k < 3; k++) {
            int va = blendsel(u.ba, cs[k], cd[k]), vb = blendsel(u.bb, cs[k], cd[k]), vd = blendsel(u.bd, cs[k], cd[k]);
            o[k] = (((va - vb) * cc) >> 7) + vd;
        }
        if (u.colclamp != 0) o = clamp(o, 0, 255); else o = o & 255;
        r = o.r; g = o.g; b = o.b;
    }
    uint m = u.fbmsk | u.xmask;
    uint s = uint(r) | (uint(g) << 8) | (uint(b) << 16) | (uint(oa) << 24);
    uint dp = uint(dst.r) | (uint(dst.g) << 8) | (uint(dst.b) << 16) | (uint(dst.a) << 24);
    uint w = (s & ~m) | (dp & m);
    FOut o;
    o.c = float4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), float(w >> 24)) / 255.0;
    o.depth = min(floor(max(in.z, 0.0)), u.zmax) * (1.0 / 4294967296.0);
    return o;
}
// full-target quads: native image upload, depth upload, native read-back
struct QOut { float4 pos [[position]]; float2 uv; };
vertex QOut quad_vs(uint vid [[vertex_id]]) {
    float2 c = float2(vid & 1, vid >> 1);
    QOut o; o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0); o.uv = c; return o;
}
fragment float4 copy_fs(QOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
    uint2 p = uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)));
    return t.read(p);
}
struct DOut { float4 c [[color(0)]]; float depth [[depth(any)]]; };
fragment DOut depth_fs(QOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
    uint2 p = uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)));
    DOut o; o.c = float4(0.0); o.depth = t.read(p).r; return o;
}
struct NOut { float4 c [[color(0)]]; float z [[color(1)]]; };
fragment NOut down_fs(QOut in [[stage_in]], texture2d<float> col [[texture(0)]], depth2d<float> dep [[texture(1)]], constant uint& scale [[buffer(0)]]) {
    uint2 p = uint2(in.pos.xy) * scale + scale / 2;
    NOut o; o.c = col.read(p); o.z = dep.read(p); return o;
}
)";

struct MtlU {
    int tme, fst, iip, fge, tfx, tcc, bil, hw;
    int tsx, tsy, wrapx, wrapy, rx0, rx1, ry0, ry1;
    float hsx, hsy; int atst, aref;
    int apass, fba, abe, ba, bb, bc, bd, fix;
    int date, colclamp; u32 fbmsk; float zmax;
    float fogr, fogg, fogb; u32 xmask;
};

struct MetalBackend : GpuBackend {
    id<MTLRenderPipelineState> p_gs, p_copy, p_depth, p_down;
    id<MTLDepthStencilState> ds[9];           // [func*2 + write] for funcs never/always/gequal/greater; [8] = off
    id<MTLDepthStencilState> ds_always_write;
    id<MTLTexture> col, depth, ncol, nz, empty;
    id<MTLSamplerState> aniso_s[4];
    std::vector<id<MTLTexture>> texs;          // gpu_tex ids are 1-based indices
    std::vector<u8> mipped;
    std::vector<unsigned> free_ids, free_later;
    int w = 0, h = 0, nw = 0, nh = 0, scale = 1;
    float aniso = 0;

    const char* name() const override { return "Metal"; }
    bool exact_fetch() const override { return true; }
    bool init() {
        id<MTLLibrary> lib = make_library(kGsMSL, "renderer");
        if (!lib) return false;
        p_gs = make_pipe(lib, "gs_vs", "gs_fs", MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float, false);
        p_copy = make_pipe(lib, "quad_vs", "copy_fs", MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float, false);
        p_depth = make_pipe(lib, "quad_vs", "depth_fs", MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float, false, MTLColorWriteMaskNone);
        p_down = make_pipe(lib, "quad_vs", "down_fs", MTLPixelFormatRGBA8Unorm, MTLPixelFormatInvalid, false, MTLColorWriteMaskAll, MTLPixelFormatR32Float);
        if (!p_gs || !p_copy || !p_depth || !p_down) return false;
        static const MTLCompareFunction f[4] = {MTLCompareFunctionNever, MTLCompareFunctionAlways, MTLCompareFunctionGreaterEqual, MTLCompareFunctionGreater};
        for (int i = 0; i < 9; i++) {
            MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = i < 8 ? f[i >> 1] : MTLCompareFunctionAlways;
            d.depthWriteEnabled = i < 8 && (i & 1);
            ds[i] = [s_dev newDepthStencilStateWithDescriptor:d];
        }
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = YES;
        ds_always_write = [s_dev newDepthStencilStateWithDescriptor:d];
        u32 zero = 0;
        empty = make_tex(&zero, 1, 1, false);
        set_aniso(0);
        texs.push_back(nil); mipped.push_back(0);       // id 0 = none
        return true;
    }
    static id<MTLTexture> make_tex(const u32* px, int tw, int th, bool mips, bool priv = false) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:tw height:th mipmapped:mips ? YES : NO];
        td.usage = MTLTextureUsageShaderRead;
        if (priv || mips) td.storageMode = MTLStorageModePrivate;          // filled by the GPU (blits)
        id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
        if (px) [t replaceRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)tw * 4];
        return t;
    }
    id<MTLTexture> target_tex(MTLPixelFormat f, int tw, int th, MTLTextureUsage use, MTLStorageMode sm = MTLStorageModePrivate) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:tw height:th mipmapped:NO];
        td.usage = use; td.storageMode = sm;
        return [s_dev newTextureWithDescriptor:td];
    }
    MTLRenderPassDescriptor* pass(bool clear) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = col;
        rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = 0.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        return rp;
    }
    void set_target(int tw, int th, int wn, int hn, int sc) override {
        @autoreleasepool {
        w = tw; h = th; nw = wn; nh = hn; scale = sc;
        col = target_tex(MTLPixelFormatRGBA8Unorm, w, h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        depth = target_tex(MTLPixelFormatDepth32Float, w, h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        ncol = nil; nz = nil;
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:pass(true)] endEncoding];
        commit(cb);
    }}
    void draw_quad(id<MTLRenderCommandEncoder> e, int vw, int vh) {
        MTLViewport vp = {0, 0, (double)vw, (double)vh, 0, 1};
        [e setViewport:vp];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    void upload_target(const u32* px, int wn, int hn) override {
        @autoreleasepool {
        id<MTLTexture> t = make_tex(px, wn, hn, false);
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(false)];
        [e setRenderPipelineState:p_copy];
        [e setDepthStencilState:ds[2]];                     // always, no write (GL: depth test off)
        [e setFragmentTexture:t atIndex:0];
        draw_quad(e, std::min(wn * scale, w), std::min(hn * scale, h));
        [e endEncoding];
        commit(cb);
    }}
    void write_native(const u32* c, const float* d) override {
        @autoreleasepool {
        upload_target(c, nw, nh);
        if (!d) return;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:nw height:nh mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
        [t replaceRegion:MTLRegionMake2D(0, 0, nw, nh) mipmapLevel:0 withBytes:d bytesPerRow:(NSUInteger)nw * 4];
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(false)];
        [e setRenderPipelineState:p_depth];
        [e setDepthStencilState:ds_always_write];
        [e setFragmentTexture:t atIndex:0];
        draw_quad(e, w, h);
        [e endEncoding];
        commit(cb);
    }}
    bool read_native(std::vector<u32>& c, std::vector<float>* d) override {
        @autoreleasepool {
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        const size_t n = (size_t)nw * nh;
        id<MTLBuffer> bc = [s_dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bz = d ? [s_dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared] : nil;
        id<MTLTexture> sc = col, sz = nil;
        if (scale > 1 || d) {         // nearest down-sample (the GL blit's sample positions); depth as R32F colour
            if (!ncol) {
                ncol = target_tex(MTLPixelFormatRGBA8Unorm, nw, nh, MTLTextureUsageRenderTarget);
                nz = target_tex(MTLPixelFormatR32Float, nw, nh, MTLTextureUsageRenderTarget);
            }
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = ncol; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[1].texture = nz; rp.colorAttachments[1].loadAction = MTLLoadActionDontCare; rp.colorAttachments[1].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:p_down];
            [e setFragmentTexture:col atIndex:0];
            [e setFragmentTexture:depth atIndex:1];
            u32 s = (u32)scale;
            [e setFragmentBytes:&s length:4 atIndex:0];
            draw_quad(e, nw, nh);
            [e endEncoding];
            sc = ncol; sz = nz;
        }
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:sc sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(nw, nh, 1)
                   toBuffer:bc destinationOffset:0 destinationBytesPerRow:(NSUInteger)nw * 4 destinationBytesPerImage:n * 4];
        if (d) [bl copyFromTexture:sz sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(nw, nh, 1)
                          toBuffer:bz destinationOffset:0 destinationBytesPerRow:(NSUInteger)nw * 4 destinationBytesPerImage:n * 4];
        [bl endEncoding];
        commit(cb); [cb waitUntilCompleted];
        c.resize(n); std::memcpy(c.data(), [bc contents], n * 4);
        if (d) { d->resize(n); std::memcpy(d->data(), [bz contents], n * 4); }
        return true;
    }}
    bool read(int x0, int y0, int rw, int rh, std::vector<u32>& out) override {
        @autoreleasepool {
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        const size_t n = (size_t)rw * rh;
        id<MTLBuffer> b = [s_dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:col sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(rw, rh, 1)
                   toBuffer:b destinationOffset:0 destinationBytesPerRow:(NSUInteger)rw * 4 destinationBytesPerImage:n * 4];
        [bl endEncoding];
        commit(cb); [cb waitUntilCompleted];
        out.resize(n); std::memcpy(out.data(), [b contents], n * 4);
        return true;
    }}
    // freed textures, kept for reuse: the game replaces textures about a thousand times a second in a match
    std::unordered_map<u64, std::vector<id<MTLTexture>>> pool;
    std::deque<std::pair<u64, id<MTLTexture>>> pending;           // (serial at free time, texture)
    size_t pooled = 0;
    // texture uploads go through a shared staging buffer and are copied by the GPU at the start of the next flush
    // (replaceRegion would lay the pixels out for the GPU on the CPU, and the game uploads thousands a second)
    struct Stage { id<MTLBuffer> buf; size_t size, used; u64 last; };
    std::vector<Stage> stages; size_t stage_cur = 0;
    struct Upload { id<MTLBuffer> buf; size_t off; id<MTLTexture> tex; int w, h; size_t stage; };
    std::vector<Upload> uploads;
    id<MTLBuffer> stage_alloc(size_t n, size_t& off) {
        const u64 done = s_completed.load();
        for (size_t k = 0; k <= stages.size(); k++) {
            if (stage_cur < stages.size()) {
                Stage& st = stages[stage_cur];
                size_t o = (st.used + 255) & ~(size_t)255;
                if (o + n <= st.size) { st.used = o + n; st.last = ~0ull; off = o; return st.buf; }   // (in use until encoded)
            }
            // next chunk that the GPU has finished with, else a new one
            size_t pick = stages.size();
            for (size_t i = 0; i < stages.size(); i++) if (i != stage_cur && stages[i].last <= done && stages[i].size >= n) { pick = i; break; }
            if (pick == stages.size()) {
                Stage st; st.size = std::max<size_t>(n, 32u << 20); st.used = 0; st.last = 0;
                st.buf = [s_dev newBufferWithLength:st.size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
                if (!st.buf) return nil;
                stages.push_back(st);
            } else stages[pick].used = 0;
            stage_cur = pick;
        }
        return nil;
    }
    void encode_uploads(id<MTLCommandBuffer> cb) {
        if (uploads.empty()) return;
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        for (const Upload& u : uploads) {
            stages[u.stage].last = s_serial + 1;                     // the command buffer execute() commits next
            [bl copyFromBuffer:u.buf sourceOffset:u.off sourceBytesPerRow:(NSUInteger)u.w * 4 sourceBytesPerImage:(NSUInteger)u.w * u.h * 4
                    sourceSize:MTLSizeMake(u.w, u.h, 1) toTexture:u.tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [bl endEncoding];
        uploads.clear();
    }
    unsigned tex_create(const u32* px, int tw, int th) override {
        @autoreleasepool {
        const u64 done = s_completed.load();
        while (!pending.empty() && pending.front().first <= done) {
            id<MTLTexture> f = pending.front().second; pending.pop_front();
            pool[(u64)f.width << 20 | f.height].push_back(f);
        }
        id<MTLTexture> t = nil;
        auto it = pool.find((u64)tw << 20 | (u64)th);
        if (it != pool.end() && !it->second.empty()) { t = it->second.back(); it->second.pop_back(); pooled--; }
        else t = make_tex(nullptr, tw, th, false, true);
        size_t so = 0;
        id<MTLBuffer> sb = px ? stage_alloc((size_t)tw * th * 4, so) : nil;
        if (sb) {
            std::memcpy((u8*)[sb contents] + so, px, (size_t)tw * th * 4);
            uploads.push_back({sb, so, t, tw, th, stage_cur});
        }
        unsigned tid;
        if (!free_ids.empty()) { tid = free_ids.back(); free_ids.pop_back(); texs[tid] = t; mipped[tid] = 0; }
        else { tid = (unsigned)texs.size(); texs.push_back(t); mipped.push_back(0); }
        return tid;
    }}
    void tex_free(unsigned tid) override { if (tid < texs.size()) free_later.push_back(tid); }
    void end_flush() override {
        for (unsigned tid : free_later) {
            if (texs[tid] && !mipped[tid] && texs[tid].mipmapLevelCount == 1 && pooled < 4096) { pending.emplace_back(s_serial, texs[tid]); pooled++; }
            texs[tid] = nil; mipped[tid] = 0; free_ids.push_back(tid);                 // (command buffers keep what they use)
        }
        free_later.clear();
    }
    void set_aniso(float level) override {
        if (level == aniso && aniso_s[0]) return;
        aniso = level;
        for (int i = 0; i < 4; i++) {
            MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
            sd.minFilter = MTLSamplerMinMagFilterLinear; sd.magFilter = MTLSamplerMinMagFilterLinear; sd.mipFilter = MTLSamplerMipFilterLinear;
            sd.maxAnisotropy = (NSUInteger)std::max(1.0f, std::min(16.0f, level));
            sd.sAddressMode = (i & 1) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
            sd.tAddressMode = (i & 2) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
            aniso_s[i] = [s_dev newSamplerStateWithDescriptor:sd];
        }
    }
    uintptr_t target_handle() override { return (uintptr_t)(__bridge void*)col; }

    void execute(const float* verts, size_t nverts, const GpuBatch* batches, size_t nb) override {
        @autoreleasepool {
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        encode_uploads(cb);
        // anisotropic filtering samples mipmaps: give the textures that need them a full chain first
        if (aniso > 1.0f) {
            id<MTLBlitCommandEncoder> bl = nil;
            for (size_t i = 0; i < nb; i++) {
                const GpuState& st = batches[i].st;
                if (!(st.kind == 2 && st.tme && st.tex && st.bil && st.wms < 2 && st.wmt < 2) || st.tex >= texs.size() || mipped[st.tex] || !texs[st.tex]) continue;
                id<MTLTexture> src = texs[st.tex];
                id<MTLTexture> m = make_tex(nullptr, (int)src.width, (int)src.height, true);
                if (!bl) bl = [cb blitCommandEncoder];
                [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(src.width, src.height, 1)
                          toTexture:m destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [bl generateMipmapsForTexture:m];
                texs[st.tex] = m; mipped[st.tex] = 1;
            }
            if (bl) [bl endEncoding];
        }
        // vertices go into the staging ring too (no buffer allocation per flush)
        const size_t vbytes = nverts * GPU_VTX_FLOATS * sizeof(float);
        size_t voff = 0;
        id<MTLBuffer> vb = stage_alloc(vbytes, voff);
        const size_t vstage = stage_cur;
        if (vb) std::memcpy((u8*)[vb contents] + voff, verts, vbytes);
        else { vb = [s_dev newBufferWithBytes:verts length:vbytes options:MTLResourceStorageModeShared]; voff = 0; }
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(false)];
        MTLViewport vp = {0, 0, (double)w, (double)h, 0, 1};
        [e setViewport:vp];
        [e setRenderPipelineState:p_gs];
        [e setVertexBuffer:vb offset:voff atIndex:0];
        [e setCullMode:MTLCullModeNone];
        static const MTLPrimitiveType kModes[3] = {MTLPrimitiveTypePoint, MTLPrimitiveTypeLine, MTLPrimitiveTypeTriangle};
        static const u32 kPer[3] = {1, 2, 3};
        // state set only when it changes (most consecutive batches share the texture, sampler, depth test, scissor)
        id<MTLTexture> cur_tex = nil; id<MTLSamplerState> cur_smp = nil; id<MTLDepthStencilState> cur_ds = nil;
        MTLScissorRect cur_sr = {0, 0, 0, 0};
        for (size_t bi = 0; bi < nb; bi++) {
            const GpuBatch& b = batches[bi];
            const GpuState& st = b.st;
            int x0 = st.sx0 * scale, y0 = st.sy0 * scale;
            int x1 = std::min((st.sx1 + 1) * scale, w), y1 = std::min((st.sy1 + 1) * scale, h);
            x0 = std::max(x0, 0); y0 = std::max(y0, 0);
            if (x1 <= x0 || y1 <= y0) continue;
            MTLScissorRect sr = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
            if (sr.x != cur_sr.x || sr.y != cur_sr.y || sr.width != cur_sr.width || sr.height != cur_sr.height) { [e setScissorRect:sr]; cur_sr = sr; }
            MtlU u; std::memset(&u, 0, sizeof u);
            u.tme = st.tme; u.fst = st.fst; u.iip = st.iip || st.kind == 3 ? 1 : 0; u.fge = st.fge; u.tfx = st.tfx; u.tcc = st.tcc; u.bil = st.bil;
            const int hx = std::max<int>(st.hsx, 1), hy = std::max<int>(st.hsy, 1);
            u.tsx = std::max<int>(st.tex_w, 1) * hx; u.tsy = std::max<int>(st.tex_h, 1) * hy;
            u.wrapx = st.wms; u.wrapy = st.wmt; u.hsx = (float)hx; u.hsy = (float)hy;
            if (st.wms == 2) { u.rx0 = st.minu * hx; u.rx1 = st.maxu * hx + hx - 1; } else { u.rx0 = st.minu; u.rx1 = st.maxu; }
            if (st.wmt == 2) { u.ry0 = st.minv * hy; u.ry1 = st.maxv * hy + hy - 1; } else { u.ry0 = st.minv; u.ry1 = st.maxv; }
            u.fogr = (float)(st.fogcol & 0xff); u.fogg = (float)((st.fogcol >> 8) & 0xff); u.fogb = (float)((st.fogcol >> 16) & 0xff);
            u.fba = st.fba; u.abe = st.abe;
            u.ba = st.alpha & 3; u.bb = (st.alpha >> 2) & 3; u.bc = (st.alpha >> 4) & 3; u.bd = (st.alpha >> 6) & 3; u.fix = (st.alpha >> 8) & 0xff;
            u.colclamp = st.colclamp; u.fbmsk = st.fbmsk;
            const bool z16 = st.zpsm == 0x32 || st.zpsm == 0x3a;
            u.zmax = z16 ? 65535.0f : st.zpsm == 0x31 ? 16777215.0f : 4294967295.0f;
            u.date = (st.test & (1 << 14)) ? (int)((st.test >> 15) & 1) + 1 : 0;
            const bool hw = aniso > 1.0f && st.kind == 2 && st.tme && st.tex && st.bil && st.wms < 2 && st.wmt < 2;
            u.hw = hw ? 1 : 0;
            id<MTLTexture> t = st.tme && st.tex && st.tex < texs.size() && texs[st.tex] ? texs[st.tex] : empty;
            if (t != cur_tex) { [e setFragmentTexture:t atIndex:0]; cur_tex = t; }
            id<MTLSamplerState> smp = aniso_s[(st.wms == 0 ? 0 : 1) | (st.wmt == 0 ? 0 : 2)];
            if (smp != cur_smp) { [e setFragmentSamplerState:smp atIndex:0]; cur_smp = smp; }
            const bool zte = (st.test >> 16) & 1;
            const bool zw = zte && !st.zmsk;
            auto zstate = [&](bool write) { return zte ? ds[(((st.test >> 17) & 3) << 1) | (write ? 1 : 0)] : ds[8]; };
            auto draw = [&](bool write_z, u32 extra) {
                id<MTLDepthStencilState> z = zstate(write_z);
                if (z != cur_ds) { [e setDepthStencilState:z]; cur_ds = z; }
                u.xmask = extra;
                [e setFragmentBytes:&u length:sizeof u atIndex:0];
                [e drawPrimitives:kModes[b.mode] vertexStart:b.first vertexCount:b.count instanceCount:1 baseInstance:(b.first | kPer[b.mode] << 28)];
            };
            const bool ate = (st.test & 1) != 0;
            const u32 atst = (st.test >> 1) & 7, afail = (st.test >> 12) & 3;
            if (!ate || atst == 1) { u.atst = -1; draw(zw, 0); continue; }
            u.atst = (int)atst; u.aref = (int)((st.test >> 4) & 0xff);
            if (atst != 0) { u.apass = 1; draw(zw, 0); }
            if (afail != 0) {                                   // pixels that fail: partial writes
                u.apass = 0;
                if (afail == 1) draw(false, 0);
                else if (afail == 2) draw(zw, 0xffffffffu);
                else draw(false, 0xff000000u);
            }
        }
        [e endEncoding];
        commit(cb);
        if (vstage < stages.size() && stages[vstage].buf == vb) stages[vstage].last = s_serial;     // free once this flush has run
    }}
};
static MetalBackend* s_backend = nullptr;
GpuBackend* gpu_backend_metal() {
    if (s_backend) return s_backend;
    if (!s_dev) return nullptr;            // the Metal presenter isn't running
    MetalBackend* b = new MetalBackend();
    if (!b->init()) { delete b; return nullptr; }
    return s_backend = b;
}

// ================================================================ presenter
static const char* kPresentMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct QOut { float4 pos [[position]]; float2 uv; };
struct Rect { float4 rect; float4 uv; };     // rect: x0 y0 x1 y1 in NDC (y up); uv: u0 v0 u1 v1
vertex QOut quad_vs(uint vid [[vertex_id]], constant Rect& r [[buffer(0)]]) {
    float2 c = float2(vid & 1, vid >> 1);
    QOut o; o.pos = float4(mix(r.rect.xy, r.rect.zw, c), 0.0, 1.0); o.uv = mix(r.uv.xy, r.uv.zw, c); return o;
}
struct FxaaP { float2 rcp; float span; float reduce; };
fragment float4 fxaa_fs(QOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant FxaaP& p [[buffer(0)]]) {
    float2 uv = in.uv; float2 rcp = p.rcp;
    float3 L = float3(0.299, 0.587, 0.114);
    float3 nw = t.sample(s, uv + float2(-1.0, -1.0) * rcp).rgb, ne = t.sample(s, uv + float2(1.0, -1.0) * rcp).rgb;
    float3 sw = t.sample(s, uv + float2(-1.0, 1.0) * rcp).rgb, se = t.sample(s, uv + float2(1.0, 1.0) * rcp).rgb;
    float3 m = t.sample(s, uv).rgb;
    float lnw = dot(nw, L), lne = dot(ne, L), lsw = dot(sw, L), lse = dot(se, L), lm = dot(m, L);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    float2 dir = float2(-((lnw + lne) - (lsw + lse)), ((lnw + lsw) - (lne + lse)));
    float red = max((lnw + lne + lsw + lse) * (0.25 * 0.125), p.reduce);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);
    dir = clamp(dir * rcpMin, float2(-p.span), float2(p.span)) * rcp;
    float3 a = 0.5 * (t.sample(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb + t.sample(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 b = a * 0.5 + 0.25 * (t.sample(s, uv + dir * -0.5).rgb + t.sample(s, uv + dir * 0.5).rgb);
    float lb = dot(b, L);
    return float4((lb < lmin || lb > lmax) ? a : b, 1.0);
}
struct ResP { float2 src; float2 dst; int mode; int pad0, pad1, pad2; };
float cw(float x) { x = abs(x); return x < 1.0 ? 1.5*x*x*x - 2.5*x*x + 1.0 : x < 2.0 ? -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0 : 0.0; }
fragment float4 resample_fs(QOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant ResP& p [[buffer(0)]]) {
    if (p.mode == 2) {
        float2 q = in.uv * p.src - 0.5; float2 f = fract(q); int2 b = int2(floor(q));
        float3 acc = float3(0.0); float ws = 0.0;
        for (int j = -1; j <= 2; j++) for (int i = -1; i <= 2; i++) {
            float w = cw(float(i) - f.x) * cw(float(j) - f.y);
            int2 c = clamp(b + int2(i, j), int2(0), int2(p.src) - 1);
            acc += w * t.read(uint2(c)).rgb; ws += w;
        }
        return float4(acc / ws, 1.0);
    } else if (p.mode == 3) {
        float2 fp = p.src / p.dst;
        float2 n = clamp(ceil(fp), float2(1.0), float2(6.0));
        float3 acc = float3(0.0);
        for (float j = 0.0; j < 6.0; j++) for (float i = 0.0; i < 6.0; i++) {
            if (i >= n.x || j >= n.y) continue;
            float2 off = ((float2(i, j) + 0.5) / n - 0.5) * fp / p.src;
            acc += t.sample(s, in.uv + off).rgb;
        }
        return float4(acc / (n.x * n.y), 1.0);
    }
    return float4(t.sample(s, in.uv).rgb, 1.0);
}
struct FinP { float2 size; float sharp; float gamma; float4 color; int crt; float lines; float pad0, pad1; };
float3 px(texture2d<float> t, sampler s, float2 uv, float2 d, float2 size) { return t.sample(s, uv + d / size).rgb; }
fragment float4 final_fs(QOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant FinP& p [[buffer(0)]]) {
    float2 uv = in.uv;
    float vig = 1.0;
    if (p.crt == 2) {
        float2 c = uv * 2.0 - 1.0;
        c *= 1.0 + 0.045 * dot(c.yx, c.yx);
        uv = c * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return float4(0.0, 0.0, 0.0, 1.0);
        float2 e = uv * (1.0 - uv.yx); vig = clamp(pow(e.x * e.y * 18.0, 0.22), 0.0, 1.0);
    }
    float3 col = px(t, s, uv, float2(0.0), p.size);
    if (p.sharp > 0.0) {
        float3 b = px(t, s, uv, float2(0.0, -1.0), p.size), d = px(t, s, uv, float2(-1.0, 0.0), p.size);
        float3 f = px(t, s, uv, float2(1.0, 0.0), p.size), h = px(t, s, uv, float2(0.0, 1.0), p.size);
        float3 mn = min(min(min(d, col), min(f, b)), h), mx = max(max(max(d, col), max(f, b)), h);
        float3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, float3(1e-4)), 0.0, 1.0));
        float3 w = amp * (-1.0 / mix(8.0, 5.0, p.sharp));
        col = clamp((b * w + d * w + f * w + h * w + col) / (1.0 + 4.0 * w), 0.0, 1.0);
    }
    col = (col - 0.5) * p.color.y + 0.5 + p.color.x;
    float l = dot(col, float3(0.299, 0.587, 0.114));
    col = mix(float3(l), col, p.color.z);
    col = pow(clamp(col, 0.0, 1.0), float3(1.0 / p.gamma));
    if (p.crt >= 1) {
        float row = uv.y * p.lines;
        float ppl = p.size.y / p.lines;
        float amt = clamp((ppl - 1.6) / 1.4, 0.0, 1.0);
        float d = abs(fract(row) - 0.5) * 2.0;
        float sl = 1.0 - smoothstep(0.35, 1.0, d);
        col *= mix(1.0, 0.62 + 0.38 * sl, amt);
        col *= 1.0 + 0.14 * amt;
    }
    return float4(col * vig, 1.0);
}
struct UiOut { float4 pos [[position]]; float2 uv; float4 col; float2 local; float4 shape; int kind [[flat]]; };
vertex UiOut ui_vs(uint vid [[vertex_id]], const device float* v [[buffer(0)]], constant float2& screen [[buffer(1)]]) {
    const device float* d = v + vid * 15;
    UiOut o;
    o.pos = float4(d[0] / screen.x * 2.0 - 1.0, 1.0 - d[1] / screen.y * 2.0, 0.0, 1.0);
    o.uv = float2(d[2], d[3]); o.col = float4(d[4], d[5], d[6], d[7]); o.local = float2(d[8], d[9]);
    o.shape = float4(d[10], d[11], d[12], d[13]); o.kind = int(d[14] + 0.5);
    return o;
}
float sdbox(float2 p, float2 b, float r) { float2 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }
fragment float4 ui_fs(UiOut in [[stage_in]], texture2d<float> font [[texture(0)]], texture2d<float> img [[texture(1)]], sampler s [[sampler(0)]]) {
    float a;
    if (in.kind == 1) a = font.sample(s, in.uv, bias(-0.35)).r;
    else if (in.kind == 2) {
        float blur = max(in.shape.w, 1.0);
        float d = sdbox(in.local, in.shape.xy - blur, in.shape.z);
        a = 1.0 - smoothstep(-blur, blur, d);
        a *= a;
    } else {
        float d = sdbox(in.local, in.shape.xy, min(in.shape.z, min(in.shape.x, in.shape.y)));
        a = clamp(0.5 - d, 0.0, 1.0);
        if (in.shape.w > 0.0 && in.kind == 0) a *= clamp(0.5 + d + in.shape.w, 0.0, 1.0);
    }
    if (in.kind == 3) { float4 t = img.sample(s, in.uv); return float4(t.rgb * in.col.rgb, t.a * in.col.a * a); }
    if (in.kind == 4) { float k = in.shape.w > 0.5 ? in.uv.y : in.uv.x; return float4(in.col.rgb, in.col.a * a * k); }
    return float4(in.col.rgb, in.col.a * a);
}
)";

class MetalPresenter : public Presenter {
public:
    SDL_Window* win = nullptr;
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    id<MTLRenderPipelineState> p_fxaa, p_resample, p_final, p_ui;
    id<MTLSamplerState> s_lin, s_near, s_font;
    id<MTLTexture> game, fb1, fb2, font, empty_img;
    id<MTLTexture> ui_img[kUiImages];
    int game_w = 0, game_h = 0;
    bool have_game = false;
    std::vector<u32> captured; int cap_w = 0, cap_h = 0;
    std::vector<float> uiv;

    const char* name() const override { return "Metal"; }
    bool init(SDL_Window* w) {
        win = w;
        view = SDL_Metal_CreateView(win);
        if (!view) { fprintf(stderr, "[metal] no view (%s)\n", SDL_GetError()); return false; }
        layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { fprintf(stderr, "[metal] no device\n"); return false; }
        if (![s_dev supportsFamily:MTLGPUFamilyApple1]) {          // framebuffer fetch: Apple GPUs only
            fprintf(stderr, "[metal] %s is not an Apple GPU - using OpenGL\n", [[s_dev name] UTF8String]);
            s_dev = nil; return false;
        }
        s_queue = [s_dev newCommandQueue];
        layer.device = s_dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;                                  // screenshots read the drawable
        layer.maximumDrawableCount = 3;
        id<MTLLibrary> lib = make_library(kPresentMSL, "presenter");
        if (!lib) return false;
        p_fxaa = make_pipe(lib, "quad_vs", "fxaa_fs", MTLPixelFormatRGBA8Unorm, MTLPixelFormatInvalid, false);
        p_resample = make_pipe(lib, "quad_vs", "resample_fs", MTLPixelFormatRGBA8Unorm, MTLPixelFormatInvalid, false);
        p_final = make_pipe(lib, "quad_vs", "final_fs", MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid, false);
        p_ui = make_pipe(lib, "ui_vs", "ui_fs", MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid, true);
        if (!p_fxaa || !p_resample || !p_final || !p_ui) return false;
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.minFilter = MTLSamplerMinMagFilterLinear; sd.magFilter = MTLSamplerMinMagFilterLinear;
        s_lin = [s_dev newSamplerStateWithDescriptor:sd];
        sd.mipFilter = MTLSamplerMipFilterLinear;
        s_font = [s_dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest; sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
        s_near = [s_dev newSamplerStateWithDescriptor:sd];
        const UiAtlas& a = ui_atlas();                               // drawn at 56 px: small text samples the mips
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:a.w height:a.h mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead;
        font = [s_dev newTextureWithDescriptor:td];
        [font replaceRegion:MTLRegionMake2D(0, 0, a.w, a.h) mipmapLevel:0 withBytes:a.px.data() bytesPerRow:(NSUInteger)a.w];
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl generateMipmapsForTexture:font];
        [bl endEncoding]; [cb commit];
        u32 zero = 0;
        empty_img = tex(&zero, 1, 1);
        rlog("metal", "presenter: %s", [[s_dev name] UTF8String]);
        return true;
    }
    static id<MTLTexture> tex(const u32* px, int w, int h, MTLTextureUsage use = MTLTextureUsageShaderRead) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = use;
        id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
        if (px) [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w * 4];
        return t;
    }
    static id<MTLTexture> target(id<MTLTexture> t, int w, int h) {
        if (t && (int)t.width == w && (int)t.height == h) return t;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        return [s_dev newTextureWithDescriptor:td];
    }
    void set_vsync(bool on) override { layer.displaySyncEnabled = on ? YES : NO; }
    void drawable_size(int& w, int& h) override { SDL_Metal_GetDrawableSize(win, &w, &h); }
    void set_ui_image(int img, const u32* px, int w, int h) override {
        if (img < 0 || img >= kUiImages) return;
        @autoreleasepool { ui_img[img] = w ? tex(px, w, h) : nil; }
    }
    void set_game(const u32* px, int w, int h) override {
        @autoreleasepool {
        // a new texture each time: the previous one may still be in use by a frame in flight
        game = tex(px, w, h); game_w = w; game_h = h; have_game = true;
    }}
    bool set_game_gpu(uintptr_t handle, int x0, int y0, int w, int h) override {
        @autoreleasepool {
        if (!handle || w <= 0 || h <= 0) return false;
        id<MTLTexture> src = (__bridge id<MTLTexture>)(void*)handle;
        if (!game || game_w != w || game_h != h || !(game.usage & MTLTextureUsageRenderTarget)) {
            game = tex(nullptr, w, h, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget); game_w = w; game_h = h;
        }
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1)
                  toTexture:game destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding]; [cb commit];
        have_game = true;
        return true;
    }}
    struct RectP { float rect[4]; float uv[4]; };
    static void quad(id<MTLRenderCommandEncoder> e, float x0, float y0, float x1, float y1, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
        RectP r = {{x0, y0, x1, y1}, {u0, v0, u1, v1}};
        [e setVertexBytes:&r length:sizeof r atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    static MTLRenderPassDescriptor* rp_for(id<MTLTexture> t, bool clear) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        return rp;
    }
    void frame(int dw, int dh, const DstRect& dst, const PostParams& pp, const UiBatch& ui) override {
        @autoreleasepool {
            if (dw <= 0 || dh <= 0) return;
            if ((int)layer.drawableSize.width != dw || (int)layer.drawableSize.height != dh) layer.drawableSize = CGSizeMake(dw, dh);
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) return;
            id<MTLCommandBuffer> cb = [s_queue commandBuffer];
            const bool pic = have_game && game && dst.w > 0 && dst.h > 0;
            id<MTLTexture> src = game; int sw = game_w, sh = game_h;
            if (pic) {
                if (pp.aa) {                                     // 1. FXAA at the picture's own resolution
                    fb1 = target(fb1, sw, sh);
                    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp_for(fb1, false)];
                    [e setRenderPipelineState:p_fxaa];
                    [e setFragmentTexture:src atIndex:0]; [e setFragmentSamplerState:s_lin atIndex:0];
                    float fp[4] = {1.0f / sw, 1.0f / sh, pp.aa >= 2 ? 16.0f : 8.0f, pp.aa >= 2 ? 1.0f / 256 : 1.0f / 128};
                    [e setFragmentBytes:fp length:sizeof fp atIndex:0];
                    quad(e, -1, 1, 1, -1);
                    [e endEncoding];
                    src = fb1;
                }
                fb2 = target(fb2, dst.w, dst.h);                       // 2. resample to the output size
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp_for(fb2, false)];
                [e setRenderPipelineState:p_resample];
                [e setFragmentTexture:src atIndex:0]; [e setFragmentSamplerState:pp.scale_filter == 0 ? s_near : s_lin atIndex:0];
                int mode = pp.scale_filter;
                if (mode == 3 && sw <= dst.w && sh <= dst.h) mode = 2;
                struct { float src[2], dst[2]; int mode, p0, p1, p2; } rp = {{(float)sw, (float)sh}, {(float)dst.w, (float)dst.h}, mode, 0, 0, 0};
                [e setFragmentBytes:&rp length:sizeof rp atIndex:0];
                quad(e, -1, 1, 1, -1);
                [e endEncoding];
            }
            // 3. the window: UI under the picture, picture (sharpen + colour + CRT), UI
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp_for(drawable.texture, true)];
            MTLViewport vp = {0, 0, (double)dw, (double)dh, 0, 1};
            [e setViewport:vp];
            id<MTLBuffer> uib = ui_buffer(ui);
            if (ui.under) draw_ui(e, uib, dw, dh, ui, 0, ui.under);
            if (pic) {
                [e setRenderPipelineState:p_final];
                [e setFragmentTexture:fb2 atIndex:0]; [e setFragmentSamplerState:s_lin atIndex:0];
                struct { float size[2]; float sharp, gamma; float color[4]; int crt; float lines, p0, p1; } fp =
                    {{(float)dst.w, (float)dst.h}, pp.sharpen, pp.gamma, {pp.brightness, pp.contrast, pp.saturation, 0}, pp.crt, (float)pp.native_lines, 0, 0};
                [e setFragmentBytes:&fp length:sizeof fp atIndex:0];
                float x0 = (float)dst.x / dw * 2 - 1, x1 = (float)(dst.x + dst.w) / dw * 2 - 1;
                float y0 = 1 - (float)dst.y / dh * 2, y1 = 1 - (float)(dst.y + dst.h) / dh * 2;
                quad(e, x0, y0, x1, y1);
            }
            draw_ui(e, uib, dw, dh, ui, ui.under, ui.q.size());
            [e endEncoding];
            id<MTLBuffer> capbuf = nil;
            if (capture) {
                capture = false;
                capbuf = [s_dev newBufferWithLength:(NSUInteger)dw * dh * 4 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(dw, dh, 1)
                           toBuffer:capbuf destinationOffset:0 destinationBytesPerRow:(NSUInteger)dw * 4 destinationBytesPerImage:(NSUInteger)dw * dh * 4];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (capbuf) {
                [cb waitUntilCompleted];
                cap_w = dw; cap_h = dh;
                captured.resize((size_t)dw * dh);
                const u32* s = (const u32*)[capbuf contents];
                for (size_t i = 0; i < captured.size(); i++) { u32 c = s[i]; captured[i] = (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16); }   // BGRA -> RGBA
            }
        }
    }
    id<MTLBuffer> ui_buffer(const UiBatch& ui) {
        if (ui.q.empty()) return nil;
        uiv.clear();
        uiv.reserve(ui.q.size() * 6 * 15);
        for (const UiQuad& q : ui.q) {
            float r = ((q.color >> 16) & 0xff) / 255.0f, g = ((q.color >> 8) & 0xff) / 255.0f, b = (q.color & 0xff) / 255.0f, a = (q.color >> 24) / 255.0f;
            float hw = q.w * 0.5f, hh = q.h * 0.5f;
            const float cx[6] = {0, 1, 0, 1, 1, 0}, cy[6] = {0, 0, 1, 0, 1, 1};
            for (int i = 0; i < 6; i++) {
                float px = q.x + cx[i] * q.w, py = q.y + cy[i] * q.h;
                float u = q.u0 + (q.u1 - q.u0) * cx[i], v = q.v0 + (q.v1 - q.v0) * cy[i];
                float lx = (cx[i] - 0.5f) * q.w, ly = (cy[i] - 0.5f) * q.h;
                float v15[15] = {px, py, u, v, r, g, b, a, lx, ly, hw, hh, q.radius, q.soft, (float)q.kind};
                uiv.insert(uiv.end(), v15, v15 + 15);
            }
        }
        return [s_dev newBufferWithBytes:uiv.data() length:uiv.size() * sizeof(float) options:MTLResourceStorageModeShared];
    }
    void draw_ui(id<MTLRenderCommandEncoder> e, id<MTLBuffer> vb, int dw, int dh, const UiBatch& ui, size_t first, size_t last) {
        if (last <= first || !vb) return;
        [e setRenderPipelineState:p_ui];
        [e setVertexBuffer:vb offset:0 atIndex:0];
        float scr[2] = {(float)dw, (float)dh};
        [e setVertexBytes:scr length:sizeof scr atIndex:1];
        [e setFragmentTexture:font atIndex:0];
        [e setFragmentTexture:empty_img atIndex:1];
        [e setFragmentSamplerState:s_font atIndex:0];
        size_t i = first;
        while (i < last) {                    // one draw per run of quads using the same image
            int img = ui.q[i].kind == UI_IMAGE ? (int)ui.q[i].soft : -1;
            size_t j = i + 1;
            while (j < last && (ui.q[j].kind == UI_IMAGE ? (int)ui.q[j].soft : -1) == img) j++;
            if (img >= 0) [e setFragmentTexture:(img < kUiImages && ui_img[img] ? ui_img[img] : empty_img) atIndex:1];
            if (img < 0 || (img < kUiImages && ui_img[img])) [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:i * 6 vertexCount:(j - i) * 6];
            i = j;
        }
    }
    bool read_window(std::vector<u32>& px, int& w, int& h) override {
        if (captured.empty()) return false;
        px = captured; w = cap_w; h = cap_h; return true;
    }
};

Presenter* presenter_metal_create(SDL_Window* win) {
    MetalPresenter* p = new MetalPresenter();
    if (!p->init(win)) { delete p; return nullptr; }
    return p;
}
#endif
