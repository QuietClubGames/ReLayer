#pragma once
/* Shared between the Direct3D 11 presenter (video_d3d11.c) and the high-resolution renderer (gpu_hd_d3d11.c). */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include "../core/gpu_hd.h"

typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                       ID3DBlob **, ID3DBlob **);

ID3D11Device        *d3d_dev(void);
ID3D11DeviceContext *d3d_ctx(void);
D3DCompileFn         d3d_compiler(void);
int                  d3d_picture_height(void);      /* height of the 4:3 picture area in the window */
int                  d3d_blit_and_present(ID3D11ShaderResourceView *picture, int linear);

/* gpu_hd_d3d11.c */
ID3D11ShaderResourceView *hd_d3d_render(const HdFrame *f);   /* NULL if it can't */
void hd_d3d_release(void);
