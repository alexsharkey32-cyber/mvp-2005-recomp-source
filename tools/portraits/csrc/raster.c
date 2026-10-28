// The triangle loop of tools/portraits/render.py in C (same arithmetic): perspective-correct, bilinear,
// alpha-blended in submission order with a depth test, plus the per-pixel normal of the nearest solid triangle.
#include <math.h>
#include <stdint.h>
static inline void sample(const float* t, int w, int h, float u, float v, float* o) {
    float x = u * w - 0.5f, y = v * h - 0.5f;
    if (x < 0) x = 0; if (y < 0) y = 0; if (x > w - 1.001f) x = w - 1.001f; if (y > h - 1.001f) y = h - 1.001f;
    int x0 = (int)x, y0 = (int)y; float fx = x - x0, fy = y - y0;
    const float *a = t + (y0 * w + x0) * 4, *b = a + 4, *c = a + w * 4, *d = c + 4;
    for (int k = 0; k < 4; k++) o[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}
void raster(int nt, const float* V, const int* tid, const int* tfx, const int* abe, const float** tex, const int* tw, const int* th,
            int W, int H, float scale, float x0, float y0, float zlo, float zhi, float* col, float* zb, float* cov, float* nrm) {
    for (int i = 0; i < nt; i++) {
        const float* v = V + i * 30;
        float P[3][2];
        for (int k = 0; k < 3; k++) { P[k][0] = (v[k * 10] - x0) * scale; P[k][1] = (v[k * 10 + 1] - y0) * scale; }
        float mnx = fminf(P[0][0], fminf(P[1][0], P[2][0])), mxx = fmaxf(P[0][0], fmaxf(P[1][0], P[2][0]));
        float mny = fminf(P[0][1], fminf(P[1][1], P[2][1])), mxy = fmaxf(P[0][1], fmaxf(P[1][1], P[2][1]));
        int bx0 = (int)floorf(mnx), by0 = (int)floorf(mny), bx1 = (int)ceilf(mxx) + 1, by1 = (int)ceilf(mxy) + 1;
        if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > W) bx1 = W; if (by1 > H) by1 = H;
        if (bx1 <= bx0 || by1 <= by0) continue;
        float ax = P[0][0], ay = P[0][1], bx = P[1][0], byp = P[1][1], cx = P[2][0], cy = P[2][1];
        float den = (byp - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (fabsf(den) < 1e-6f) continue;
        // the triangle's normal (render.py: depth scaled to about the screen's units)
        float pz[3]; for (int k = 0; k < 3; k++) pz[k] = (v[k * 10 + 2] - zlo) / fmaxf(zhi - zlo, 1) * (W / scale) * 0.6f;
        float e1[3] = {bx - ax, byp - ay, (pz[1] - pz[0]) * scale}, e2[3] = {cx - ax, cy - ay, (pz[2] - pz[0]) * scale};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f; for (int k = 0; k < 3; k++) n[k] /= nl;
        if (n[2] < 0) for (int k = 0; k < 3; k++) n[k] = -n[k];
        const float q0 = v[9], q1 = v[19], q2 = v[29];
        const float* T = tex[tid[i]]; int w = tw[tid[i]], h = th[tid[i]];
        for (int yy = by0; yy < by1; yy++) for (int xx = bx0; xx < bx1; xx++) {
            float px = xx + 0.5f, py = yy + 0.5f;
            float l0 = ((byp - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
            float l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
            float l2 = 1 - l0 - l1;
            if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
            float z = l0 * v[2] + l1 * v[12] + l2 * v[22];
            int o = yy * W + xx;
            if (z < zb[o]) continue;
            float qq = l0 * q0 + l1 * q1 + l2 * q2;
            float u = (l0 * v[7] * q0 + l1 * v[17] * q1 + l2 * v[27] * q2) / qq;
            float w_ = (l0 * v[8] * q0 + l1 * v[18] * q1 + l2 * v[28] * q2) / qq;
            float tx[4]; sample(T, w, h, u, w_, tx);
            float vc[4]; for (int k = 0; k < 4; k++) vc[k] = (l0 * v[3 + k] + l1 * v[13 + k] + l2 * v[23 + k]) / 128.0f;
            float add = tfx[i] == 3 ? vc[3] * 0.15f : 0;
            float a = abe[i] ? tx[3] : 1.0f; if (a < 0) a = 0; if (a > 1) a = 1;
            for (int k = 0; k < 3; k++) { float rgb = tx[k] * vc[k] + add; col[o * 3 + k] = col[o * 3 + k] * (1 - a) + rgb * a; }
            if (a > cov[o]) cov[o] = a;
            if (a > 0.5f) { zb[o] = z; nrm[o * 3] = n[0]; nrm[o * 3 + 1] = n[1]; nrm[o * 3 + 2] = n[2]; }
        }
    }
}
