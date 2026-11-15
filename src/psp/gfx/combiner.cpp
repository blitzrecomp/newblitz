// Combiner and blender (docs/PSP_PORT.md 4.5), our own code.
//
// The colour combiner computes (A - B) * C + D per cycle. Each input is written as a polynomial over the texel
// (T = TEXEL0's rgb, Ta = its alpha) and the shade colour (S, Sa) with constant coefficients: 3 x 3 terms
// {1, T, Ta} x {1, S, Sa}. Constants (PRIM, ENV, PRIM_LOD_FRAC, ...) are folded in with their current values, so the
// two cycles reduce to one such polynomial for rgb and one for alpha. The GE can do:
//   rgb  = Cf * Ct (MODULATE), Cf + Ct (ADD), Cf * (1 - Ct) + Cc * Ct (BLEND), Cf (untextured)
//   alpha = At * Af (TCC_RGBA) or Af (TCC_RGB)
// where Cf/Af is the vertex colour, which we compute per vertex as an affine function of the shade colour, and Cc is
// the env colour register. Whatever doesn't fit (a product of texel and shade plus another term, texel alpha in rgb, a
// texel term with a per-vertex offset and a slope other than 1) falls back to MODULATE and is reported once.
// v1 has no second texture and no LOD: TEXEL1 is TEXEL0 and LOD_FRACTION is 0. NOISE is 0.5; K4, K5, CENTER and SCALE 0.
#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>

namespace blitzgfx {
namespace {

struct Poly {
    float k[3][3][3];   // [tex term: 1, T, Ta][shade term: 1, S, Sa][channel]
    bool bad;
};

Poly zero() { Poly p; memset(&p, 0, sizeof p); return p; }
Poly constant(float r, float g, float b) { Poly p = zero(); p.k[0][0][0] = r; p.k[0][0][1] = g; p.k[0][0][2] = b; return p; }
Poly constant1(float v) { return constant(v, v, v); }
Poly term(int t, int s) { Poly p = zero(); for (int c = 0; c < 3; c++) p.k[t][s][c] = 1.0f; return p; }

bool nz(float v) { return fabsf(v) > 1e-4f; }

Poly sub(const Poly& a, const Poly& b) {
    Poly r = zero();
    r.bad = a.bad || b.bad;
    for (int t = 0; t < 3; t++) for (int s = 0; s < 3; s++) for (int c = 0; c < 3; c++) r.k[t][s][c] = a.k[t][s][c] - b.k[t][s][c];
    return r;
}
Poly add(const Poly& a, const Poly& b) {
    Poly r = zero();
    r.bad = a.bad || b.bad;
    for (int t = 0; t < 3; t++) for (int s = 0; s < 3; s++) for (int c = 0; c < 3; c++) r.k[t][s][c] = a.k[t][s][c] + b.k[t][s][c];
    return r;
}
Poly mul(const Poly& a, const Poly& b) {
    Poly r = zero();
    r.bad = a.bad || b.bad;
    for (int t1 = 0; t1 < 3; t1++) for (int s1 = 0; s1 < 3; s1++) for (int t2 = 0; t2 < 3; t2++) for (int s2 = 0; s2 < 3; s2++)
        for (int c = 0; c < 3; c++) {
            float v = a.k[t1][s1][c] * b.k[t2][s2][c];
            if (!nz(v)) continue;
            if ((t1 && t2) || (s1 && s2)) { r.bad = true; continue; }   // T*T, S*S: the GE has no such product
            r.k[t1 | t2][s1 | s2][c] += v;
        }
    return r;
}
// An alpha polynomial used as an rgb input (COMBINED_ALPHA): channel 0 to all three.
Poly broadcast(const Poly& a) {
    Poly r = a;
    for (int t = 0; t < 3; t++) for (int s = 0; s < 3; s++) r.k[t][s][1] = r.k[t][s][2] = a.k[t][s][0];
    return r;
}

struct Ctx {
    bool tex_on;
    int only;                // 0: TEXEL1 is TEXEL0 (v1); 1/2: only TEXEL0/TEXEL1 is the texel, the other is 0 (weighing)
    float prim[4], env[4], plf;
    Poly comb_rgb, comb_a;   // the previous cycle (zero in cycle 1)
};

// k: 0 TEXEL0, 1 TEXEL1
Poly tex_rgb(const Ctx& x, int k) { return !x.tex_on ? constant1(0.0f) : x.only && x.only != k + 1 ? constant1(0.0f) : term(1, 0); }
Poly tex_a(const Ctx& x, int k) { return !x.tex_on ? constant1(1.0f) : x.only && x.only != k + 1 ? constant1(0.0f) : term(2, 0); }

// rgb inputs. which: 0 = A, 1 = B, 2 = C, 3 = D.
Poly rgb_in(const Ctx& x, int which, int v) {
    switch (v) {
    case 0: return x.comb_rgb;
    case 1: case 2: return tex_rgb(x, v - 1);
    case 3: return constant(x.prim[0], x.prim[1], x.prim[2]);
    case 4: return term(0, 1);
    case 5: return constant(x.env[0], x.env[1], x.env[2]);
    }
    if (which == 0) return v == 6 ? constant1(1.0f) : v == 7 ? constant1(0.5f) : constant1(0.0f);   // 1, NOISE
    if (which == 1) return constant1(0.0f);                                                        // CENTER, K4, 0
    if (which == 3) return v == 6 ? constant1(1.0f) : constant1(0.0f);
    switch (v) {   // C
    case 6: return constant1(0.0f);                  // SCALE
    case 7: return broadcast(x.comb_a);
    case 8: case 9: return tex_a(x, v - 8);
    case 10: return constant1(x.prim[3]);
    case 11: return term(0, 2);
    case 12: return constant1(x.env[3]);
    case 13: return constant1(0.0f);                 // LOD_FRACTION
    case 14: return constant1(x.plf);
    default: return constant1(0.0f);                 // K5, 0
    }
}

// alpha inputs: A, B, D share one list; C has LOD_FRACTION and PRIM_LOD_FRAC in place of COMBINED and 1.
Poly alpha_in(const Ctx& x, int which, int v) {
    if (which == 2) {
        switch (v) {
        case 0: return constant1(0.0f);
        case 1: case 2: return tex_a(x, v - 1);
        case 3: return constant1(x.prim[3]);
        case 4: return term(0, 2);
        case 5: return constant1(x.env[3]);
        case 6: return constant1(x.plf);
        default: return constant1(0.0f);
        }
    }
    switch (v) {
    case 0: return x.comb_a;
    case 1: case 2: return tex_a(x, v - 1);
    case 3: return constant1(x.prim[3]);
    case 4: return term(0, 2);
    case 5: return constant1(x.env[3]);
    case 6: return constant1(1.0f);
    default: return constant1(0.0f);
    }
}

void unpack(uint32_t c, float out[4]) {
    out[0] = (c >> 24) / 255.0f;
    out[1] = ((c >> 16) & 255) / 255.0f;
    out[2] = ((c >> 8) & 255) / 255.0f;
    out[3] = (c & 255) / 255.0f;
}

uint32_t abgr(const float c[3], float a) {
    auto q = [](float v) { return (uint32_t)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f); };
    return q(c[0]) | (q(c[1]) << 8) | (q(c[2]) << 16) | (q(a) << 24);
}

uint32_t bits(uint32_t v, int sh, int n) { return (v >> sh) & ((1u << n) - 1); }

}  // namespace

namespace {
CombineOut solve(uint32_t w0, uint32_t w1, bool two_cycle, bool tex_on, uint32_t prim, uint32_t env, uint8_t prim_lodfrac);
struct Memo { uint32_t w0, w1, prim, env, misc; bool valid; CombineOut out; };
Memo g_memo[64];
}  // namespace

// The solver is a few thousand float operations; the same inputs come back for every batch, so a small direct-mapped
// memo answers almost every call.
CombineOut solve_combiner(uint32_t w0, uint32_t w1, bool two_cycle, bool tex_on, uint32_t prim, uint32_t env,
                          uint8_t prim_lodfrac) {
    uint32_t misc = (uint32_t)prim_lodfrac | (two_cycle ? 0x100u : 0) | (tex_on ? 0x200u : 0);
    uint32_t h = (w0 * 0x9E3779B1u) ^ (w1 * 0x85EBCA6Bu) ^ (prim * 0xC2B2AE35u) ^ (env * 0x27D4EB2Fu) ^ misc;
    Memo& m = g_memo[(h ^ (h >> 16)) & 63];
    if (m.valid && m.w0 == w0 && m.w1 == w1 && m.prim == prim && m.env == env && m.misc == misc) return m.out;
    m.out = solve(w0, w1, two_cycle, tex_on, prim, env, prim_lodfrac);
    m.w0 = w0; m.w1 = w1; m.prim = prim; m.env = env; m.misc = misc; m.valid = true;
    return m.out;
}

namespace {
CombineOut solve(uint32_t w0, uint32_t w1, bool two_cycle, bool tex_on, uint32_t prim, uint32_t env, uint8_t prim_lodfrac) {
    Ctx x;
    x.tex_on = tex_on;
    unpack(prim, x.prim);
    unpack(env, x.env);
    x.plf = prim_lodfrac / 255.0f;
    x.only = 0;
    // cycle fields (gbi.h G_SETCOMBINE layout)
    int ra[2] = {(int)bits(w0, 20, 4), (int)bits(w0, 5, 4)}, rc[2] = {(int)bits(w0, 15, 5), (int)bits(w0, 0, 5)};
    int aa[2] = {(int)bits(w0, 12, 3), (int)bits(w1, 21, 3)}, ac[2] = {(int)bits(w0, 9, 3), (int)bits(w1, 18, 3)};
    int rb[2] = {(int)bits(w1, 28, 4), (int)bits(w1, 24, 4)}, rd[2] = {(int)bits(w1, 15, 3), (int)bits(w1, 6, 3)};
    int ab[2] = {(int)bits(w1, 12, 3), (int)bits(w1, 3, 3)}, ad[2] = {(int)bits(w1, 9, 3), (int)bits(w1, 0, 3)};
    auto run = [&](Ctx& c) {
        c.comb_rgb = constant1(0.0f);
        c.comb_a = constant1(0.0f);
        for (int cy = 0; cy < (two_cycle ? 2 : 1); cy++) {
            Poly rgb = add(mul(sub(rgb_in(c, 0, ra[cy]), rgb_in(c, 1, rb[cy])), rgb_in(c, 2, rc[cy])), rgb_in(c, 3, rd[cy]));
            Poly a = add(mul(sub(alpha_in(c, 0, aa[cy]), alpha_in(c, 1, ab[cy])), alpha_in(c, 2, ac[cy])), alpha_in(c, 3, ad[cy]));
            c.comb_rgb = rgb;
            c.comb_a = a;
        }
    };
    run(x);
    const Poly& F = x.comb_rgb;
    const Poly& A = x.comb_a;

    CombineOut o;
    memset(&o, 0, sizeof o);
    o.exact = !F.bad && !A.bad;
    // Which texel the result mostly is: v1 has one texture, so a combiner that is mostly TEXEL1 (a crossfade
    // (T1 - T0) * PRIM_LOD_FRAC + T0 with the fraction over 0.5, or alpha from TEXEL1) samples tile + 1.
    if (tex_on) {
        float w[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            Ctx y = x;
            y.only = k + 1;
            run(y);
            for (int s = 0; s < 3; s++)
                for (int c = 0; c < 3; c++) w[k] += fabsf(y.comb_rgb.k[1][s][c]) + fabsf(y.comb_rgb.k[2][s][c]);
            for (int s = 0; s < 3; s++) w[k] += 3 * fabsf(y.comb_a.k[2][s][0]);
        }
        o.use_tex1 = w[1] > w[0] + 1e-3f;
    }
    o.func = ge::TFX_MODULATE;

    // alpha: Af (no texel) or At * Af
    bool tex_alpha = nz(A.k[2][0][0]) || nz(A.k[2][2][0]);
    if (!tex_alpha) {
        o.tcc_rgba = false;
        o.a0 = A.k[0][0][0];
        o.asa = A.k[0][2][0];
    } else {
        o.tcc_rgba = true;
        o.a0 = A.k[2][0][0];
        o.asa = A.k[2][2][0];
        if (nz(A.k[0][0][0]) || nz(A.k[0][2][0])) o.exact = false;   // At * k + m
    }
    for (int t = 0; t < 3; t++) for (int s = 0; s < 3; s++) if (t == 1 || s == 1) if (nz(A.k[t][s][0])) o.exact = false;

    bool tex_rgb_used = false, ta_in_rgb = false;
    bool alpha_const = true, beta_zero = true, beta_const = true, alpha_one = true;
    for (int c = 0; c < 3; c++) {
        for (int s = 0; s < 3; s++) { if (nz(F.k[1][s][c])) tex_rgb_used = true; if (nz(F.k[2][s][c])) ta_in_rgb = true; }
        if (nz(F.k[1][1][c]) || nz(F.k[1][2][c])) alpha_const = false;
        if (nz(F.k[1][0][c] - 1.0f)) alpha_one = false;
        if (nz(F.k[0][0][c]) || nz(F.k[0][1][c]) || nz(F.k[0][2][c])) beta_zero = false;
        if (nz(F.k[0][1][c]) || nz(F.k[0][2][c])) beta_const = false;
    }
    if (ta_in_rgb) o.exact = false;
    auto set_cf = [&](int t) { for (int c = 0; c < 3; c++) { o.c0[c] = F.k[t][0][c]; o.cs[c] = F.k[t][1][c]; o.csa[c] = F.k[t][2][c]; } };

    if (!tex_rgb_used && !tex_alpha) {
        o.textured = false;
        set_cf(0);
    } else if (!tex_rgb_used) {
        // rgb from the vertex, alpha from the texel: BLEND with Cc = Cf (only when that colour is a constant)
        o.textured = true;
        set_cf(0);
        if (beta_const) {
            o.func = ge::TFX_BLEND;
            o.env_abgr = abgr(o.c0, 0.0f);
        } else {
            o.exact = false;
        }
    } else {
        o.textured = true;
        if (beta_zero) {
            o.func = ge::TFX_MODULATE;
            set_cf(1);
        } else if (alpha_const && alpha_one) {
            o.func = ge::TFX_ADD;
            set_cf(0);
        } else if (alpha_const && beta_const) {
            o.func = ge::TFX_BLEND;
            set_cf(0);
            float cc[3];
            for (int c = 0; c < 3; c++) {
                cc[c] = F.k[0][0][c] + F.k[1][0][c];
                if (cc[c] < -1e-3f || cc[c] > 1.0f + 2.0f / 255.0f) o.exact = false;   // 0.75 + 64/255 is 1.004
            }
            o.env_abgr = abgr(cc, 0.0f);
        } else {
            o.exact = false;
        }
    }
    if (!o.exact) {   // MODULATE with the shade colour (logged by the caller)
        o.textured = tex_on;
        o.func = ge::TFX_MODULATE;
        o.tcc_rgba = true;
        for (int c = 0; c < 3; c++) { o.c0[c] = 0; o.cs[c] = 1; o.csa[c] = 0; }
        o.a0 = 0;
        o.asa = 1;
    }
    return o;
}
}  // namespace

void describe_combiner(char* out, size_t n, uint32_t w0, uint32_t w1, bool two_cycle, const CombineOut& c) {
    static const char* A[] = {"COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "1", "NOISE", "0", "0", "0", "0", "0", "0", "0", "0"};
    static const char* B[] = {"COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "CENTER", "K4", "0", "0", "0", "0", "0", "0", "0", "0"};
    static const char* C[] = {"COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "SCALE", "COMB_A", "T0_A", "T1_A", "PRIM_A",
                              "SHADE_A", "ENV_A", "LOD", "PRIM_LOD", "K5", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0",
                              "0", "0", "0", "0", "0", "0"};
    static const char* D[] = {"COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "1", "0"};
    static const char* AA[] = {"COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "1", "0"};
    static const char* AC[] = {"LOD", "T0", "T1", "PRIM", "SHADE", "ENV", "PRIM_LOD", "0"};
    static const char* F[] = {"MODULATE", "DECAL", "BLEND", "REPLACE", "ADD"};
    char c1[64], c2[64];
    snprintf(c1, sizeof c1, "(%s-%s)*%s+%s/(%s-%s)*%s+%s", A[bits(w0, 20, 4)], B[bits(w1, 28, 4)], C[bits(w0, 15, 5)],
             D[bits(w1, 15, 3)], AA[bits(w0, 12, 3)], AA[bits(w1, 12, 3)], AC[bits(w0, 9, 3)], AA[bits(w1, 9, 3)]);
    snprintf(c2, sizeof c2, " | (%s-%s)*%s+%s/(%s-%s)*%s+%s", A[bits(w0, 5, 4)], B[bits(w1, 24, 4)], C[bits(w0, 0, 5)],
             D[bits(w1, 6, 3)], AA[bits(w1, 21, 3)], AA[bits(w1, 3, 3)], AC[bits(w1, 18, 3)], AA[bits(w1, 0, 3)]);
    snprintf(out, n, "%s%s -> %s%s%s%s", c1, two_cycle ? c2 : "", c.textured ? F[c.func] : "untextured",
             c.textured ? (c.tcc_rgba ? "/RGBA" : "/RGB") : "", c.textured && c.use_tex1 ? " (tile+1)" : "",
             c.exact ? "" : " (FALLBACK)");
}

// The blender: P * A + M * B per cycle (in two-cycle mode the second cycle decides; the first is the fog cycle, and
// fog is off in v1). Without FORCE_BL the RDP blends only on coverage edges, so the surface is opaque.
BlendOut solve_blender(uint32_t h, uint32_t lo, bool zbuffer, uint32_t blend_color) {
    BlendOut o;
    memset(&o, 0, sizeof o);
    o.exact = true;
    int cyc = (int)bits(h, 20, 2);
    if (cyc >= 2) return o;   // copy / fill: no blender, no z
    int n = cyc == 1 ? 1 : 0;
    int P = (int)bits(lo, 30 - 2 * n, 2), Af = (int)bits(lo, 26 - 2 * n, 2), M = (int)bits(lo, 22 - 2 * n, 2),
        Bf = (int)bits(lo, 18 - 2 * n, 2);
    // P/M: 0 IN, 1 MEM, 2 BL (blend colour), 3 FOG. A: 0 IN_A, 1 FOG_A, 2 SHADE_A, 3 0. B: 0 1-A, 1 MEM_A, 2 1, 3 0.
    bool force = lo & 0x4000;
    if (force) {
        auto fa = [&](int a) { return a == 0 ? ge::BF_SRC_ALPHA : a == 3 ? ge::BF_ZERO : ge::BF_SRC_ALPHA; };
        auto fb = [&](int b, int a) {
            if (b == 0) return a == 0 ? ge::BF_INV_SRC_ALPHA : a == 3 ? ge::BF_ONE : ge::BF_INV_SRC_ALPHA;
            if (b == 1 || b == 2) return ge::BF_ONE;   // MEM_A: the frame buffer has no real alpha here
            return ge::BF_ZERO;
        };
        if (Af == 1 || Af == 2) o.exact = false;   // FOG_A / SHADE_A as a factor: approximated by IN_A
        int p = P >= 2 ? 0 : P, m = M >= 2 ? 1 : M;   // BL/FOG colour: approximated
        if (P >= 2 || M >= 2) o.exact = false;
        if (p == 0 && m == 1) {
            o.blend = true;
            o.src = fa(Af);
            o.dst = fb(Bf, Af);
        } else if (p == 1 && m == 0) {
            o.blend = true;
            o.src = fb(Bf, Af);
            o.dst = fa(Af);
        } else if (p == 1 && m == 1) {
            o.blend = true;
            o.src = ge::BF_ZERO;
            o.dst = ge::BF_ONE;
        }
        // IN * x + IN * y: opaque
        if (o.blend && o.src == ge::BF_ONE && o.dst == ge::BF_ZERO) o.blend = false;
    }
    int ac = (int)bits(lo, 0, 2);
    if (ac == 1) {
        int ref = (int)(blend_color & 255);
        if (ref > 0) { o.alpha_test = true; o.alpha_ref = ref; }
    } else if (ac == 3) {
        o.alpha_test = true;
        o.alpha_ref = 128;
    }
    if ((lo & 0x1000) && !o.blend) {   // CVG_X_ALPHA: coverage from alpha, so low alpha leaves no pixel
        o.alpha_test = true;
        if (o.alpha_ref < 32) o.alpha_ref = 32;
    }
    o.depth_test = zbuffer && (lo & 0x10);
    o.depth_write = zbuffer && (lo & 0x20);
    return o;
}

uint32_t hash32(const void* p, size_t n, uint32_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

}  // namespace blitzgfx
