/*
 * Direct3D 11 backend for the overlay UI (port/ui): rounded rectangles drawn as signed-distance shapes in the
 * pixel shader (crisp at any size, soft shadows for free) and text from a glyph atlas rasterised with GDI at
 * the exact pixel size of the current window, so it is always sharp.
 */
#include "d3d11_internal.h"
#include "../ui/ui.h"
#include "../core/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float x, y;          /* pixels */
    float u, v;
    uint32_t col;        /* RGBA8 */
    float lx, ly;        /* position relative to the rectangle centre, pixels */
    float hx, hy;        /* half size */
    float radius, border, feather, mode;   /* mode 0 = shape, 1 = text */
    uint32_t bcol;
} UVtx;

/* ---------------------------------------------------------------- fonts */
typedef struct { float u0, v0, u1, v1, w, h, xoff, adv; } Glyph;
typedef struct { Glyph g[128 + 32]; float line_h, ascent; int extra_cp[32]; int nextra; } Font;

static const int k_extra[] = {0x00D7, 0x203A, 0x2039, 0x2022, 0x25CF, 0x2014, 0x00B7, 0x2026, 0x2190, 0x2192, 0x2713};
static Font g_fonts[UI_FONT_COUNT];
static float g_font_scale;               /* pixels per virtual unit the atlas was built for */
static ID3D11Texture2D *atlas_tex;
static ID3D11ShaderResourceView *atlas_srv;

static const struct { const char *face, *fallback; int weight; float size; } k_font_spec[UI_FONT_COUNT] = {
    [UI_FONT_BODY] = {"Segoe UI Semibold", "Segoe UI", FW_SEMIBOLD, 25},
    [UI_FONT_VALUE] = {"Segoe UI", "Segoe UI", FW_NORMAL, 23},
    [UI_FONT_SMALL] = {"Segoe UI Semibold", "Segoe UI", FW_SEMIBOLD, 19},
    [UI_FONT_HEAD] = {"Bahnschrift SemiBold", "Segoe UI Semibold", FW_SEMIBOLD, 25},
    [UI_FONT_TITLE] = {"Bahnschrift Bold", "Segoe UI Black", FW_BOLD, 50},
};

static Glyph *glyph_for(Font *f, int cp) {
    if (cp >= 32 && cp < 127) return &f->g[cp];
    for (int i = 0; i < f->nextra; i++) if (f->extra_cp[i] == cp) return &f->g[128 + i];
    return &f->g['?'];
}

static int build_atlas(float scale) {
    const int AW = 2048, AH = 1024;
    uint8_t *pix = calloc((size_t)AW * AH, 1);
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 256; bi.bmiHeader.biHeight = -256; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    int pen_x = 1, pen_y = 1, row_h = 0;
    for (int fi = 0; fi < UI_FONT_COUNT; fi++) {
        Font *f = &g_fonts[fi];
        memset(f, 0, sizeof *f);
        int px = (int)lroundf(k_font_spec[fi].size * scale);
        if (px < 8) px = 8;
        HFONT font = CreateFontA(-px, 0, 0, 0, k_font_spec[fi].weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, k_font_spec[fi].face);
        HGDIOBJ oldf = SelectObject(dc, font);
        char got[64] = {0};
        GetTextFaceA(dc, sizeof got, got);
        if (_stricmp(got, k_font_spec[fi].face) != 0) {         /* not installed: fall back */
            SelectObject(dc, oldf);
            DeleteObject(font);
            font = CreateFontA(-px, 0, 0, 0, k_font_spec[fi].weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, k_font_spec[fi].fallback);
            oldf = SelectObject(dc, font);
        }
        TEXTMETRICA tm;
        GetTextMetricsA(dc, &tm);
        f->line_h = (float)tm.tmHeight;
        f->ascent = (float)tm.tmAscent;
        f->nextra = (int)(sizeof k_extra / sizeof k_extra[0]);
        for (int i = 0; i < f->nextra; i++) f->extra_cp[i] = k_extra[i];
        int total = 95 + f->nextra;
        for (int gi = 0; gi < total; gi++) {
            int cp = gi < 95 ? 32 + gi : k_extra[gi - 95];
            Glyph *g = gi < 95 ? &f->g[cp] : &f->g[128 + (gi - 95)];
            WCHAR wc = (WCHAR)cp;
            ABC abc;
            if (!GetCharABCWidthsW(dc, (UINT)cp, (UINT)cp, &abc)) { SIZE sz; GetTextExtentPoint32W(dc, &wc, 1, &sz); abc.abcA = 0; abc.abcB = (UINT)sz.cx; abc.abcC = 0; }
            int pad = 2, left = abc.abcA < 0 ? -abc.abcA : 0;
            int cw = left + (int)abc.abcB + (abc.abcA > 0 ? abc.abcA : 0) + 2 * pad, ch = tm.tmHeight;
            if (cw > 250) cw = 250;
            memset(bits, 0, 256 * 256 * 4);
            TextOutW(dc, pad + left, 0, &wc, 1);
            GdiFlush();
            if (pen_x + cw >= AW) { pen_x = 1; pen_y += row_h + 1; row_h = 0; }
            if (pen_y + ch >= AH) break;
            for (int y = 0; y < ch && y < 256; y++)
                for (int x = 0; x < cw; x++)
                    pix[(size_t)(pen_y + y) * AW + pen_x + x] = ((uint8_t *)bits)[(y * 256 + x) * 4 + 1];
            g->u0 = (float)pen_x / AW; g->v0 = (float)pen_y / AH;
            g->u1 = (float)(pen_x + cw) / AW; g->v1 = (float)(pen_y + ch) / AH;
            g->w = (float)cw; g->h = (float)ch;
            g->xoff = (float)(-pad - left);
            g->adv = (float)(abc.abcA + (int)abc.abcB + abc.abcC);
            pen_x += cw + 1;
            if (ch > row_h) row_h = ch;
        }
        SelectObject(dc, oldf);
        DeleteObject(font);
    }
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);

    if (atlas_srv) { ID3D11ShaderResourceView_Release(atlas_srv); atlas_srv = NULL; }
    if (atlas_tex) { ID3D11Texture2D_Release(atlas_tex); atlas_tex = NULL; }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = AW; td.Height = AH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {pix, AW, 0};
    HRESULT hr = ID3D11Device_CreateTexture2D(d3d_dev(), &td, &sd, &atlas_tex);
    free(pix);
    if (FAILED(hr)) return 0;
    ID3D11Device_CreateShaderResourceView(d3d_dev(), (ID3D11Resource *)atlas_tex, NULL, &atlas_srv);
    g_font_scale = scale;
    return atlas_srv != NULL;
}

/* ---------------------------------------------------------------- draw list */
static UVtx *uv_list;
static int uv_n, uv_cap;
static float g_scale, g_ox, g_oy;        /* virtual -> pixels */

static UVtx *push(int n) {
    if (uv_n + n > uv_cap) { uv_cap = (uv_n + n) * 2; uv_list = realloc(uv_list, sizeof(UVtx) * (size_t)uv_cap); }
    UVtx *v = &uv_list[uv_n];
    uv_n += n;
    return v;
}

static uint32_t rgba_to_abgr(uint32_t c) {     /* 0xRRGGBBAA -> bytes R,G,B,A in memory (R8G8B8A8) */
    return ((c >> 24) & 0xFF) | (((c >> 16) & 0xFF) << 8) | (((c >> 8) & 0xFF) << 16) | ((c & 0xFF) << 24);
}

void uip_rect(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom,
              float border, uint32_t border_col, float feather) {
    if (w <= 0 || h <= 0) return;
    float s = g_scale;
    float X = g_ox + x * s, Y = g_oy + y * s, Wp = w * s, Hp = h * s;
    float R = radius * s, B = border * s, F = feather * s;
    if (R > Wp / 2) R = Wp / 2;
    if (R > Hp / 2) R = Hp / 2;
    float grow = F + 1.5f;
    float cx = X + Wp / 2, cy = Y + Hp / 2;
    float xs[2] = {X - grow, X + Wp + grow}, ys[2] = {Y - grow, Y + Hp + grow};
    uint32_t ct = rgba_to_abgr(top), cb = rgba_to_abgr(bottom), bc = rgba_to_abgr(border_col);
    int idx[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
    UVtx *v = push(6);
    for (int k = 0; k < 6; k++) {
        UVtx *q = &v[k];
        q->x = xs[idx[k][0]]; q->y = ys[idx[k][1]];
        q->u = q->v = 0;
        q->col = idx[k][1] ? cb : ct;
        q->lx = q->x - cx; q->ly = q->y - cy;
        q->hx = Wp / 2; q->hy = Hp / 2;
        q->radius = R; q->border = B; q->feather = F; q->mode = 0;
        q->bcol = bc;
    }
}

static void rect_common(float x, float y, float w, float h, float radius, uint32_t c00, uint32_t c10,
                        uint32_t c01, uint32_t c11, float mode, float u0, float v0, float u1, float v1) {
    float s = g_scale;
    float X = g_ox + x * s, Y = g_oy + y * s, Wp = w * s, Hp = h * s, R = radius * s;
    if (R > Wp / 2) R = Wp / 2;
    if (R > Hp / 2) R = Hp / 2;
    float cx = X + Wp / 2, cy = Y + Hp / 2;
    float xs[2] = {X - 1.5f, X + Wp + 1.5f}, ys[2] = {Y - 1.5f, Y + Hp + 1.5f};
    float us[2] = {u0, u1}, vs[2] = {v0, v1};
    uint32_t cols[2][2] = {{rgba_to_abgr(c00), rgba_to_abgr(c01)}, {rgba_to_abgr(c10), rgba_to_abgr(c11)}};
    int idx[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
    UVtx *v = push(6);
    for (int k = 0; k < 6; k++) {
        UVtx *q = &v[k];
        memset(q, 0, sizeof *q);
        q->x = xs[idx[k][0]]; q->y = ys[idx[k][1]];
        q->u = us[idx[k][0]]; q->v = vs[idx[k][1]];
        q->col = cols[idx[k][0]][idx[k][1]];
        q->lx = q->x - cx; q->ly = q->y - cy;
        q->hx = Wp / 2; q->hy = Hp / 2;
        q->radius = R; q->mode = mode;
    }
}

void uip_rect_hgrad(float x, float y, float w, float h, float radius, uint32_t left, uint32_t right) {
    if (w > 0 && h > 0) rect_common(x, y, w, h, radius, left, right, left, right, 0, 0, 0, 0, 0);
}

/* ---- menu art: menu_art.png / menu_art.jpg next to the game, loaded once with WIC ---- */
#include <wincodec.h>
static ID3D11Texture2D *art_tex;
static ID3D11ShaderResourceView *art_srv;
static int art_w, art_h, art_tried;
static const GUID k_CLSID_WICImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID k_IID_IWICImagingFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID k_GUID_WICPixelFormat32bppPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

static void load_art(void) {
    art_tried = 1;
    static const wchar_t *names[] = {L"menu_art.png", L"menu_art.jpg", L"menu_art.jpeg"};
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *wic = NULL;
    if (FAILED(CoCreateInstance(&k_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &k_IID_IWICImagingFactory, (void **)&wic)))
        return;
    for (unsigned i = 0; i < sizeof names / sizeof names[0] && !art_srv; i++) {
        IWICBitmapDecoder *dec = NULL;
        IWICBitmapFrameDecode *frame = NULL;
        IWICFormatConverter *conv = NULL;
        if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(wic, names[i], NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
            continue;
        UINT w = 0, h = 0;
        if (SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
            SUCCEEDED(IWICImagingFactory_CreateFormatConverter(wic, &conv)) &&
            SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &k_GUID_WICPixelFormat32bppPBGRA,
                                                     WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            uint8_t *px = malloc((size_t)w * h * 4);
            if (px && SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, px))) {
                D3D11_TEXTURE2D_DESC td;
                memset(&td, 0, sizeof td);
                td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA sd = {px, w * 4, 0};
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(d3d_dev(), &td, &sd, &art_tex)))
                    ID3D11Device_CreateShaderResourceView(d3d_dev(), (ID3D11Resource *)art_tex, NULL, &art_srv);
                art_w = (int)w; art_h = (int)h;
                if (art_srv) rt_log("ui: menu art %ls (%ux%u)", names[i], w, h);
            }
            free(px);
        }
        if (conv) IWICFormatConverter_Release(conv);
        if (frame) IWICBitmapFrameDecode_Release(frame);
        IWICBitmapDecoder_Release(dec);
    }
    IWICImagingFactory_Release(wic);
}

int uip_art_available(void) {
    if (!art_tried && d3d_dev()) load_art();
    return art_srv != NULL;
}

void uip_art(float x, float y, float w, float h, float radius, float alpha) {
    if (!art_srv || w <= 0 || h <= 0) return;
    /* cover: crop the image to the rectangle's shape, keeping the right-hand side (where the subject is) */
    float ra = w / h, ia = (float)art_w / (float)art_h;
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (ia > ra) { float keep = ra / ia; u0 = 1 - keep; }
    else { float keep = ia / ra; v0 = (1 - keep) / 2; v1 = v0 + keep; }
    uint32_t c = 0xFFFFFF00u | (uint32_t)(alpha * 255.0f + 0.5f);
    rect_common(x, y, w, h, radius, c, c, c, c, 2, u0, v0, u1, v1);
}

static int utf8_next(const char **p) {
    /* never steps past the terminating zero, even in a sequence cut short (a name truncated mid-character) */
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s++, more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= more == 3 ? 7 : more == 2 ? 15 : 31;
    for (; more > 0; more--) {
        if ((*s & 0xC0) != 0x80) { c = '?'; break; }
        c = (c << 6) | (*s++ & 63);
    }
    *p = (const char *)s;
    return c;
}

float uip_text_w(int font, const char *s) {
    Font *f = &g_fonts[font];
    float w = 0;
    while (*s) w += glyph_for(f, utf8_next(&s))->adv;
    return w / g_font_scale;
}

float uip_line_h(int font) { return g_fonts[font].line_h / g_font_scale; }

void uip_text(float x, float y, int font, uint32_t col, const char *s) {
    Font *f = &g_fonts[font];
    float px = floorf(g_ox + x * g_scale + 0.5f), py = floorf(g_oy + y * g_scale + 0.5f);
    uint32_t c = rgba_to_abgr(col);
    while (*s) {
        Glyph *g = glyph_for(f, utf8_next(&s));
        if (g->w > 0) {
            float x0 = px + g->xoff, y0 = py, x1 = x0 + g->w, y1 = y0 + g->h;
            float xs[2] = {x0, x1}, ys[2] = {y0, y1}, us[2] = {g->u0, g->u1}, vs[2] = {g->v0, g->v1};
            int idx[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
            UVtx *v = push(6);
            for (int k = 0; k < 6; k++) {
                memset(&v[k], 0, sizeof v[k]);
                v[k].x = xs[idx[k][0]]; v[k].y = ys[idx[k][1]];
                v[k].u = us[idx[k][0]]; v[k].v = vs[idx[k][1]];
                v[k].col = c; v[k].mode = 1;
            }
        }
        px += g->adv;
    }
}

/* ---------------------------------------------------------------- GPU objects */
static ID3D11VertexShader *uvs;
static ID3D11PixelShader *ups;
static ID3D11InputLayout *ulayout;
static ID3D11Buffer *ubuf, *ucb;
static int ubuf_cap;
static ID3D11BlendState *ublend;
static ID3D11SamplerState *usamp;
static int g_ui_failed;
int ui_d3d_failed(void) { return g_ui_failed; }

static const char k_ui_shader[] =
    "cbuffer C : register(b0) { float2 screen; float2 pad; };\n"
    "Texture2D<float> atlas : register(t0); SamplerState s0 : register(s0); Texture2D art : register(t1);\n"
    "struct VI { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float2 lp : TEXCOORD1;\n"
    "            float2 hs : TEXCOORD2; float4 prm : TEXCOORD3; float4 bcol : COLOR1; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; float2 lp : TEXCOORD1;\n"
    "            nointerpolation float2 hs : TEXCOORD2; nointerpolation float4 prm : TEXCOORD3; nointerpolation float4 bcol : COLOR1; };\n"
    "VO vs(VI i) { VO o; o.pos = float4(i.pos.x / screen.x * 2 - 1, 1 - i.pos.y / screen.y * 2, 0, 1);\n"
    "  o.uv = i.uv; o.col = i.col; o.lp = i.lp; o.hs = i.hs; o.prm = i.prm; o.bcol = i.bcol; return o; }\n"
    "float sdbox(float2 p, float2 b, float r) { float2 q = abs(p) - b + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r; }\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  if (i.prm.w > 0.5 && i.prm.w < 1.5) { float a = atlas.Sample(s0, i.uv) * i.col.a; return float4(i.col.rgb * a, a); }\n"
    "  if (i.prm.w > 1.5) { float d2 = sdbox(i.lp, i.hs, i.prm.x); float cov = 1 - smoothstep(-0.7, 0.7, d2);\n"
    "    float4 t = art.Sample(s0, i.uv); float a = cov * i.col.a; return float4(t.rgb * a, a); }\n"
    "  float d = sdbox(i.lp, i.hs, i.prm.x);\n"
    "  float aa = max(i.prm.z, 0.7);\n"
    "  float cover = 1 - smoothstep(-aa, aa, d);\n"
    "  float4 c = i.col;\n"
    "  if (i.prm.y > 0) {\n"
    "    float bd = abs(d + i.prm.y * 0.5) - i.prm.y * 0.5;\n"
    "    float bm = (1 - smoothstep(-0.7, 0.7, bd)) * i.bcol.a;\n"
    "    c.rgb = lerp(c.rgb * c.a, i.bcol.rgb, bm); c.a = bm + c.a * (1 - bm);\n"
    "    if (c.a > 0) c.rgb /= c.a;\n"
    "  }\n"
    "  float a = c.a * cover; return float4(c.rgb * a, a); }\n";

static int ui_init_gpu(void) {
    ID3D11Device *dev = d3d_dev();
    D3DCompileFn compile = d3d_compiler();
    if (!dev || !compile) return 0;
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(compile(k_ui_shader, sizeof k_ui_shader - 1, "ui", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(compile(k_ui_shader, sizeof k_ui_shader - 1, "ui", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err))) {
        if (err) { rt_log("ui: shader: %s", (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
        if (vb) ID3D10Blob_Release(vb);
        return 0;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &uvs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ups);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, el, 7, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), &ulayout);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &ucb);
    D3D11_BLEND_DESC b;
    memset(&b, 0, sizeof b);
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &b, &ublend);
    D3D11_SAMPLER_DESC sm;
    memset(&sm, 0, sizeof sm);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sm, &usamp);
    return uvs && ups && ulayout && ucb && ublend && usamp;
}

void ui_d3d_release(void) {
#define REL(p, T) do { if (p) { T##_Release(p); p = NULL; } } while (0)
    REL(uvs, ID3D11VertexShader); REL(ups, ID3D11PixelShader); REL(ulayout, ID3D11InputLayout);
    REL(ubuf, ID3D11Buffer); REL(ucb, ID3D11Buffer); REL(ublend, ID3D11BlendState); REL(usamp, ID3D11SamplerState);
    REL(atlas_srv, ID3D11ShaderResourceView); REL(atlas_tex, ID3D11Texture2D);
    REL(art_srv, ID3D11ShaderResourceView); REL(art_tex, ID3D11Texture2D);
    art_tried = 0;
#undef REL
    ubuf_cap = 0;
    g_font_scale = 0;
}

extern float g_ui_fps;

/* Draw the overlay UI on top of the current render target (the window's back buffer). */
void ui_d3d_render(int w, int h) {
    if (g_ui_failed || w <= 0 || h <= 0) return;
    if (!uvs && !ui_init_gpu()) { g_ui_failed = 1; rt_log("ui: overlay unavailable"); return; }
    float scale = (float)h / 1080.0f;
    if (fabsf(scale - g_font_scale) > 0.001f && !build_atlas(scale)) { g_ui_failed = 1; return; }
    g_scale = scale; g_ox = 0; g_oy = 0;
    uv_n = 0;
    ui_draw((float)w / scale, 1080.0f, g_ui_fps);
    if (!uv_n) return;
    ID3D11DeviceContext *ctx = d3d_ctx();
    if (uv_n > ubuf_cap) {
        if (ubuf) ID3D11Buffer_Release(ubuf);
        ubuf_cap = uv_n * 2;
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)(sizeof(UVtx) * (size_t)ubuf_cap); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d_dev(), &bd, NULL, &ubuf))) { ubuf = NULL; ubuf_cap = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ubuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, uv_list, sizeof(UVtx) * (size_t)uv_n);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ubuf, 0);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ucb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *f = (float *)m.pData;
        f[0] = (float)w; f[1] = (float)h; f[2] = f[3] = 0;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ucb, 0);
    }
    D3D11_VIEWPORT vp = {0, 0, (FLOAT)w, (FLOAT)h, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    UINT stride = sizeof(UVtx), offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &ubuf, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, ulayout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, uvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ups, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &ucb);
    ID3D11ShaderResourceView *srvs[2] = {atlas_srv, art_srv};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &usamp);
    ID3D11DeviceContext_OMSetBlendState(ctx, ublend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(ctx, (UINT)uv_n, 0);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
}
