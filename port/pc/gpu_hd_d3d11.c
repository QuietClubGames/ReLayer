/*
 * High-resolution renderer: redraws a recorded frame (core/gpu_hd.c) with Direct3D 11 at an integer
 * multiple of the arcade resolution, then hands the image to the presenter.
 *
 * - Textures are decoded in the pixel shader straight from a copy of the PS1 VRAM (4-bit and 8-bit palette
 *   textures, 15-bit direct), with the texture window, so results match the software renderer texel for texel.
 * - The four PS1 semi-transparency modes map onto D3D blend states. Textured semi-transparent primitives are
 *   drawn in two passes (opaque texels, then translucent texels), because the PS1 decides per texel.
 * - Vertices carry the GTE's unrounded positions and depth, so polygons don't wobble and textures are
 *   perspective-correct (the hardware divides by w). --affine turns the perspective part off.
 */
#include "d3d11_internal.h"
#include "../core/gpu.h"
#include "../core/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float x, y, w;
    float u, v;
    uint8_t r, g, b, a;
    uint16_t tx, ty, cx, cy;
    uint16_t flags, twmask, twoff, pad;
} GVtx;

enum { PASS_ALL = 0, PASS_OPAQUE_TEXELS = 1, PASS_SEMI_TEXELS = 2 };
enum { BLEND_NONE = -1 };

typedef struct {
    int start, count;
    int type;            /* 0 opaque, 1 blended, 2 split (opaque texels then blended texels) */
    int mode;            /* PS1 semi-transparency mode 0-3 */
    D3D11_RECT scissor;
} Batch;

static ID3D11Texture2D *vram_tex;
static ID3D11ShaderResourceView *vram_srv;
static ID3D11Texture2D *rt_tex;
static ID3D11RenderTargetView *rt_rtv;
static ID3D11ShaderResourceView *rt_srv;
static int rt_w, rt_h, rt_scale;
static ID3D11VertexShader *hvs;
static ID3D11PixelShader *hps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vbuf, *cbuf;
static int vbuf_cap;
static ID3D11BlendState *blend[4];
static ID3D11RasterizerState *rast;
static int g_failed;

static GVtx *verts;
static int nverts, verts_cap;
static Batch *batches;
static int nbatches, batches_cap;

int g_hd_scale_override;      /* --render-scale N / quality preset (0 = fit the window) */
int g_hd_scale_cap = 8;       /* Auto quality lowers this when the GPU can't keep up */

/* GPU time of the high-resolution pass, measured with timestamp queries (read a few frames later) */
#define NQ 4
static ID3D11Query *q_dis[NQ], *q_t0[NQ], *q_t1[NQ];
static int q_n;
static float g_gpu_ms;
float hd_d3d_gpu_ms(void) { return g_gpu_ms; }

static void gpu_timer(int begin) {
    ID3D11Device *dev = d3d_dev();
    ID3D11DeviceContext *ctx = d3d_ctx();
    int i = q_n % NQ;
    if (!q_dis[i]) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Device_CreateQuery(dev, &qd, &q_dis[i]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Device_CreateQuery(dev, &qd, &q_t0[i]);
        ID3D11Device_CreateQuery(dev, &qd, &q_t1[i]);
        if (!q_dis[i] || !q_t0[i] || !q_t1[i]) return;
    }
    if (begin) {
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)q_dis[i]);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_t0[i]);
        return;
    }
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_t1[i]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_dis[i]);
    q_n++;
    /* collect the oldest finished measurement */
    int j = q_n % NQ;
    if (q_n < NQ || !q_dis[j]) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0, t1;
    if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_dis[j], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_t0[j], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_t1[j], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        !dj.Disjoint && dj.Frequency) {
        float ms = (float)((double)(t1 - t0) * 1000.0 / (double)dj.Frequency);
        g_gpu_ms = g_gpu_ms * 0.9f + ms * 0.1f;
    }
}
int g_hd_affine;              /* --affine: PS1-style texture warping */

static const char k_hd_shader[] =
    "cbuffer C : register(b0) { float2 region; float2 origin; uint pass_mode; uint3 pad; };\n"
    "Texture2DArray<uint> vram : register(t0);\n"
    "static uint g_gen;\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; uint4 tp : TEXCOORD1; uint4 fl : TEXCOORD2; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR;\n"
    "            nointerpolation uint4 tp : TEXCOORD1; nointerpolation uint4 fl : TEXCOORD2; };\n"
    "VO vs(VI i) {\n"
    "  VO o; float2 p = (i.pos.xy - origin) / region; float w = i.pos.z;\n"
    "  o.pos = float4((p.x * 2 - 1) * w, (1 - p.y * 2) * w, 0, w);\n"
    "  o.uv = i.uv; o.col = i.col; o.tp = i.tp; o.fl = i.fl; return o; }\n"
    "uint fetch(uint x, uint y) { return vram.Load(int4(x & 1023, y & 1023, g_gen, 0)); }\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  uint flags = i.fl.x; float3 rgb = i.col.rgb; g_gen = i.fl.w;\n"
    "  if (flags & 1) {\n"
    "    uint u = ((uint)(int)floor(i.uv.x)) & 255, v = ((uint)(int)floor(i.uv.y)) & 255;\n"
    "    uint mx = i.fl.y & 31, my = (i.fl.y >> 5) & 31, ox = i.fl.z & 31, oy = (i.fl.z >> 5) & 31;\n"
    "    u = (u & ~(mx * 8)) | ((ox & mx) * 8); v = (v & ~(my * 8)) | ((oy & my) * 8);\n"
    "    uint depth = (flags >> 2) & 3; uint t;\n"
    "    if (depth == 0) { uint w = fetch(i.tp.x + (u >> 2), i.tp.y + v); t = fetch(i.tp.z + ((w >> ((u & 3) * 4)) & 15), i.tp.w); }\n"
    "    else if (depth == 1) { uint w = fetch(i.tp.x + (u >> 1), i.tp.y + v); t = fetch(i.tp.z + ((w >> ((u & 1) * 8)) & 255), i.tp.w); }\n"
    "    else t = fetch(i.tp.x + u, i.tp.y + v);\n"
    "    if (t == 0) discard;\n"
    "    bool semi_px = (flags & 16) != 0 && (t & 0x8000) != 0;\n"
    "    if (pass_mode == 1 && semi_px) discard;\n"
    "    if (pass_mode == 2 && !semi_px) discard;\n"
    "    float3 tc = float3(t & 31, (t >> 5) & 31, (t >> 10) & 31) / 31.0;\n"
    "    rgb = (flags & 2) ? tc : saturate(tc * i.col.rgb * (255.0 / 128.0));\n"
    "  }\n"
    "  return float4(rgb, 1); }\n";

void hd_d3d_release(void) {
#define REL(p, T) do { if (p) { T##_Release(p); p = NULL; } } while (0)
    REL(vram_srv, ID3D11ShaderResourceView); REL(vram_tex, ID3D11Texture2D);
    REL(rt_srv, ID3D11ShaderResourceView); REL(rt_rtv, ID3D11RenderTargetView); REL(rt_tex, ID3D11Texture2D);
    REL(hvs, ID3D11VertexShader); REL(hps, ID3D11PixelShader); REL(layout, ID3D11InputLayout);
    REL(vbuf, ID3D11Buffer); REL(cbuf, ID3D11Buffer); REL(rast, ID3D11RasterizerState);
    for (int i = 0; i < 4; i++) REL(blend[i], ID3D11BlendState);
#undef REL
    rt_w = rt_h = 0;
    vbuf_cap = 0;
}

static int init(void) {
    ID3D11Device *dev = d3d_dev();
    D3DCompileFn compile = d3d_compiler();
    if (!dev || !compile) return 0;
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(compile(k_hd_shader, sizeof k_hd_shader - 1, "hd", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(compile(k_hd_shader, sizeof k_hd_shader - 1, "hd", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err))) {
        if (err) { rt_log("hd: shader: %s", (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
        if (vb) ID3D10Blob_Release(vb);
        return 0;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &hvs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &hps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R16G16B16A16_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R16G16B16A16_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, el, 5, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), &layout);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = 1024; td.Height = 1024; td.MipLevels = 1; td.ArraySize = HD_MAX_GEN;   /* one slice per VRAM generation */
    td.Format = DXGI_FORMAT_R16_UINT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &vram_tex);
    if (vram_tex) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)vram_tex, NULL, &vram_srv);

    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

    /* PS1 semi-transparency: 0 = B/2+F/2, 1 = B+F, 2 = B-F, 3 = B+F/4 (blend factor supplies 0.5 / 0.25) */
    for (int m = 0; m < 4; m++) {
        D3D11_BLEND_DESC b;
        memset(&b, 0, sizeof b);
        D3D11_RENDER_TARGET_BLEND_DESC *t = &b.RenderTarget[0];
        t->BlendEnable = TRUE;
        t->RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        t->SrcBlendAlpha = D3D11_BLEND_ONE; t->DestBlendAlpha = D3D11_BLEND_ZERO; t->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        switch (m) {
        case 0: t->SrcBlend = D3D11_BLEND_BLEND_FACTOR; t->DestBlend = D3D11_BLEND_BLEND_FACTOR; t->BlendOp = D3D11_BLEND_OP_ADD; break;
        case 1: t->SrcBlend = D3D11_BLEND_ONE; t->DestBlend = D3D11_BLEND_ONE; t->BlendOp = D3D11_BLEND_OP_ADD; break;
        case 2: t->SrcBlend = D3D11_BLEND_ONE; t->DestBlend = D3D11_BLEND_ONE; t->BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
        default: t->SrcBlend = D3D11_BLEND_BLEND_FACTOR; t->DestBlend = D3D11_BLEND_ONE; t->BlendOp = D3D11_BLEND_OP_ADD; break;
        }
        ID3D11Device_CreateBlendState(dev, &b, &blend[m]);
    }
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = FALSE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rast);
    if (!hvs || !hps || !layout || !vram_srv || !cbuf || !rast || !blend[0]) return 0;
    rt_log("hd: high-resolution renderer ready%s", g_hd_affine ? " (affine textures)" : "");
    return 1;
}

static int ensure_target(int w, int h) {
    int scale = g_hd_scale_override;
    if (scale <= 0) {
        int ph = d3d_picture_height();
        scale = (ph + h - 1) / h;              /* at least as tall as the picture on screen */
        if (scale > g_hd_scale_cap) scale = g_hd_scale_cap;
    }
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (rt_tex && rt_w == w * scale && rt_h == h * scale) return 1;
    if (rt_srv) { ID3D11ShaderResourceView_Release(rt_srv); rt_srv = NULL; }
    if (rt_rtv) { ID3D11RenderTargetView_Release(rt_rtv); rt_rtv = NULL; }
    if (rt_tex) { ID3D11Texture2D_Release(rt_tex); rt_tex = NULL; }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)(w * scale); td.Height = (UINT)(h * scale); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device *dev = d3d_dev();
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt_tex))) return 0;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt_tex, NULL, &rt_rtv);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rt_tex, NULL, &rt_srv);
    rt_w = w * scale; rt_h = h * scale; rt_scale = scale;
    rt_log("hd: rendering at %dx (%dx%d)", scale, rt_w, rt_h);
    return rt_rtv && rt_srv;
}

/* ---------------------------------------------------------------- building the vertex list */
static GVtx *push_verts(int n) {
    if (nverts + n > verts_cap) {
        verts_cap = (nverts + n) * 2;
        verts = realloc(verts, sizeof(GVtx) * (size_t)verts_cap);
    }
    GVtx *v = &verts[nverts];
    nverts += n;
    return v;
}

static void fill_common(GVtx *g, const HdPrim *p, int textured) {
    g->tx = p->tx; g->ty = p->ty; g->cx = p->cx; g->cy = p->cy;
    g->flags = (uint16_t)((textured ? 1 : 0) | ((p->flags & HD_RAW) ? 2 : 0) | ((p->depth & 3) << 2) |
                          ((p->flags & HD_SEMI) ? 16 : 0));
    g->twmask = (uint16_t)(p->tw_mask_x | (p->tw_mask_y << 5));
    g->twoff = (uint16_t)(p->tw_off_x | (p->tw_off_y << 5));
    g->pad = p->gen;            /* which VRAM generation (texture array slice) */
}

static void vtx(GVtx *g, const HdPrim *p, float x, float y, float w, float u, float v, const HdVtx *c, int textured) {
    g->x = x; g->y = y; g->w = w; g->u = u; g->v = v;
    g->r = c->r; g->g = c->g; g->b = c->b; g->a = 255;
    fill_common(g, p, textured);
}

static void add_batch(int start, int count, int type, int mode, D3D11_RECT sc) {
    if (nbatches > 0) {
        Batch *b = &batches[nbatches - 1];
        if (b->start + b->count == start && b->type == type && b->mode == mode &&
            !memcmp(&b->scissor, &sc, sizeof sc)) { b->count += count; return; }
    }
    if (nbatches == batches_cap) {
        batches_cap = batches_cap ? batches_cap * 2 : 1024;
        batches = realloc(batches, sizeof(Batch) * (size_t)batches_cap);
    }
    batches[nbatches++] = (Batch){start, count, type, mode, sc};
}

static void build(const HdFrame *f) {
    nverts = nbatches = 0;
    float s = (float)rt_scale;
    for (int i = 0; i < f->count; i++) {
        const HdPrim *p = &f->prims[i];
        int textured = (p->flags & HD_TEX) != 0, semi = (p->flags & HD_SEMI) != 0;
        int type = !semi ? 0 : textured ? 2 : 1;
        D3D11_RECT sc;
        if (p->kind == HD_FILL) {
            sc.left = 0; sc.top = 0; sc.right = rt_w; sc.bottom = rt_h;
            type = 0;
        } else {
            sc.left = (LONG)((p->clip_x1 - f->x) * s); sc.top = (LONG)((p->clip_y1 - f->y) * s);
            sc.right = (LONG)((p->clip_x2 + 1 - f->x) * s); sc.bottom = (LONG)((p->clip_y2 + 1 - f->y) * s);
            if (sc.left < 0) sc.left = 0;
            if (sc.top < 0) sc.top = 0;
            if (sc.right > rt_w) sc.right = rt_w;
            if (sc.bottom > rt_h) sc.bottom = rt_h;
            if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
        }
        int start = nverts;
        switch (p->kind) {
        case HD_TRI: {
            GVtx *g = push_verts(3);
            for (int k = 0; k < 3; k++) {
                const HdVtx *a = &p->v[k];
                float w = g_hd_affine ? 1.0f : (a->w > 0 ? a->w : 1.0f);
                /* flat-shaded polygons use the first vertex colour everywhere */
                vtx(&g[k], p, a->x, a->y, w, a->u, a->v, (p->flags & HD_GOURAUD) ? a : &p->v[0], textured);
            }
            break;
        }
        case HD_RECT:
        case HD_FILL: {
            const HdVtx *a = &p->v[0], *b = &p->v[1];
            GVtx *g = push_verts(6);
            float xs[6] = {a->x, b->x, a->x, b->x, b->x, a->x}, ys[6] = {a->y, a->y, b->y, a->y, b->y, b->y};
            float us[6] = {a->u, b->u, a->u, b->u, b->u, a->u}, vs[6] = {a->v, a->v, b->v, a->v, b->v, b->v};
            for (int k = 0; k < 6; k++) vtx(&g[k], p, xs[k], ys[k], 1, us[k], vs[k], a, p->kind == HD_RECT && textured);
            break;
        }
        case HD_LINE: {
            /* a quad one native pixel thick that covers both end pixels */
            const HdVtx *a = &p->v[0], *b = &p->v[1];
            const HdVtx *ca = a, *cb = (p->flags & HD_GOURAUD) ? b : a;
            int horiz = fabsf(b->x - a->x) >= fabsf(b->y - a->y);
            if ((horiz && b->x < a->x) || (!horiz && b->y < a->y)) {     /* draw left-to-right / top-to-bottom */
                const HdVtx *t = a; a = b; b = t;
                const HdVtx *tc = ca; ca = cb; cb = tc;
            }
            float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y;
            float tx = horiz ? 0.0f : 1.0f, ty = horiz ? 1.0f : 0.0f;   /* thickness direction */
            if (horiz) x1 += 1; else y1 += 1;                            /* include the last pixel */
            GVtx *g = push_verts(6);
            vtx(&g[0], p, x0, y0, 1, 0, 0, ca, 0);
            vtx(&g[1], p, x1, y1, 1, 0, 0, cb, 0);
            vtx(&g[2], p, x0 + tx, y0 + ty, 1, 0, 0, ca, 0);
            vtx(&g[3], p, x1, y1, 1, 0, 0, cb, 0);
            vtx(&g[4], p, x1 + tx, y1 + ty, 1, 0, 0, cb, 0);
            vtx(&g[5], p, x0 + tx, y0 + ty, 1, 0, 0, ca, 0);
            break;
        }
        }
        add_batch(start, nverts - start, type, p->semi_mode & 3, sc);
    }
}

/* ---------------------------------------------------------------- drawing */
static void set_pass(int pass, float rw, float rh, float ox, float oy) {
    ID3D11DeviceContext *ctx = d3d_ctx();
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *fl = (float *)m.pData;
        fl[0] = rw; fl[1] = rh; fl[2] = ox; fl[3] = oy;
        ((uint32_t *)m.pData)[4] = (uint32_t)pass;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);
    }
}

ID3D11ShaderResourceView *hd_d3d_render(const HdFrame *f) {
    if (g_failed) return NULL;
    if (!hvs && !init()) { g_failed = 1; rt_log("hd: renderer unavailable, using the software picture"); return NULL; }
    if (!f || f->count == 0 || !ensure_target(f->w, f->h)) return NULL;
    ID3D11DeviceContext *ctx = d3d_ctx();
    gpu_timer(1);

    /* textures come from the PS1 VRAM as it was when this frame went on screen, plus earlier snapshots for
     * primitives whose textures the game replaced while still drawing the frame */
    for (int g = 0; g < f->ngen && g < HD_MAX_GEN; g++)
        if (f->vram[g]) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)vram_tex, (UINT)g, NULL, f->vram[g], 2048, 0);
    D3D11_MAPPED_SUBRESOURCE m;

    build(f);
    if (!nverts) return NULL;
    if (nverts > vbuf_cap) {
        if (vbuf) ID3D11Buffer_Release(vbuf);
        vbuf_cap = nverts * 2;
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)(sizeof(GVtx) * (size_t)vbuf_cap); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d_dev(), &bd, NULL, &vbuf))) { vbuf = NULL; vbuf_cap = 0; return NULL; }
    }
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return NULL;
    memcpy(m.pData, verts, sizeof(GVtx) * (size_t)nverts);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vbuf, 0);

    D3D11_VIEWPORT vp = {0, 0, (FLOAT)rt_w, (FLOAT)rt_h, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rt_rtv, NULL);
    static const FLOAT black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rt_rtv, black);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, rast);
    UINT stride = sizeof(GVtx), offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbuf, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, hvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, hps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &vram_srv);

    float rw = (float)f->w, rh = (float)f->h, ox = (float)f->x, oy = (float)f->y;
    int cur_pass = -1;
    static const FLOAT half[4] = {0.5f, 0.5f, 0.5f, 0.5f}, quarter[4] = {0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < nbatches; i++) {
        Batch *b = &batches[i];
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &b->scissor);
        int passes[2], blends[2], np = 0;
        if (b->type == 0) { passes[np] = PASS_ALL; blends[np++] = BLEND_NONE; }
        else if (b->type == 1) { passes[np] = PASS_ALL; blends[np++] = b->mode; }
        else { passes[np] = PASS_OPAQUE_TEXELS; blends[np++] = BLEND_NONE; passes[np] = PASS_SEMI_TEXELS; blends[np++] = b->mode; }
        for (int k = 0; k < np; k++) {
            if (passes[k] != cur_pass) { set_pass(passes[k], rw, rh, ox, oy); cur_pass = passes[k]; }
            if (blends[k] == BLEND_NONE) ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
            else ID3D11DeviceContext_OMSetBlendState(ctx, blend[blends[k]], blends[k] == 3 ? quarter : half, 0xFFFFFFFF);
            ID3D11DeviceContext_Draw(ctx, (UINT)b->count, (UINT)b->start);
        }
    }
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    gpu_timer(0);
    return rt_srv;
}

int hd_d3d_scale(void) { return rt_scale; }

/* Save the last high-resolution image as a 24-bit BMP (for tests and comparisons). */
#include <stdio.h>
int hd_d3d_save(const char *path) {
    if (!rt_tex) return 0;
    ID3D11Device *dev = d3d_dev();
    ID3D11DeviceContext *ctx = d3d_ctx();
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(rt_tex, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st))) return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt_tex);
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = fopen(path, "wb");
        if (f) {
            int w = (int)td.Width, h = (int)td.Height, row = (w * 3 + 3) & ~3;
            uint32_t data = (uint32_t)(row * h), file = 54 + data;
            uint8_t hdr[54] = {'B', 'M'};
            memcpy(hdr + 2, &file, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &data, 4);
            fwrite(hdr, 1, 54, f);
            uint8_t *line = malloc((size_t)row);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                memset(line, 0, (size_t)row);
                for (int x = 0; x < w; x++) { line[x * 3] = src[x * 4 + 2]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4]; }
                fwrite(line, 1, (size_t)row, f);
            }
            free(line);
            fclose(f);
            ok = 1;
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return ok;
}
