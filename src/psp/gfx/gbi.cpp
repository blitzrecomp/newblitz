// The display-list walker (docs/PSP_PORT.md 4.5): F3DEX2 and F3DLX2.Rej on the RSP side, the RDP's state, and the draws
// it makes, turned into GE batches. The command decode follows tools/gbi_dump.py (the reference for the M6 checks);
// the code is our own.
//
// Geometry: G_VTX vertices keep the N64's s16 position and (G_TEXTURE-scaled) s10.5 texture coordinates, so the GE
// transforms them: GU_MODEL = the load-time modelview (times 32768, the GE's s16 scale), GU_PROJECTION = the projection.
// A run of triangles with one state and one load matrix becomes one indexed draw (8-bit indices into a copy of the
// vertex buffer range it uses). Triangles that mix vertices loaded under different matrices, and triangles the CPU
// clipped, go out as float vertices in eye space (GU_MODEL = identity) instead.
// Clipping: in F3DEX2 sections every vertex a triangle uses is transformed on the CPU once; a triangle wholly outside
// one plane is dropped, one that crosses the near, far or guard planes is clipped. F3DLX2.Rej sections (most of the
// triangles) are not clipped: the GE gets them as they are.
#include "gfx_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#ifdef __PSP__
#include <psprtc.h>
#include <pspthreadman.h>
#else
#include <chrono>
#endif

namespace blitzgfx {

namespace ge { Stats* g_stats = nullptr; }
// Where the walker is, for the HANG block (psp_gfx.cpp's gfx_breadcrumb): plain stores, read by the watchdog.
volatile uint32_t g_walk_pc = 0, g_walk_cmds = 0, g_walk_op = 0, g_walk_runs = 0;

namespace {

Config g_cfg;
Stats g_st;
bool g_inited = false;
uint32_t g_frame = 0;
std::vector<ComboInfo> g_combos;
std::unordered_map<uint64_t, int> g_combo_idx;
std::unordered_map<uint64_t, bool> g_blend_seen;
uint8_t g_op_logged[32];

uint32_t now_us() {
#ifdef __PSP__
    return (uint32_t)sceKernelGetSystemTimeWide();
#else
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// F3DEX2 opcodes
enum : uint32_t {
    G_SPNOOP = 0x00, G_VTX = 0x01, G_MODIFYVTX = 0x02, G_CULLDL = 0x03, G_BRANCH_Z = 0x04, G_TRI1 = 0x05, G_TRI2 = 0x06,
    G_QUAD = 0x07, G_LINE3D = 0x08, G_SPECIAL_3 = 0xD3, G_SPECIAL_2 = 0xD4, G_SPECIAL_1 = 0xD5, G_DMA_IO = 0xD6,
    G_TEXTURE = 0xD7, G_POPMTX = 0xD8, G_GEOMETRYMODE = 0xD9, G_MTX = 0xDA, G_MOVEWORD = 0xDB, G_MOVEMEM = 0xDC,
    G_LOAD_UCODE = 0xDD, G_DL = 0xDE, G_ENDDL = 0xDF, G_NOOP = 0xE0, G_RDPHALF_1 = 0xE1, G_SETOTHERMODE_L = 0xE2,
    G_SETOTHERMODE_H = 0xE3, G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5, G_RDPLOADSYNC = 0xE6, G_RDPPIPESYNC = 0xE7,
    G_RDPTILESYNC = 0xE8, G_RDPFULLSYNC = 0xE9, G_SETKEYGB = 0xEA, G_SETKEYR = 0xEB, G_SETCONVERT = 0xEC,
    G_SETSCISSOR = 0xED, G_SETPRIMDEPTH = 0xEE, G_RDPSETOTHERMODE = 0xEF, G_LOADTLUT = 0xF0, G_RDPHALF_2 = 0xF1,
    G_SETTILESIZE = 0xF2, G_LOADBLOCK = 0xF3, G_LOADTILE = 0xF4, G_SETTILE = 0xF5, G_FILLRECT = 0xF6,
    G_SETFILLCOLOR = 0xF7, G_SETFOGCOLOR = 0xF8, G_SETBLENDCOLOR = 0xF9, G_SETPRIMCOLOR = 0xFA, G_SETENVCOLOR = 0xFB,
    G_SETCOMBINE = 0xFC, G_SETTIMG = 0xFD, G_SETZIMG = 0xFE, G_SETCIMG = 0xFF,
};
const uint32_t G_ZBUFFER = 0x1, G_SHADE = 0x4, G_CULL_FRONT = 0x200, G_CULL_BACK = 0x400, G_FOG = 0x10000,
               G_LIGHTING = 0x20000, G_SHADING_SMOOTH = 0x200000;
const uint32_t UCODE_F3DEX2 = 0x027C10, UCODE_F3DLX2_REJ = 0x028FA0;   // G_LOAD_UCODE text, physical (gbi_dump.py agrees)
const uint32_t RT64_HOOK_W0 = 0xE0525464;
const int VTX_F3DEX2 = 32, VTX_REJ = 64, DL_STACK_MAX = 64, MTX_STACK = 10;
const uint32_t MAX_CMDS = 200000;

int gex_words(uint32_t sub) {   // RT64 extended GBI command lengths (rt64_extended_gbi.h)
    switch (sub) {
    case 0x02: case 0x08: return 3;
    case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x0C: case 0x13: case 0x14: case 0x30: case 0x31: return 2;
    default: return 1;
    }
}

inline uint32_t bits(uint32_t v, int sh, int n) { return (v >> sh) & ((1u << n) - 1); }
inline int16_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }

void mat_mul(const float* a, const float* b, float* out) {   // row vectors: v * a * b
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, t, sizeof t);
}
const float IDENT[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
const float IDENT12[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

struct MtxRec {
    float mv[16], p[16], mvp[16], model[12];
    uint32_t id, pid;
    bool affine;
};

struct Slot {
    int16_t x, y, z, s, t;
    uint8_t r, g, b, a;
    uint32_t abgr;
    int mtx;               // MtxRec index (-1: none)
    bool have_clip;
    float clip[4];
};

// A vertex on its way through the CPU clipper: eye position, clip position, attributes.
struct CVtx { float eye[3], clip[4], s, t, col[4]; };

struct ColourXform { float c0[3], cs[3], csa[3], a0, asa; bool identity; };

class Walker {
public:
    Mem m;
    Tmem tmem;

    void begin_frame() {
        tmem.begin_frame(g_frame);
        recs_.clear();
        mtx_cur_ = -1;
        batch_.kind = B_NONE;
        resolved_sver_ = 0;
    }
    void run(uint32_t dl, uint32_t ucode);
    void flush();
    void reset_rdp() {
        geometry_ = 0; omh_ = 0; oml_ = 0; cc_w0_ = 0; cc_w1_ = 0;
        prim_ = env_ = fog_ = blend_ = fill_ = 0; prim_lodfrac_ = 0;
        for (Tile& t : tiles_) t = Tile();
        scissor_[0] = 0; scissor_[1] = 0; scissor_[2] = 512 * 4; scissor_[3] = 256 * 4;
    }

private:
    enum { B_NONE, B_S16, B_FLOAT };
    struct Batch {
        int kind = B_NONE;
        uint32_t sver = 0;
        int mtx = -1;
        uint32_t pid = 0;
        int lo = 64, hi = -1;
        std::vector<uint8_t> idx;
        std::vector<GVtxF> fv;
        ColourXform col;
        int combo = -1;
    };

    // RSP
    uint32_t seg_[16] = {};
    bool rej_ = false;
    int gex_op_ = -1;
    int vtx_size_ = VTX_F3DEX2;
    Slot vtx_[VTX_REJ] = {};
    float proj_[16], mv_[16];
    float mv_stack_[MTX_STACK][16];
    int mv_sp_ = 0;
    bool have_proj_ = false;
    int mtx_cur_ = -1;           // MtxRec for the current matrices, -1 when they changed
    std::vector<MtxRec> recs_;
    uint32_t mtx_serial_ = 2, proj_serial_ = 1;
    uint32_t cur_pid_ = 0;
    int16_t vp_[8] = {1024, 512, 511, 0, 1024, 512, 511, 0};
    uint32_t tex_on_ = 0, tex_tile_ = 0, tex_levels_ = 0, tex_s_ = 0, tex_t_ = 0;
    uint32_t rdphalf1_ = 0, rdphalf2_ = 0;
    // RDP
    uint32_t geometry_ = 0, omh_ = 0, oml_ = 0, cc_w0_ = 0, cc_w1_ = 0;
    uint32_t prim_ = 0, env_ = 0, fog_ = 0, blend_ = 0, fill_ = 0;
    uint8_t prim_lodfrac_ = 0;
    Tile tiles_[8];
    Timg timg_;
    uint32_t cimg_ = 0, cimg_width_ = 512, zimg_ = 0;
    uint32_t scissor_[4] = {0, 0, 2048, 1024};
    // texture rectangle in progress
    bool rect_pending_ = false;
    float rect_[4];
    int rect_tile_ = 0;
    bool rect_flip_ = false;
    float rect_s_ = 0, rect_t_ = 0, rect_dsdx_ = 1, rect_dtdy_ = 1;
    // draws
    uint32_t sver_ = 1, resolved_sver_ = 0;
    Batch batch_;
    CombineOut cc_out_;
    int combo_ = -1;
    ColourXform col_;
    float tex_w_ = 1, tex_h_ = 1;

    float kx() const { return 480.0f / (float)(cimg_width_ ? cimg_width_ : 512); }
    float ky() const { return 272.0f / (cimg_width_ == 512 || cimg_width_ == 0 ? 256.0f : 240.0f); }
    uint32_t seg(uint32_t a) const { return (seg_[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) & 0x00FFFFFF; }
    void dirty() { sver_++; }

    void exec(uint32_t op, uint32_t w0, uint32_t w1, int& action, uint32_t& target);
    void read_mtx(uint32_t addr, float* out) const;
    int load_mtx();
    void clip_of(Slot& v);
    static int outcode(const float* c, float g);
    void tri(int a, int b, int c);
    void clip_tri(int a, int b, int c, int planes);
    void emit_float(const CVtx* v, int n, int mtx);
    void begin_batch(int kind, int mtx, uint32_t pid);
    void resolve_tris();
    void apply_texture(int tile_idx, bool linear, bool through);
    int note_combiner(const CombineOut& c, bool two);
    void texrect();
    void fillrect(uint32_t w0, uint32_t w1);
    void apply_scissor();
    void apply_viewport();
    uint32_t shade_colour(const ColourXform& x, const Slot& s) const;
    uint32_t xform(const ColourXform& x, float r, float g, float b, float a) const;
    static ColourXform make_xform(const CombineOut& c);
};

ColourXform Walker::make_xform(const CombineOut& c) {
    ColourXform x;
    memcpy(x.c0, c.c0, sizeof x.c0);
    memcpy(x.cs, c.cs, sizeof x.cs);
    memcpy(x.csa, c.csa, sizeof x.csa);
    x.a0 = c.a0;
    x.asa = c.asa;
    x.identity = true;
    for (int i = 0; i < 3; i++) if (x.c0[i] != 0 || x.cs[i] != 1 || x.csa[i] != 0) x.identity = false;
    if (x.a0 != 0 || x.asa != 1) x.identity = false;
    return x;
}

uint32_t Walker::xform(const ColourXform& x, float r, float g, float b, float a) const {
    float sh[3] = {r, g, b};
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        float v = x.c0[i] + x.cs[i] * sh[i] + x.csa[i] * a;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        out |= (uint32_t)lrintf(v * 255.0f) << (i * 8);
    }
    float va = x.a0 + x.asa * a;
    va = va < 0 ? 0 : va > 1 ? 1 : va;
    return out | (uint32_t)lrintf(va * 255.0f) << 24;
}

uint32_t Walker::shade_colour(const ColourXform& x, const Slot& s) const {
    if (x.identity) return s.abgr;
    return xform(x, s.r / 255.0f, s.g / 255.0f, s.b / 255.0f, s.a / 255.0f);
}

void Walker::read_mtx(uint32_t addr, float* out) const {
    for (int i = 0; i < 16; i++) {
        int16_t hi = (int16_t)m.u16(addr + i * 2);
        uint16_t lo = m.u16(addr + 32 + i * 2);
        out[i] = (float)hi + lo / 65536.0f;
    }
}

int Walker::load_mtx() {
    if (mtx_cur_ >= 0) return mtx_cur_;
    MtxRec r;
    memcpy(r.mv, mv_, sizeof r.mv);
    memcpy(r.p, proj_, sizeof r.p);
    mat_mul(r.mv, r.p, r.mvp);
    r.affine = fabsf(r.mv[3]) < 1e-6f && fabsf(r.mv[7]) < 1e-6f && fabsf(r.mv[11]) < 1e-6f && fabsf(r.mv[15] - 1) < 1e-6f;
    if (!r.affine) g_st.nonaffine_mv++;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) r.model[i * 3 + j] = r.mv[i * 4 + j] * (i < 3 ? 32768.0f : 1.0f);
    r.id = mtx_serial_++;
    r.pid = cur_pid_;
    recs_.push_back(r);
    mtx_cur_ = (int)recs_.size() - 1;
    return mtx_cur_;
}

void Walker::clip_of(Slot& v) {
    if (v.have_clip) return;
    v.have_clip = true;
    if (v.mtx < 0) { v.clip[0] = v.x; v.clip[1] = v.y; v.clip[2] = v.z; v.clip[3] = 1; return; }
    const float* M = recs_[v.mtx].mvp;
    float x = v.x, y = v.y, z = v.z;
    for (int j = 0; j < 4; j++) v.clip[j] = x * M[j] + y * M[4 + j] + z * M[8 + j] + M[12 + j];
}

// Bits: 1 -x, 2 +x, 4 -y, 8 +y (at g * w), 16 far, 32 near.
int Walker::outcode(const float* c, float g) {
    float x = c[0], y = c[1], z = c[2], w = c[3];
    int o = 0;
    if (x < -g * w) o |= 1;
    if (x > g * w) o |= 2;
    if (y < -g * w) o |= 4;
    if (y > g * w) o |= 8;
    if (z > w) o |= 16;
    if (w < 0.0f || z < -w) o |= 32;
    return o;
}

void Walker::apply_scissor() {
    float x0 = scissor_[0] / 4.0f * kx(), y0 = scissor_[1] / 4.0f * ky();
    float x1 = scissor_[2] / 4.0f * kx(), y1 = scissor_[3] / 4.0f * ky();
    ge::set_scissor((int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1));
}

void Walker::apply_viewport() {
    ge::set_viewport(vp_[0] / 4.0f * kx(), -vp_[1] / 4.0f * ky(), vp_[4] / 4.0f * kx(), vp_[5] / 4.0f * ky());
}

int Walker::note_combiner(const CombineOut& c, bool two) {
    uint64_t key = ((uint64_t)(0xFC000000u | cc_w0_) << 32) | cc_w1_;
    uint64_t k2 = key ^ ((uint64_t)two << 63);
    auto it = g_combo_idx.find(k2);
    if (it != g_combo_idx.end()) return it->second;
    ComboInfo ci;
    ci.key = key;
    ci.cycles = two ? 2 : 1;
    ci.exact = c.exact;
    ci.uses = 0;
    describe_combiner(ci.desc, sizeof ci.desc, cc_w0_, cc_w1_, two, c);
    g_combos.push_back(ci);
    int idx = (int)g_combos.size() - 1;
    g_combo_idx[k2] = idx;
    note("%s combiner key 0x%016llX (%d-cycle): %s", c.exact ? "new" : "UNKNOWN", (unsigned long long)key, ci.cycles, ci.desc);
    return idx;
}

void Walker::apply_texture(int tile_idx, bool linear, bool through) {
    const Tile& t = tiles_[tile_idx & 7];
    int tlut = (int)bits(omh_, 14, 2);
    const TexEntry* e = tmem.get(m, t, tlut);
    if (!e) { ge::set_texture(nullptr); return; }
    ge::set_texture(&e->tex);
    ge::set_tex_sampling(linear, e->repeat_s, e->repeat_t);
    tex_w_ = (float)e->tex.w;
    tex_h_ = (float)e->tex.h;
    if (!through) {
        auto shiftmul = [](int sh) { return sh == 0 ? 1.0f : sh <= 10 ? 1.0f / (float)(1 << sh) : (float)(1 << (16 - sh)); };
        float off = linear ? 0.5f : 0.0f;
        float su = 1024.0f * shiftmul(t.shifts) / tex_w_, sv = 1024.0f * shiftmul(t.shiftt) / tex_h_;
        float ou = (-(float)t.uls / 4.0f + off) / tex_w_, ov = (-(float)t.ult / 4.0f + off) / tex_h_;
        ge::set_texcoord(su, sv, ou, ov);
    }
}

void Walker::resolve_tris() {
    if (resolved_sver_ == sver_) return;
    resolved_sver_ = sver_;
    bool two = bits(omh_, 20, 2) == 1;
    CombineOut c = solve_combiner(cc_w0_, cc_w1_, two, tex_on_ != 0, prim_, env_, prim_lodfrac_);
    cc_out_ = c;
    combo_ = note_combiner(c, two);
    col_ = make_xform(c);
    BlendOut b = solve_blender(omh_, oml_, (geometry_ & G_ZBUFFER) != 0, blend_);
    if (!b.exact) {
        uint64_t k = ((uint64_t)omh_ << 32) | oml_;
        if (!g_blend_seen.count(k)) { g_blend_seen[k] = true; note("blender approximated: othermode 0x%08X 0x%08X", omh_, oml_); }
    }
    ge::set_blend(b.blend, b.src, b.dst);
    ge::set_alpha_test(b.alpha_test, b.alpha_ref);
    ge::set_depth(b.depth_test, b.depth_write);
    uint32_t cm = geometry_ & (G_CULL_FRONT | G_CULL_BACK);
    ge::set_cull(cm == G_CULL_BACK ? ge::CULL_N64_BACK : cm == G_CULL_FRONT ? ge::CULL_N64_FRONT : ge::CULL_NONE);
    ge::set_shade_smooth((geometry_ & G_SHADING_SMOOTH) || !(geometry_ & G_SHADE));
    ge::set_dither(bits(omh_, 6, 2) != 3);
    apply_scissor();
    apply_viewport();
    if (c.textured) {
        // LOD is off in v1, so the draw samples its base level: G_TEXTURE's tile, or the next one in detail mode
        // (textlod LOD + textdetail DETAIL: the tile itself is the detail texture; the field's markings are in tile + 1)
        int base = (int)tex_tile_;
        if (bits(omh_, 16, 1) && bits(omh_, 17, 2) == 2) base = (base + 1) & 7;
        else if (c.use_tex1) base = (base + 1) & 7;   // mostly TEXEL1 (e.g. a PRIM_LOD_FRAC crossfade over 0.5)
        apply_texture(base, bits(omh_, 12, 2) != 0, false);
        ge::set_texfunc(c.func, c.tcc_rgba, c.env_abgr);
    } else {
        ge::set_texture(nullptr);
    }
    if (!c.exact) g_st.cc_inexact_draws++;
    if (!b.exact) g_st.bl_inexact_draws++;
}

void Walker::begin_batch(int kind, int mtx, uint32_t pid) {
    resolve_tris();
    batch_.kind = kind;
    batch_.sver = sver_;
    batch_.mtx = mtx;
    batch_.pid = pid;
    batch_.lo = 64;
    batch_.hi = -1;
    batch_.idx.clear();
    batch_.fv.clear();
    batch_.col = col_;
    batch_.combo = combo_;
    const MtxRec* r = mtx >= 0 ? &recs_[mtx] : nullptr;
    if (kind == B_S16) {
        if (r) { ge::set_model(r->model, r->id); ge::set_proj(r->p, r->pid); }
    } else {
        ge::set_model(IDENT12, 1);
        if (r) ge::set_proj(r->p, r->pid);
        else ge::set_proj(proj_, cur_pid_);
    }
    if (combo_ >= 0) g_combos[combo_].uses++;
    g_st.batches++;
}

void Walker::flush() {
    Batch& b = batch_;
    if (b.kind == B_S16 && !b.idx.empty()) {
        int n = b.hi - b.lo + 1;
        // one allocation: a list restart between two would hand out the first block again
        GVtx16* v = (GVtx16*)ge::list_mem((size_t)n * sizeof(GVtx16) + b.idx.size());
        uint8_t* ix = (uint8_t*)(v + n);
        for (int i = 0; i < n; i++) {
            const Slot& s = vtx_[b.lo + i];
            v[i].u = s.s; v[i].v = s.t;
            v[i].c = shade_colour(b.col, s);
            v[i].x = s.x; v[i].y = s.y; v[i].z = s.z; v[i].pad = 0;
        }
        for (size_t i = 0; i < b.idx.size(); i++) ix[i] = (uint8_t)(b.idx[i] - b.lo);
        ge::draw_indexed(v, ix, (int)b.idx.size());
        g_st.tris_s16 += (uint32_t)b.idx.size() / 3;
    } else if (b.kind == B_FLOAT && !b.fv.empty()) {
        GVtxF* v = (GVtxF*)ge::list_mem(b.fv.size() * sizeof(GVtxF));
        memcpy(v, b.fv.data(), b.fv.size() * sizeof(GVtxF));
        ge::draw_float(v, (int)b.fv.size());
        g_st.tris_float += (uint32_t)b.fv.size() / 3;
    }
    b.kind = B_NONE;
    b.idx.clear();
    b.fv.clear();
}

void Walker::emit_float(const CVtx* v, int n, int mtx) {
    uint32_t pid = mtx >= 0 ? recs_[mtx].pid : cur_pid_;
    if (batch_.kind != B_FLOAT || batch_.sver != sver_ || batch_.pid != pid || batch_.fv.size() > 3000) {
        flush();
        begin_batch(B_FLOAT, mtx, pid);
    }
    for (int i = 1; i + 1 < n; i++) {
        const CVtx* t3[3] = {&v[0], &v[i], &v[i + 1]};
        for (const CVtx* c : t3) {
            GVtxF f;
            f.u = c->s / 32768.0f;
            f.v = c->t / 32768.0f;
            f.c = xform(batch_.col, c->col[0], c->col[1], c->col[2], c->col[3]);
            f.x = c->eye[0]; f.y = c->eye[1]; f.z = c->eye[2];
            batch_.fv.push_back(f);
        }
    }
}

void Walker::clip_tri(int a, int b, int c, int planes) {
    CVtx poly[2][12];
    int n = 3;
    int ids[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        Slot& s = vtx_[ids[i]];
        CVtx& o = poly[0][i];
        const MtxRec* r = s.mtx >= 0 ? &recs_[s.mtx] : nullptr;
        float x = s.x, y = s.y, z = s.z;
        if (r) for (int j = 0; j < 3; j++) o.eye[j] = x * r->mv[j] + y * r->mv[4 + j] + z * r->mv[8 + j] + r->mv[12 + j];
        else { o.eye[0] = x; o.eye[1] = y; o.eye[2] = z; }
        memcpy(o.clip, s.clip, sizeof o.clip);
        o.s = s.s; o.t = s.t;
        o.col[0] = s.r / 255.0f; o.col[1] = s.g / 255.0f; o.col[2] = s.b / 255.0f; o.col[3] = s.a / 255.0f;
    }
    float g = g_cfg.guard;
    const float in = 0.999f;
    int cur = 0;
    for (int p = 0; p < 6 && n >= 3; p++) {
        int bit = 1 << p;
        if (!(planes & bit)) continue;
        auto d = [&](const CVtx& v) {
            const float* q = v.clip;
            switch (p) {
            case 0: return q[0] + g * q[3];
            case 1: return g * q[3] - q[0];
            case 2: return q[1] + g * q[3];
            case 3: return g * q[3] - q[1];
            case 4: return q[3] * in - q[2];
            default: return q[2] + q[3] * in;
            }
        };
        int m2 = 0;
        CVtx* src = poly[cur];
        CVtx* dst = poly[cur ^ 1];
        for (int i = 0; i < n && m2 < 11; i++) {
            const CVtx& A = src[i];
            const CVtx& B = src[(i + 1) % n];
            float da = d(A), db = d(B);
            if (da >= 0) dst[m2++] = A;
            if ((da >= 0) != (db >= 0) && m2 < 12) {
                float f = da / (da - db);
                CVtx& o = dst[m2++];
                for (int k = 0; k < 3; k++) o.eye[k] = A.eye[k] + (B.eye[k] - A.eye[k]) * f;
                for (int k = 0; k < 4; k++) o.clip[k] = A.clip[k] + (B.clip[k] - A.clip[k]) * f;
                for (int k = 0; k < 4; k++) o.col[k] = A.col[k] + (B.col[k] - A.col[k]) * f;
                o.s = A.s + (B.s - A.s) * f;
                o.t = A.t + (B.t - A.t) * f;
            }
        }
        n = m2;
        cur ^= 1;
    }
    if (n < 3) { g_st.tris_culled++; return; }
    g_st.tris_clipped++;
    emit_float(poly[cur], n, vtx_[a].mtx);
}

void Walker::tri(int a, int b, int c) {
    g_st.tris++;
    if (a >= vtx_size_ || b >= vtx_size_ || c >= vtx_size_) return;
    if ((geometry_ & (G_CULL_FRONT | G_CULL_BACK)) == (G_CULL_FRONT | G_CULL_BACK)) { g_st.tris_culled++; return; }
    Slot &A = vtx_[a], &B = vtx_[b], &C = vtx_[c];
    if (!rej_ && g_cfg.near_clip && have_proj_) {
        clip_of(A); clip_of(B); clip_of(C);
        int oa = outcode(A.clip, g_cfg.guard), ob = outcode(B.clip, g_cfg.guard), oc = outcode(C.clip, g_cfg.guard);
        if (oa & ob & oc) { g_st.tris_culled++; return; }
        if (oa | ob | oc) {
            if (batch_.kind == B_S16) flush();
            clip_tri(a, b, c, oa | ob | oc);
            return;
        }
    }
    if (A.mtx != B.mtx || B.mtx != C.mtx) {
        g_st.tris_mixed++;
        if (batch_.kind == B_S16) flush();
        for (Slot* s : {&A, &B, &C}) clip_of(*s);
        clip_tri(a, b, c, 0);
        g_st.tris_clipped--;   // not a clip, only the float path
        return;
    }
    uint32_t pid = A.mtx >= 0 ? recs_[A.mtx].pid : cur_pid_;
    if (batch_.kind != B_S16 || batch_.sver != sver_ || batch_.mtx != A.mtx || batch_.idx.size() > 3000) {
        flush();
        begin_batch(B_S16, A.mtx, pid);
    }
    for (int i : {a, b, c}) {
        batch_.idx.push_back((uint8_t)i);
        if (i < batch_.lo) batch_.lo = i;
        if (i > batch_.hi) batch_.hi = i;
    }
}

void Walker::texrect() {
    rect_pending_ = false;
    flush();
    resolved_sver_ = 0;   // the rect sets its own state
    g_st.texrects++;
    int cyc = (int)bits(omh_, 20, 2);
    bool copy = cyc == 2;
    float xl = rect_[0], yl = rect_[1], xh = rect_[2], yh = rect_[3];
    float x1 = copy || cyc == 3 ? xh + 1 : xh, y1 = copy || cyc == 3 ? yh + 1 : yh;
    float dsdx = copy ? rect_dsdx_ / 4.0f : rect_dsdx_, dtdy = rect_dtdy_;
    bool n64_linear = !copy && bits(omh_, 12, 2) != 0;
    bool linear = n64_linear || g_cfg.rect_linear;
    ge::set_cull(ge::CULL_NONE);
    ge::set_depth(false, false);
    ge::set_shade_smooth(true);
    ge::set_dither(bits(omh_, 6, 2) != 3);
    apply_scissor();
    uint32_t colour = 0xFFFFFFFFu;
    bool textured = true, use_t1 = false;
    if (copy) {
        ge::set_blend(false, 0, 0);
        ge::set_alpha_test((oml_ & 3) != 0, 1);
        ge::set_texfunc(ge::TFX_MODULATE, true, 0);
    } else {
        bool two = cyc == 1;
        CombineOut c = solve_combiner(cc_w0_, cc_w1_, two, true, prim_, env_, prim_lodfrac_);
        int ci = note_combiner(c, two);
        if (ci >= 0) g_combos[ci].uses++;
        if (!c.exact) g_st.cc_inexact_draws++;
        BlendOut b = solve_blender(omh_, oml_, false, blend_);
        ge::set_blend(b.blend, b.src, b.dst);
        ge::set_alpha_test(b.alpha_test, b.alpha_ref);
        colour = xform(make_xform(c), 1, 1, 1, 1);
        textured = c.textured;
        use_t1 = c.use_tex1;
        if (textured) ge::set_texfunc(c.func, c.tcc_rgba, c.env_abgr);
    }
    int rtile = (rect_tile_ + (use_t1 ? 1 : 0)) & 7;
    const Tile& t = tiles_[rtile];
    if (textured) apply_texture(rtile, linear, true);
    else ge::set_texture(nullptr);
    auto shiftmul = [](int sh) { return sh == 0 ? 1.0f : sh <= 10 ? 1.0f / (float)(1 << sh) : (float)(1 << (16 - sh)); };
    float ms = shiftmul(t.shifts), mt = shiftmul(t.shiftt);
    // The N64 samples pixel x (its left edge) at s + (x - xl) * dsdx; the GE interpolates the corners' u/v to pixel
    // centres. Point (nearest on the GE): corner u = s - dsdx/2, so a centre lands on the N64's texel. N64 bilinear has
    // texel i at s = i, the GE's at i + 0.5: corner u = s - dsdx/2 + 0.5. A point-sampled rect drawn with GE linear
    // (rect_linear) keeps the continuous texel position: corner u = s.
    float half_s = n64_linear || !linear ? -0.5f * dsdx : 0.0f, half_t = n64_linear || !linear ? -0.5f * dtdy : 0.0f;
    float off = n64_linear ? 0.5f : 0.0f, eps = linear ? 0.0f : 1.0f / 64;
    auto U = [&](float s) { return (s * ms) - t.uls / 4.0f + off + eps; };
    auto V = [&](float tt) { return (tt * mt) - t.ult / 4.0f + off + eps; };
    float s0 = rect_s_ + half_s, t0 = rect_t_ + half_t;
    float X0 = xl * kx(), Y0 = yl * ky(), X1 = x1 * kx(), Y1 = y1 * ky();
    GVtxF* v = (GVtxF*)ge::list_mem(sizeof(GVtxF) * 4);
    if (!rect_flip_) {
        float s1 = s0 + (x1 - xl) * dsdx, t1 = t0 + (y1 - yl) * dtdy;
        v[0] = {U(s0), V(t0), colour, X0, Y0, 0};
        v[1] = {U(s1), V(t1), colour, X1, Y1, 0};
        ge::draw_rect(v, false);
    } else {   // s runs down, t across
        float s1 = s0 + (y1 - yl) * dsdx, t1 = t0 + (x1 - xl) * dtdy;
        v[0] = {U(s0), V(t0), colour, X0, Y0, 0};
        v[1] = {U(s0), V(t1), colour, X1, Y0, 0};
        v[2] = {U(s1), V(t0), colour, X0, Y1, 0};
        v[3] = {U(s1), V(t1), colour, X1, Y1, 0};
        ge::draw_rect(v, true);
    }
}

void Walker::fillrect(uint32_t w0, uint32_t w1) {
    flush();
    resolved_sver_ = 0;
    g_st.fillrects++;
    int cyc = (int)bits(omh_, 20, 2);
    float xh = bits(w0, 12, 12) / 4.0f, yh = bits(w0, 0, 12) / 4.0f, xl = bits(w1, 12, 12) / 4.0f, yl = bits(w1, 0, 12) / 4.0f;
    if (cyc >= 2) { xh += 1; yh += 1; }
    GVtxF* v = (GVtxF*)ge::list_mem(sizeof(GVtxF) * 2);
    ge::set_texture(nullptr);
    ge::set_cull(ge::CULL_NONE);
    ge::set_alpha_test(false, 0);
    apply_scissor();
    float X0 = xl * kx(), Y0 = yl * ky(), X1 = xh * kx(), Y1 = yh * ky();
    if (cimg_ == zimg_) {   // the z buffer: a depth clear (to the far value)
        g_st.zclears++;
        ge::set_blend(true, ge::BF_ZERO, ge::BF_ONE);
        ge::set_depth(false, true);
        v[0] = {0, 0, 0, X0, Y0, 0};
        v[1] = {0, 0, 0, X1, Y1, 0};
        ge::draw_rect(v, false);
        return;
    }
    uint32_t colour;
    ge::set_depth(false, false);
    if (cyc == 3) {
        uint16_t c = (uint16_t)(fill_ >> 16);
        uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
        colour = ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
        ge::set_blend(false, 0, 0);
    } else {
        bool two = cyc == 1;
        CombineOut c = solve_combiner(cc_w0_, cc_w1_, two, false, prim_, env_, prim_lodfrac_);
        BlendOut b = solve_blender(omh_, oml_, false, blend_);
        ge::set_blend(b.blend, b.src, b.dst);
        colour = xform(make_xform(c), 1, 1, 1, 1);
    }
    v[0] = {0, 0, colour, X0, Y0, 0};
    v[1] = {0, 0, colour, X1, Y1, 0};
    ge::draw_rect(v, false);
}

void Walker::run(uint32_t dl, uint32_t ucode) {
    memset(seg_, 0, sizeof seg_);
    rej_ = (ucode & 0x00FFFFFF) == UCODE_F3DLX2_REJ;
    vtx_size_ = rej_ ? VTX_REJ : VTX_F3DEX2;
    gex_op_ = -1;
    memcpy(proj_, IDENT, sizeof proj_);
    cur_pid_ = 0x80000000u | proj_serial_++;   // a new id: the identity is not the previous list's projection
    memcpy(mv_, IDENT, sizeof mv_);
    mv_sp_ = 0;
    have_proj_ = false;
    mtx_cur_ = -1;
    for (Slot& s : vtx_) { memset(&s, 0, sizeof s); s.mtx = -1; }
    rect_pending_ = false;
    dirty();
    uint32_t stack[DL_STACK_MAX];
    int sp = 0;
    uint32_t pc = seg(dl);
    g_walk_runs++;
    g_walk_cmds = 0;
    while (true) {
        if (!m.valid(pc, 8)) { note("display list PC 0x%08X is outside RDRAM; stopping", pc); break; }
        uint32_t w0 = m.u32(pc), w1 = m.u32(pc + 4);
        g_walk_pc = pc;
        g_walk_op = w0 >> 24;
        g_walk_cmds++;
        pc += 8;
        if (++g_st.cmds > MAX_CMDS) { note("stopped after %u commands", MAX_CMDS); break; }
        if (rej_) g_st.rej_cmds++;
        uint32_t op = w0 >> 24;
        if ((int)op == gex_op_) {
            pc += 8 * (uint32_t)(gex_words(w0 & 0xFFFFFF) - 1);
            g_st.ex_cmds++;
            g_st.op_counts[0x100]++;
            continue;
        }
        g_st.op_counts[op]++;
        if (rect_pending_ && op != G_RDPHALF_1 && op != G_RDPHALF_2) texrect();
        int action = 0;
        uint32_t target = 0;
        exec(op, w0, w1, action, target);
        if (action == 1) {   // call
            if (sp >= DL_STACK_MAX) { g_st.dl_overflow++; note("display list stack overflow"); break; }
            stack[sp++] = pc;
            pc = target;
            g_st.dl_calls++;
        } else if (action == 2) {
            pc = target;
        } else if (action == 3) {
            if (!sp) break;
            pc = stack[--sp];
        }
    }
    if (rect_pending_) texrect();
    flush();
}

void Walker::exec(uint32_t op, uint32_t w0, uint32_t w1, int& action, uint32_t& target) {
    switch (op) {
    case G_VTX: {
        int n = (int)bits(w0, 12, 8);
        int v0 = (int)bits(w0, 1, 7) - n;
        uint32_t src = seg(w1);
        g_st.vtx_cmds++;
        // a queued indexed batch reads these slots: draw it first
        if (batch_.kind == B_S16 && !batch_.idx.empty() && v0 <= batch_.hi && v0 + n > batch_.lo) flush();
        int mi = load_mtx();
        bool lit = geometry_ & G_LIGHTING, fog = geometry_ & G_FOG;
        for (int i = 0; i < n; i++) {
            int k = v0 + i;
            if (k < 0 || k >= vtx_size_) continue;
            uint32_t a = src + (uint32_t)i * 16;
            uint32_t q0 = m.u32(a), q1 = m.u32(a + 4), q2 = m.u32(a + 8), q3 = m.u32(a + 12);
            Slot& s = vtx_[k];
            s.x = s16(q0 >> 16); s.y = s16(q0); s.z = s16(q1 >> 16);
            int sraw = s16(q2 >> 16), traw = s16(q2);
            s.s = (int16_t)((sraw * (int)tex_s_) >> 16);   // the RSP's vmudm: s * scale >> 16
            s.t = (int16_t)((traw * (int)tex_t_) >> 16);
            s.r = (uint8_t)(q3 >> 24); s.g = (uint8_t)(q3 >> 16); s.b = (uint8_t)(q3 >> 8); s.a = (uint8_t)q3;
            if (lit) { s.r = s.g = s.b = 160; g_st.lit_vtx++; }   // lighting is not done in v1 (gbi_dump.py's grey)
            if (fog) s.a = 255;                                     // fog is off in v1: shade alpha would be the fog factor
            s.abgr = s.r | (s.g << 8) | (s.b << 16) | ((uint32_t)s.a << 24);
            s.mtx = mi;
            s.have_clip = false;
            g_st.vtx_loaded++;
        }
        break;
    }
    case G_MODIFYVTX: {
        uint32_t where = bits(w0, 16, 8);
        int vi = (int)(bits(w0, 0, 16) / 2);
        g_st.modifyvtx++;
        if (vi < 0 || vi >= vtx_size_) break;
        if (batch_.kind == B_S16 && !batch_.idx.empty() && vi >= batch_.lo && vi <= batch_.hi) flush();
        Slot& s = vtx_[vi];
        if (where == 0x10) {
            s.r = (uint8_t)(w1 >> 24); s.g = (uint8_t)(w1 >> 16); s.b = (uint8_t)(w1 >> 8); s.a = (uint8_t)w1;
            s.abgr = s.r | (s.g << 8) | (s.b << 16) | ((uint32_t)s.a << 24);
        } else if (where == 0x14) {
            s.s = s16(w1 >> 16);
            s.t = s16(w1);
        }   // 0x18 XYSCREEN / 0x1C ZSCREEN: screen-space edits, not done in v1
        break;
    }
    case G_CULLDL: {
        int v0 = (int)(bits(w0, 0, 16) / 2), vn = (int)(bits(w1, 0, 16) / 2);
        if (!have_proj_) break;
        int common = 0x3F;
        bool any = false;
        for (int i = v0; i <= vn; i++) {
            if (i < 0 || i >= vtx_size_) continue;
            clip_of(vtx_[i]);
            common &= outcode(vtx_[i].clip, 1.0f);
            any = true;
        }
        if (any && common) action = 3;
        break;
    }
    case G_BRANCH_Z: {
        int vi = (int)(bits(w0, 0, 12) / 2);
        target = seg(rdphalf1_);
        if (vi < 0 || vi >= vtx_size_ || !have_proj_) break;
        Slot& v = vtx_[vi];
        clip_of(v);
        if (fabsf(v.clip[3]) < 1e-9f) break;
        double vz = ((double)v.clip[2] / v.clip[3] * vp_[2] + vp_[6]) * 65536.0;
        if ((int64_t)vz <= (int64_t)(int32_t)w1) action = 2;
        break;
    }
    case G_TRI1:
        g_st.tri_cmds++;
        tri((int)bits(w0, 16, 8) / 2, (int)bits(w0, 8, 8) / 2, (int)bits(w0, 0, 8) / 2);
        break;
    case G_TRI2: case G_QUAD:
        g_st.tri_cmds++;
        tri((int)bits(w0, 16, 8) / 2, (int)bits(w0, 8, 8) / 2, (int)bits(w0, 0, 8) / 2);
        tri((int)bits(w1, 16, 8) / 2, (int)bits(w1, 8, 8) / 2, (int)bits(w1, 0, 8) / 2);
        break;
    case G_LINE3D: case G_DMA_IO: case G_SPECIAL_1: case G_SPECIAL_2: case G_SPECIAL_3:
    case G_RDPLOADSYNC: case G_RDPPIPESYNC: case G_RDPTILESYNC: case G_RDPFULLSYNC:
    case G_SETKEYGB: case G_SETKEYR: case G_SETCONVERT: case G_SPNOOP:
        break;
    case G_NOOP:
        if (w0 == RT64_HOOK_W0) {
            uint32_t hook = w1 >> 28;
            if (hook == 1) gex_op_ = (int)(w1 & 0xFF);
            else if (hook == 2) gex_op_ = -1;
        }
        break;
    case G_TEXTURE:
        tex_levels_ = bits(w0, 11, 3); tex_tile_ = bits(w0, 8, 3); tex_on_ = bits(w0, 1, 7);
        tex_s_ = w1 >> 16; tex_t_ = w1 & 0xFFFF;
        dirty();
        break;
    case G_POPMTX: {
        uint32_t n = w1 / 64;
        for (uint32_t i = 0; i < n && mv_sp_ > 0; i++) memcpy(mv_, mv_stack_[--mv_sp_], sizeof mv_);
        mtx_cur_ = -1;
        break;
    }
    case G_GEOMETRYMODE: {
        uint32_t clear = (~w0) & 0x00FFFFFF;
        geometry_ = (geometry_ & ~clear) | w1;
        dirty();
        break;
    }
    case G_MTX: {
        uint32_t p = bits(w0, 0, 8) ^ 0x01;
        bool push = p & 1, load = p & 2, proj = p & 4;
        float mtx[16];
        read_mtx(seg(w1), mtx);
        if (proj) {
            if (load || !have_proj_) memcpy(proj_, mtx, sizeof proj_);
            else mat_mul(mtx, proj_, proj_);
            have_proj_ = true;
            cur_pid_ = 0x80000000u | proj_serial_++;
        } else {
            if (push && mv_sp_ < MTX_STACK) memcpy(mv_stack_[mv_sp_++], mv_, sizeof mv_);
            if (load) memcpy(mv_, mtx, sizeof mv_);
            else mat_mul(mtx, mv_, mv_);
        }
        mtx_cur_ = -1;
        break;
    }
    case G_MOVEWORD: {
        uint32_t index = bits(w0, 16, 8), offset = bits(w0, 0, 16);
        if (index == 0x06) seg_[(offset / 4) & 0xF] = w1 & 0x00FFFFFF;
        break;
    }
    case G_MOVEMEM: {
        uint32_t idx = bits(w0, 0, 8);
        uint32_t src = seg(w1);
        if (idx == 8) {
            for (int i = 0; i < 8; i++) vp_[i] = (int16_t)m.u16(src + i * 2);
            dirty();
        }
        break;
    }
    case G_LOAD_UCODE: {
        uint32_t text = seg(w1);
        flush();
        g_st.ucode_switches++;
        if (text == UCODE_F3DLX2_REJ) rej_ = true;
        else if (text == UCODE_F3DEX2) rej_ = false;
        else note("G_LOAD_UCODE with unknown text 0x%08X: decoding on as %s", text, rej_ ? "F3DLX2.Rej" : "F3DEX2");
        vtx_size_ = rej_ ? VTX_REJ : VTX_F3DEX2;
        break;
    }
    case G_DL:
        target = seg(w1);
        action = bits(w0, 16, 8) ? 2 : 1;
        break;
    case G_ENDDL:
        action = 3;
        break;
    case G_RDPHALF_1:
        rdphalf1_ = w1;
        if (rect_pending_) { rect_s_ = s16(w1 >> 16) / 32.0f; rect_t_ = s16(w1) / 32.0f; }
        break;
    case G_RDPHALF_2:
        rdphalf2_ = w1;
        if (rect_pending_) {
            rect_dsdx_ = s16(w1 >> 16) / 1024.0f;
            rect_dtdy_ = s16(w1) / 1024.0f;
            texrect();
        }
        break;
    case G_SETOTHERMODE_L: case G_SETOTHERMODE_H: {
        uint32_t len = bits(w0, 0, 8) + 1;
        int shift = 32 - (int)bits(w0, 8, 8) - (int)len;
        if (shift < 0) shift = 0;
        uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
        if (op == G_SETOTHERMODE_H) omh_ = (omh_ & ~mask) | (w1 & mask);
        else oml_ = (oml_ & ~mask) | (w1 & mask);
        dirty();
        break;
    }
    case G_TEXRECT: case G_TEXRECTFLIP:
        flush();
        rect_[2] = bits(w0, 12, 12) / 4.0f; rect_[3] = bits(w0, 0, 12) / 4.0f;
        rect_tile_ = (int)bits(w1, 24, 3);
        rect_[0] = bits(w1, 12, 12) / 4.0f; rect_[1] = bits(w1, 0, 12) / 4.0f;
        rect_flip_ = op == G_TEXRECTFLIP;
        rect_s_ = rect_t_ = 0;
        rect_dsdx_ = rect_dtdy_ = 1;
        rect_pending_ = true;
        break;
    case G_SETSCISSOR:
        scissor_[0] = bits(w0, 12, 12); scissor_[1] = bits(w0, 0, 12); scissor_[2] = bits(w1, 12, 12); scissor_[3] = bits(w1, 0, 12);
        dirty();
        break;
    case G_SETPRIMDEPTH:
        break;
    case G_RDPSETOTHERMODE:
        omh_ = (w0 & 0x00FFFFFF) | (omh_ & 0xFF000000);
        oml_ = w1;
        dirty();
        break;
    case G_LOADTLUT: case G_SETTILESIZE: case G_LOADTILE: case G_LOADBLOCK: {
        int sl = (int)bits(w0, 12, 12), tl = (int)bits(w0, 0, 12);
        int ti = (int)bits(w1, 24, 3), sh = (int)bits(w1, 12, 12), th = (int)bits(w1, 0, 12);
        Tile& t = tiles_[ti];
        if (op == G_SETTILESIZE) { t.uls = (uint16_t)sl; t.ult = (uint16_t)tl; t.lrs = (uint16_t)sh; t.lrt = (uint16_t)th; }
        else if (op == G_LOADBLOCK) tmem.load_block(m, timg_, t, sl, tl, sh, th);
        else if (op == G_LOADTILE) tmem.load_tile(m, timg_, t, sl, tl, sh, th);
        else tmem.load_tlut(m, timg_, t, sl, tl, sh, th);
        dirty();
        break;
    }
    case G_SETTILE: {
        Tile& t = tiles_[bits(w1, 24, 3)];
        t.fmt = (uint8_t)bits(w0, 21, 3); t.siz = (uint8_t)bits(w0, 19, 2); t.line = (uint16_t)bits(w0, 9, 9);
        t.tmem = (uint16_t)bits(w0, 0, 9);
        t.palette = (uint8_t)bits(w1, 20, 4); t.cmt = (uint8_t)bits(w1, 18, 2); t.maskt = (uint8_t)bits(w1, 14, 4);
        t.shiftt = (uint8_t)bits(w1, 10, 4); t.cms = (uint8_t)bits(w1, 8, 2); t.masks = (uint8_t)bits(w1, 4, 4);
        t.shifts = (uint8_t)bits(w1, 0, 4);
        dirty();
        break;
    }
    case G_FILLRECT:
        fillrect(w0, w1);
        break;
    case G_SETFILLCOLOR: fill_ = w1; break;
    case G_SETFOGCOLOR: fog_ = w1; dirty(); break;
    case G_SETBLENDCOLOR: blend_ = w1; dirty(); break;
    case G_SETENVCOLOR: env_ = w1; dirty(); break;
    case G_SETPRIMCOLOR: prim_ = w1; prim_lodfrac_ = (uint8_t)bits(w0, 0, 8); dirty(); break;
    case G_SETCOMBINE: cc_w0_ = w0 & 0x00FFFFFF; cc_w1_ = w1; dirty(); break;
    case G_SETTIMG: case G_SETZIMG: case G_SETCIMG: {
        uint32_t a = seg(w1);
        if (op == G_SETTIMG) { timg_.fmt = (uint8_t)bits(w0, 21, 3); timg_.siz = (uint8_t)bits(w0, 19, 2); timg_.width = (uint16_t)(bits(w0, 0, 12) + 1); timg_.addr = a; }
        else if (op == G_SETCIMG) { cimg_ = a; cimg_width_ = bits(w0, 0, 12) + 1; dirty(); }
        else zimg_ = a;
        break;
    }
    default:
        g_st.unknown_ops++;
        if (!(g_op_logged[op >> 3] & (1 << (op & 7)))) {
            g_op_logged[op >> 3] |= (uint8_t)(1 << (op & 7));
            note("unknown GBI opcode 0x%02X (w0=%08X w1=%08X)", op, w0, w1);
        }
        break;
    }
}

Walker* g_walker = nullptr;

}  // namespace

void note(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_cfg.log) g_cfg.log(buf);
}

const Config& config() { return g_cfg; }

bool init(const Config& cfg) {
    g_cfg = cfg;
    memset(&g_st, 0, sizeof g_st);
    ge::g_stats = &g_st;
    memset(g_op_logged, 0, sizeof g_op_logged);
    if (!ge::init(cfg)) return false;
    if (!g_walker) g_walker = new Walker();
    g_walker->reset_rdp();
    g_inited = true;
    return true;
}

void shutdown() {
    if (!g_inited) return;
    ge::shutdown();
    delete g_walker;
    g_walker = nullptr;
    g_inited = false;
}

void begin_frame() {
    memset(&g_st, 0, sizeof g_st);
    g_frame++;
    ge::begin_frame();
    g_walker->begin_frame();
}

void submit_dl(const uint8_t* rdram, uint32_t rdram_size, uint32_t dl, uint32_t ucode_text) {
    uint32_t t0 = now_us();
    g_walker->m.base = rdram;
    g_walker->m.size = rdram_size;
    g_walker->run(dl, ucode_text);
    g_st.walk_us += now_us() - t0;
}

void end_frame() {
    g_walker->flush();
    g_st.ge_list_bytes = ge::list_used();
    g_st.ge_us = ge::end_frame();
}

void present() { ge::present(); }
const Stats& stats() { return g_st; }

void reset_texture_cache() {
    if (g_walker) g_walker->tmem.reset_cache();
}

void texture_memory(uint32_t& vram, uint32_t& ram) { ge::tex_split(vram, ram); }
void* draw_buffer() { return ge::draw_buffer(); }

int combiner_list(ComboInfo* out, int max) {
    for (int i = 0; i < (int)g_combos.size() && i < max; i++) out[i] = g_combos[i];
    return (int)g_combos.size();
}

}  // namespace blitzgfx
