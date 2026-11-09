/*
 * Direct3D 11 presenter: uploads the emulated screen to a texture and draws it with a flip-model swap
 * chain. Present() with a sync interval is real vsync (no GDI stalls), and the swap chain keeps one finished
 * frame queued so a single slow frame does not show as a hitch.
 * Falls back to GDI (video_win32.c) if anything here fails.
 */
#define COBJMACROS
#define CINTERFACE
#include <initguid.h>
#include <windows.h>
#include <stdlib.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string.h>
#include "d3d11_present.h"
#include "d3d11_internal.h"
#include "../core/log.h"

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11SamplerState *samp, *samp_linear;
static D3DCompileFn g_compile;
static ID3D11Buffer *blit_cb;
float g_blit_dim, g_blit_blur, g_blit_scan;      /* set by the platform layer each frame */
int g_blit_smooth;
static const char *g_save_backbuffer;            /* save the next presented frame here (tests) */
void ui_d3d_render(int w, int h);
void ui_d3d_release(void);
void d3d_request_backbuffer_save(const char *path) { g_save_backbuffer = path; }
static int tex_w, tex_h, buf_w, buf_h;
static int g_warp;             /* the software device: no high-resolution renderer (far too slow there) */
static int g_need_resize;      /* entering or leaving exclusive full screen: the buffers must be resized before the next Present */
static HWND wnd;
static UINT sync_interval;

/* Final picture: optional backdrop (darken + blur + desaturate behind the menu) and scanline filter. */
static const char k_shader[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
    "cbuffer B : register(b0) { float dim; float blur; float scan; float lines; float2 texel; float2 pad; };\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float3 c = t0.Sample(s0, i.uv).rgb;\n"
    "  if (blur > 0) { float3 acc = 0; [unroll] for (int y = -3; y <= 3; y++) [unroll] for (int x = -3; x <= 3; x++)\n"
    "      acc += t0.Sample(s1, i.uv + float2(x, y) * texel * 2.5).rgb;\n"
    "    c = lerp(c, acc / 49.0, blur); }\n"
    "  if (scan > 0) { float k = abs(sin(i.uv.y * lines * 3.14159265)); c *= lerp(1.0, 0.72 + 0.36 * k, scan); }\n"
    "  float g = dot(c, float3(0.299, 0.587, 0.114));\n"
    "  c = lerp(c, g.xxx, dim * 0.5); c *= 1 - dim * 0.55;\n"
    "  return float4(c, 1); }\n";

static void release_all(void) {
    /* unbind everything and flush, or the swap chain's destruction is deferred and a new one for the same
     * window is refused (E_ACCESSDENIED) */
    if (ctx) { ID3D11DeviceContext_ClearState(ctx); ID3D11DeviceContext_Flush(ctx); }
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (tex) ID3D11Texture2D_Release(tex);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (samp) ID3D11SamplerState_Release(samp);
    if (samp_linear) ID3D11SamplerState_Release(samp_linear);
    samp_linear = NULL;
    hd_d3d_release();
    ui_d3d_release();
    if (blit_cb) { ID3D11Buffer_Release(blit_cb); blit_cb = NULL; }
    if (ps) ID3D11PixelShader_Release(ps);
    if (vs) ID3D11VertexShader_Release(vs);
    if (swap) { IDXGISwapChain1_SetFullscreenState(swap, FALSE, NULL); IDXGISwapChain1_Release(swap); }   /* never release it full screen */
    if (ctx) { ID3D11DeviceContext_ClearState(ctx); ID3D11DeviceContext_Flush(ctx); }   /* destroys it now */
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    srv = NULL; tex = NULL; rtv = NULL; samp = NULL; ps = NULL; vs = NULL; swap = NULL; ctx = NULL; dev = NULL;
}

/* Give up on Direct3D (the caller draws with GDI from now on, which only shows once the swap chain is gone). */
void d3d_shutdown(void) {
    if (swap) IDXGISwapChain1_SetFullscreenState(swap, FALSE, NULL);
    release_all();
}

static int make_rtv(void) {
    ID3D11Texture2D *back = NULL;
    if (FAILED(IDXGISwapChain1_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return 0;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    ID3D11Texture2D_Release(back);
    return SUCCEEDED(hr);
}

/* (Re)create the swap chain for the window at its current size. */
static int create_swapchain(IDXGIFactory2 *factory) {
    RECT cr;
    GetClientRect(wnd, &cr);
    buf_w = cr.right > 0 ? cr.right : 640;
    buf_h = cr.bottom > 0 ? cr.bottom : 480;
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)buf_w;
    sd.Height = (UINT)buf_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;           /* Windows 10+ */
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)dev, wnd, &sd, NULL, NULL, &swap);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;    /* Windows 8 */
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)dev, wnd, &sd, NULL, NULL, &swap);
    }
    IDXGIFactory2_MakeWindowAssociation(factory, wnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr) || !make_rtv()) { rt_log("d3d11: swap chain failed (%08lX)", (unsigned long)hr); return 0; }
    rt_log("d3d11: %s swap chain %dx%d", sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip-discard" : "flip-sequential",
           buf_w, buf_h);
    return 1;
}

/* Last resort when resizing fails: throw the swap chain away and make a new one on the same device. */
static int recreate_swapchain(void) {
    IDXGIDevice1 *dxdev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    int ok = 0;
    ID3D11DeviceContext_ClearState(ctx);
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
    if (swap) { IDXGISwapChain1_SetFullscreenState(swap, FALSE, NULL); IDXGISwapChain1_Release(swap); swap = NULL; }
    ID3D11DeviceContext_Flush(ctx);
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&dxdev)) &&
        SUCCEEDED(IDXGIDevice1_GetAdapter(dxdev, &adapter)) &&
        SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory)))
        ok = create_swapchain(factory);
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxdev) IDXGIDevice1_Release(dxdev);
    return ok;
}

static int compile(D3DCompileFn fn, const char *entry, const char *target, ID3DBlob **out) {
    ID3DBlob *err = NULL;
    HRESULT hr = fn(k_shader, sizeof k_shader - 1, "present", NULL, NULL, entry, target, 0, 0, out, &err);
    if (err) { rt_log("d3d11: shader %s: %s", entry, (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
    return SUCCEEDED(hr);
}

/* ---- graphics cards ---- */
#define MAX_GPUS 8
static char g_gpu_names[MAX_GPUS][400];       /* adapter descriptions are up to 128 UTF-16 characters */
static int g_ngpus, g_gpu_listed;
static IDXGIAdapter1 *g_gpus[MAX_GPUS];
static SIZE_T g_gpu_vram[MAX_GPUS];

static void list_gpus(void) {
    if (g_gpu_listed) return;
    g_gpu_listed = 1;
    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return;
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; g_ngpus < MAX_GPUS && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g_gpu_names[g_ngpus], sizeof g_gpu_names[0], NULL, NULL);
            g_gpu_vram[g_ngpus] = d.DedicatedVideoMemory;
            g_gpus[g_ngpus++] = a;
        } else {
            IDXGIAdapter1_Release(a);
        }
    }
    IDXGIFactory1_Release(f);
}
int d3d_gpu_count(void) { list_gpus(); return g_ngpus; }
const char *d3d_gpu_name(int i) { list_gpus(); return i >= 0 && i < g_ngpus ? g_gpu_names[i] : "?"; }

int d3d_init(HWND hwnd, int refresh_hz, int gpu) {
    wnd = hwnd;
    /* which graphics card: the chosen one, or automatically the one with the most video memory (on laptops
     * that is the dedicated GPU rather than the integrated one) */
    list_gpus();
    IDXGIAdapter *chosen = NULL;
    if (gpu >= 1 && gpu <= g_ngpus) chosen = (IDXGIAdapter *)g_gpus[gpu - 1];
    else if (g_ngpus > 0) {
        int best = 0;
        for (int i = 1; i < g_ngpus; i++) if (g_gpu_vram[i] > g_gpu_vram[best]) best = i;
        chosen = (IDXGIAdapter *)g_gpus[best];
    }
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompileFn d3dcompile = comp ? (D3DCompileFn)(void *)GetProcAddress(comp, "D3DCompile") : NULL;
    g_compile = d3dcompile;
    if (!d3dcompile) { rt_log("d3d11: d3dcompiler_47.dll not found"); return 0; }

    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    g_warp = 0;
    if (getenv("FGT_FORCE_WARP") ||
        (FAILED(D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3,
                                  D3D11_SDK_VERSION, &dev, NULL, &ctx)) &&
         FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3, D3D11_SDK_VERSION, &dev, NULL, &ctx)))) {
        /* no usable graphics card (old driver, virtual machine...): Windows' own software Direct3D still shows
         * the game and the menu, just without the high-resolution renderer */
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, want, 3, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
            rt_log("d3d11: no hardware device, and no software (WARP) device either");
            return 0;
        }
        g_warp = 1;
        rt_log("d3d11: no usable graphics card; using Windows' software renderer (WARP), original graphics only");
    }
    IDXGIDevice1 *dxdev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&dxdev)) ||
        FAILED(IDXGIDevice1_GetAdapter(dxdev, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory))) {
        rt_log("d3d11: DXGI 1.2 not available");
        if (adapter) IDXGIAdapter_Release(adapter);
        if (dxdev) IDXGIDevice1_Release(dxdev);
        release_all();
        return 0;
    }
    {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad))) {
            char name[400];
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
            rt_log("d3d11: graphics adapter: %s", name);
        }
    }
    /* one frame may wait in the queue: absorbs an occasional slow frame, costs at most one frame of lag */
    IDXGIDevice1_SetMaximumFrameLatency(dxdev, 2);

    int made = create_swapchain(factory);
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice1_Release(dxdev);
    if (!made) { release_all(); return 0; }

    ID3DBlob *vb = NULL, *pb = NULL;
    if (!compile(d3dcompile, "vs", "vs_4_0", &vb) || !compile(d3dcompile, "ps", "ps_4_0", &pb)) {
        if (vb) ID3D10Blob_Release(vb);
        release_all();
        return 0;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);

    D3D11_SAMPLER_DESC smp;
    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;               /* sharp pixels, like the GDI path */
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &smp, &samp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;              /* for the high-resolution image (downscaling) */
    ID3D11Device_CreateSamplerState(dev, &smp, &samp_linear);
    if (!vs || !ps || !samp) { release_all(); return 0; }

    /* Vsync: one display refresh per game frame on 60 Hz screens, two on 120 Hz, and so on. Otherwise
     * (e.g. 144 Hz) present without waiting and let the caller's 60 Hz timer set the pace. */
    sync_interval = 0;
    for (UINT k = 1; k <= 4; k++)
        if (refresh_hz >= 59 * (int)k && refresh_hz <= 61 * (int)k) sync_interval = k;
    rt_log("d3d11: presenting with %s", sync_interval ? "vsync" : "timer pacing (display is not a multiple of 60 Hz)");
    return 1;
}

int d3d_vsync(void) { return swap && sync_interval; }

/* Exclusive full screen: the swap chain takes over the monitor (falls back to borderless if refused). */
int d3d_set_exclusive(int on, HMONITOR monitor) {
    if (!swap) return 0;
    if (!on) { IDXGISwapChain1_SetFullscreenState(swap, FALSE, NULL); g_need_resize = 1; return 1; }
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIOutput *out = NULL, *match = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx)) && SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad))) {
        for (UINT i = 0; IDXGIAdapter_EnumOutputs(ad, i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC d;
            if (!match && SUCCEEDED(IDXGIOutput_GetDesc(out, &d)) && d.Monitor == monitor) { match = out; continue; }
            IDXGIOutput_Release(out);
        }
    }
    HRESULT hr = IDXGISwapChain1_SetFullscreenState(swap, TRUE, match);
    if (match) IDXGIOutput_Release(match);
    if (ad) IDXGIAdapter_Release(ad);
    if (dx) IDXGIDevice_Release(dx);
    if (FAILED(hr)) { rt_log("d3d11: exclusive full screen refused (%08lX)", (unsigned long)hr); return 0; }
    g_need_resize = 1;             /* DXGI requires ResizeBuffers after a full-screen change, or Present fails */
    return 1;
}

/* Exact count of display refreshes that showed a repeated frame (from DXGI frame statistics). */
static int g_repeats, g_repeats_total;
static UINT g_last_present_count, g_last_refresh;
static void track_repeats(void) {
    DXGI_FRAME_STATISTICS st;
    if (FAILED(IDXGISwapChain1_GetFrameStatistics(swap, &st))) return;
    if (g_last_refresh && st.PresentRefreshCount > g_last_refresh) {
        UINT refreshes = st.PresentRefreshCount - g_last_refresh;
        UINT frames = st.PresentCount - g_last_present_count;
        if (refreshes > frames * (sync_interval ? sync_interval : 1))
        {
            int r = (int)(refreshes - frames * (sync_interval ? sync_interval : 1));
            g_repeats += r;
            g_repeats_total += r;
        }
    }
    g_last_refresh = st.PresentRefreshCount;
    g_last_present_count = st.PresentCount;
}
int d3d_take_repeats(void) { int r = g_repeats; g_repeats = 0; return r; }
int d3d_repeats_total(void) { return swap && sync_interval ? g_repeats_total : -1; }

static int ensure_texture(int w, int h) {
    if (tex && tex_w == w && tex_h == h) return 1;
    if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
    if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) return 0;
    tex_w = w;
    tex_h = h;
    return 1;
}

/* ---- shared with the high-resolution renderer (d3d11_internal.h) ---- */
ID3D11Device *d3d_dev(void) { return dev; }
ID3D11DeviceContext *d3d_ctx(void) { return ctx; }
D3DCompileFn d3d_compiler(void) { return g_compile; }

int d3d_picture_height(void) {
    int px, py, pw, ph;
    picture_rect(buf_w, buf_h, &px, &py, &pw, &ph);
    return ph;
}

/* follow window resizes */
static int follow_resize(void) {
    RECT cr;
    GetClientRect(wnd, &cr);
    if (cr.right > 0 && cr.bottom > 0 && (cr.right != buf_w || cr.bottom != buf_h || g_need_resize)) {
        g_need_resize = 0;
        /* ResizeBuffers fails while anything still refers to the old buffers, bound views included */
        ID3D11DeviceContext_ClearState(ctx);
        if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
        ID3D11DeviceContext_Flush(ctx);
        HRESULT hr = IDXGISwapChain1_ResizeBuffers(swap, 0, (UINT)cr.right, (UINT)cr.bottom, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr) && make_rtv()) {
            buf_w = cr.right;
            buf_h = cr.bottom;
            rt_log("d3d11: swap chain resized to %dx%d", buf_w, buf_h);
            return 1;
        }
        rt_log("d3d11: resizing the swap chain to %ldx%ld failed (%08lX); making a new one", cr.right, cr.bottom,
               (unsigned long)hr);
        if (recreate_swapchain()) return 1;
        /* the old picture would stay frozen on screen behind any other way of drawing: drop Direct3D entirely
         * so the window falls back to plain GDI drawing */
        rt_log("d3d11: no swap chain; switching to GDI");
        release_all();
        return 0;
    }
    return 1;
}

static void save_backbuffer(const char *path);

/* Draw a picture into the 4:3 area of the window (black bars around it), the overlay UI on top, and
 * present with vsync. */
int d3d_blit_and_present(ID3D11ShaderResourceView *picture, int linear) {
    int px, py, pw, ph;
    if (!blit_cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &blit_cb);
    }
    {
        ID3D11Resource *res = NULL;
        D3D11_TEXTURE2D_DESC td = {0};
        ID3D11ShaderResourceView_GetResource(picture, &res);
        if (res) { ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td); ID3D11Resource_Release(res); }
        D3D11_MAPPED_SUBRESOURCE m;
        if (blit_cb && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float *f = (float *)m.pData;
            f[0] = g_blit_dim; f[1] = g_blit_blur; f[2] = g_blit_scan; f[3] = 240.0f;
            f[4] = td.Width ? 1.0f / (float)td.Width : 0; f[5] = td.Height ? 1.0f / (float)td.Height : 0;
            f[6] = f[7] = 0;
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)blit_cb, 0);
        }
    }
    picture_rect(buf_w, buf_h, &px, &py, &pw, &ph);
    D3D11_VIEWPORT vp = {(FLOAT)px, (FLOAT)py, (FLOAT)pw, (FLOAT)ph, 0, 1};
    static const FLOAT black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &picture);
    ID3D11SamplerState *smps[2] = {(linear || g_blit_smooth) ? samp_linear : samp, samp_linear};
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, smps);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &blit_cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ui_d3d_render(buf_w, buf_h);                    /* menu, toasts, frame-rate counter */
    if (g_save_backbuffer) { save_backbuffer(g_save_backbuffer); g_save_backbuffer = NULL; }
    HRESULT hr = IDXGISwapChain1_Present(swap, sync_interval, 0);
    if (SUCCEEDED(hr)) track_repeats();
    if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
        /* the swap chain is in a bad state (e.g. after a full-screen change): make a new one and carry on */
        static int retries;
        if (retries < 20 && recreate_swapchain()) {
            retries++;
            rt_log("d3d11: present failed (%08lX); made a new swap chain", (unsigned long)hr);
            return 1;
        }
    }
    if (FAILED(hr)) {
        rt_log("d3d11: present failed (%08lX), switching to GDI", (unsigned long)hr);
        release_all();
        return 0;
    }
    return 1;
}

int d3d_present(const uint32_t *rgb, int w, int h) {
    if (!swap || !follow_resize()) return 0;
    if (!ensure_texture(w, h)) return 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    for (int y = 0; y < h; y++) memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, rgb + (size_t)y * w, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex, 0);
    return d3d_blit_and_present(srv, 0);
}

/* High-resolution path: redraw the recorded frame on the GPU, then present it. */
int d3d_present_hd(const HdFrame *f) {
    if (!swap || !follow_resize()) return 0;
    if (g_warp) return -1;
    ID3D11ShaderResourceView *pic = hd_d3d_render(f);
    if (!pic) return -1;           /* couldn't: caller uses the software picture */
    return d3d_blit_and_present(pic, 1);
}

/* Save the back buffer (what is about to be shown) as a 24-bit BMP. */
#include <stdio.h>
#include <stdlib.h>
static void save_backbuffer(const char *path) {
    ID3D11Texture2D *back = NULL, *st = NULL;
    if (FAILED(IDXGISwapChain1_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE *f = fopen(path, "wb");
            if (f) {
                int w = (int)td.Width, h = (int)td.Height, row = (w * 3 + 3) & ~3;
                uint32_t data = (uint32_t)(row * h), file = 54 + data;
                uint8_t hdr[54] = {'B', 'M'};
                memcpy(hdr + 2, &file, 4); hdr[10] = 54; hdr[14] = 40;
                memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &data, 4);
                fwrite(hdr, 1, 54, f);
                uint8_t *line = calloc(1, (size_t)row);
                for (int y = h - 1; y >= 0; y--) {
                    const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                    for (int x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
                    fwrite(line, 1, (size_t)row, f);
                }
                free(line);
                fclose(f);
                rt_log("saved screen to %s", path);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(back);
}
