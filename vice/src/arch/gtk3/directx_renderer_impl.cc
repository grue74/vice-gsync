/**
 * \file directx_renderer_impl.cc
 * \brief   DirectX-based renderer for the GTK3 backend.
 *
 * \author David Hogan <david.q.hogan@gmail.com>
 */

/* This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include "directx_renderer_impl.h"

#ifdef WINDOWS_COMPILE

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <dxgi1_5.h>
#include <d3dcompiler.h>

extern "C"
{
#include "archdep.h"
#include "log.h"
#include "render_queue.h"
#include "resources.h"
#include "videoarch.h"
#include "vsync.h"
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define CANVAS_LOCK() pthread_mutex_lock(context->canvas_lock_ptr)
#define CANVAS_UNLOCK() pthread_mutex_unlock(context->canvas_lock_ptr)
#define RENDER_LOCK() pthread_mutex_lock(&context->render_lock)
#define RENDER_UNLOCK() pthread_mutex_unlock(&context->render_lock)

#define DX_RELEASE(x) if (x) { (x)->Release(); (x) = NULL; }

/** \brief How often to retry creating the swap chain after it failed (ms) */
#define SWAP_CHAIN_RETRY_MS         500

/** \brief Frames this much later than planned restart the pacing (pause, menus, ...) */
#define PACING_RESET_MS             100.0

/** \brief Frames per block of the latency history; unneeded latency is trimmed after each (about 5 s) */
#define PACING_BLOCK_FRAMES         250

/** \brief Blocks in the latency history: the latency kept covers the worst frame of the last 30 s */
#define PACING_BLOCKS               PACING_HISTORY_BLOCKS

/** \brief Safety margin kept between the latest frame seen and its present */
#define PACING_MARGIN_MS            2.0

/** \brief Frames after a (re)start during which late frames just move the clock (1 s) */
#define PACING_WARMUP_FRAMES        50

/** \brief Latency kept at the start, before anything is known */
#define PACING_START_MS             6.0

/** \brief Most the clock moves earlier per frame when trimming latency */
#define PACING_SLEW_EARLIER_MS      0.05

/** \brief Most the clock moves later per frame after a late frame */
#define PACING_SLEW_LATER_MS        0.5

/** \brief Emulated frame rate this far from the nominal one is a speed change, not drift */
#define PACING_SPEED_CHANGE         0.01

/**
 * \brief Show each frame twice when the display is at least this much faster
 *
 * VRR displays have a minimum refresh rate (often 48 Hz). Near it (PAL is
 * 50.125 Hz) the driver inserts extra refreshes on its own, and a new frame
 * that arrives during one is delayed by up to a full scan. Presenting every
 * frame twice, evenly spaced, keeps the display at ~100 Hz, well inside the
 * VRR range. The picture still changes exactly once per emulated frame.
 * Only done when the display can refresh fast enough for the extra present.
 */
#define PACING_DOUBLE_MIN_RATIO     2.2

/** \brief Paced frames between checks of the display refresh rate */
#define PACING_DISPLAY_CHECK_FRAMES 100

/*
 * CRT beam simulation (rolling scan)
 *
 * A CRT lights each part of the picture only briefly, as the beam passes. A
 * modern display holds every frame for the whole frame time, which makes
 * moving images (scrolling) look smeared and less steady to the eye. With a
 * fast display (and VRR), each emulated frame is shown as several subframes:
 *
 *  - the light each pixel should emit during the whole frame is worked out in
 *    linear light, in units of "one subframe at full brightness"
 *  - every pixel emits that light as one pulse, starting when the simulated
 *    beam passes its row (the beam moves top to bottom over the frame)
 *  - each subframe shows how much of that pulse falls into it
 *
 * Dark pixels flash for a short time, bright ones stay lit longer because a
 * subframe can't be brighter than full white. The light over a whole frame is
 * the same as the original picture, so it doesn't get darker like black frame
 * insertion does, and the flicker is spread out as a rolling band instead of
 * the whole screen flashing at once.
 *
 * Based on the idea of the Blur Busters CRT beam simulator; this is an
 * independent, simplified implementation.
 */

/** \brief Keep each subframe at least this much longer than a display refresh */
#define CRT_REFRESH_MARGIN          1.03

/** \brief Most subframes per frame */
#define CRT_MAX_SUBFRAMES           8

/** \brief Shader parameters, layout must match the cbuffer in crt_shader_source */
typedef struct crt_constants_s {
    float rect[4];      /**< emulated picture in the back buffer: left, top, right, bottom (pixels) */
    float subframe;     /**< current subframe, 0 .. subframes - 1 */
    float subframes;    /**< subframes per frame */
    float gamma;        /**< display gamma */
    float persistence;  /**< shortest time a pixel is held lit, in subframes (0 .. subframes) */
    float decay;        /**< phosphor fade time constant, in subframes (> 0) */
    float pad[3];
} crt_constants_t;

static const char crt_shader_source[] =
    "cbuffer params : register(b0)\n"
    "{\n"
    "    float4 rect;\n"
    "    float  subframe;\n"
    "    float  subframes;\n"
    "    float  gamma;\n"
    "    float  persistence;\n"
    "    float  decay;\n"
    "    float3 pad;\n"
    "};\n"
    "Texture2D<float4> source : register(t0);\n"
    "\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    /* one triangle covering the whole target */\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "\n"
    "/* light emitted from the beam pass until time t (in subframes) by a pulse of\n"
    "   brightness a, held for h subframes, then fading with time constant decay */\n"
    "float3 emitted(float t, float3 a, float3 h)\n"
    "{\n"
    "    return a * (min(t, h) + decay * (1.0 - exp(-max(t - h, 0.0) / decay)));\n"
    "}\n"
    "\n"
    "float4 ps_main(float4 pos : SV_Position) : SV_Target\n"
    "{\n"
    "    float4 c = source.Load(int3(pos.xy, 0));\n"
    "\n"
    "    /* outside the emulated picture (window background): unchanged */\n"
    "    if (pos.x < rect.x || pos.x >= rect.z || pos.y < rect.y || pos.y >= rect.w) {\n"
    "        return c;\n"
    "    }\n"
    "\n"
    "    /* when the beam passes this row, in subframes from the frame start */\n"
    "    float beam = saturate((pos.y - rect.y) / (rect.w - rect.y)) * subframes;\n"
    "\n"
    "    /* light for the whole frame, in subframes at full brightness (0 .. subframes) */\n"
    "    float3 energy = pow(saturate(c.rgb), gamma) * subframes;\n"
    "\n"
    "    /* start of this subframe relative to the beam pass, wrapped into 0 .. subframes */\n"
    "    float start = subframe - beam;\n"
    "    start -= subframes * floor(start / subframes);\n"
    "\n"
    "    /* the pulse: held for at least 'persistence' subframes, then a phosphor\n"
    "       like fade until the beam comes back. Dim pixels: lower brightness.\n"
    "       Bright pixels: full brightness, held longer (never above full white).\n"
    "       Either way the light over the frame equals the original picture. */\n"
    "    float3 h = persistence;\n"
    "    float3 full = emitted(subframes, 1.0, h);\n"
    "    float3 a = saturate(energy / max(full, 1e-6));\n"
    "\n"
    "    float3 lo = h;\n"
    "    float3 hi = subframes;\n"
    "    [unroll] for (int i = 0; i < 14; i++) {\n"
    "        float3 mid = (lo + hi) * 0.5;\n"
    "        float3 more = step(emitted(subframes, 1.0, mid), energy);\n"
    "        lo = lerp(lo, mid, more);\n"
    "        hi = lerp(mid, hi, more);\n"
    "    }\n"
    "    h = lerp(h, (lo + hi) * 0.5, step(full, energy));\n"
    "\n"
    "    /* how much of the pulse falls into [start, start + 1),\n"
    "       including the part that wraps around the end of the frame */\n"
    "    float3 lit = emitted(min(start + 1.0, subframes), a, h) - emitted(start, a, h)\n"
    "               + emitted(max(start + 1.0 - subframes, 0.0), a, h);\n"
    "\n"
    "    return float4(pow(saturate(lit), 1.0 / gamma), 1.0);\n"
    "}\n";

/** \brief D3DCompile() from d3dcompiler_47.dll (part of Windows 10 and later) */
typedef HRESULT (WINAPI *crt_d3dcompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                           ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                           ID3DBlob **, ID3DBlob **);

/** \brief Compile one entry point of crt_shader_source */
static ID3DBlob *crt_compile(crt_d3dcompile_t compile, const char *entry, const char *target)
{
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    HRESULT result;

    result = compile(crt_shader_source, sizeof(crt_shader_source) - 1, "crt_beam_sim",
                     NULL, NULL, entry, target, 0, 0, &code, &errors);
    if (FAILED(result)) {
        log_error(LOG_DEFAULT, "DirectX: CRT beam simulation shader %s failed to compile: %s",
                  entry, errors ? (const char *)errors->GetBufferPointer() : "unknown error");
        DX_RELEASE(code);
    }
    DX_RELEASE(errors);

    return code;
}

/** \brief Create the shaders and constant buffer (device dependent, once) */
static bool crt_create_shaders(vice_directx_renderer_context_t *context)
{
    static HMODULE compiler_dll;
    crt_d3dcompile_t compile;
    ID3DBlob *vs_code;
    ID3DBlob *ps_code;
    D3D11_BUFFER_DESC buffer_desc;
    HRESULT result;

    if (context->crt_pixel_shader) {
        return true;
    }
    if (context->crt_failed) {
        return false;
    }
    context->crt_failed = true; /* until it all worked */

    if (!compiler_dll) {
        compiler_dll = LoadLibraryW(L"d3dcompiler_47.dll");
    }
    compile = compiler_dll ? (crt_d3dcompile_t)(void *)GetProcAddress(compiler_dll, "D3DCompile") : NULL;
    if (!compile) {
        log_error(LOG_DEFAULT, "DirectX: CRT beam simulation unavailable, d3dcompiler_47.dll not found");
        return false;
    }

    vs_code = crt_compile(compile, "vs_main", "vs_4_0");
    ps_code = crt_compile(compile, "ps_main", "ps_4_0");
    if (vs_code && ps_code) {
        result = context->d3d_device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                                                         NULL, &context->crt_vertex_shader);
        if (SUCCEEDED(result)) {
            result = context->d3d_device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(),
                                                            NULL, &context->crt_pixel_shader);
        }
        if (FAILED(result)) {
            vice_directx_impl_log_windows_error("CRT beam simulation shaders");
        }
    }
    DX_RELEASE(vs_code);
    DX_RELEASE(ps_code);

    if (!context->crt_pixel_shader || !context->crt_vertex_shader) {
        DX_RELEASE(context->crt_vertex_shader);
        DX_RELEASE(context->crt_pixel_shader);
        return false;
    }

    ZeroMemory(&buffer_desc, sizeof(buffer_desc));
    buffer_desc.ByteWidth      = sizeof(crt_constants_t);
    buffer_desc.Usage          = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = context->d3d_device->CreateBuffer(&buffer_desc, NULL, &context->crt_constants);
    if (FAILED(result)) {
        vice_directx_impl_log_windows_error("CRT beam simulation constants");
        DX_RELEASE(context->crt_vertex_shader);
        DX_RELEASE(context->crt_pixel_shader);
        return false;
    }

    context->crt_failed = false;
    return true;
}

/** \brief Create the offscreen frame and back buffer view (size dependent) */
static bool crt_create_targets(vice_directx_renderer_context_t *context)
{
    DXGI_SWAP_CHAIN_DESC1 swap_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    ID3D11Texture2D *backbuffer = NULL;
    IDXGISurface *surface = NULL;
    FLOAT dpi_x;
    FLOAT dpi_y;
    HRESULT result;

    if (context->crt_source_bitmap && context->crt_backbuffer_rtv) {
        return true;
    }
    if (!context->d3d_swap_chain || !context->dxgi_bitmap) {
        return false;
    }

    /* back buffer as a render target for the shader */
    result = context->d3d_swap_chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (SUCCEEDED(result)) {
        result = context->d3d_device->CreateRenderTargetView(backbuffer, NULL, &context->crt_backbuffer_rtv);
        backbuffer->Release();
    }
    if (FAILED(result)) {
        vice_directx_impl_log_windows_error("CRT beam simulation back buffer view");
        return false;
    }

    /* offscreen copy of the back buffer for Direct2D to scale the frame into */
    context->d3d_swap_chain->GetDesc1(&swap_desc);
    ZeroMemory(&texture_desc, sizeof(texture_desc));
    texture_desc.Width            = swap_desc.Width;
    texture_desc.Height           = swap_desc.Height;
    texture_desc.MipLevels        = 1;
    texture_desc.ArraySize        = 1;
    texture_desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage            = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    result = context->d3d_device->CreateTexture2D(&texture_desc, NULL, &context->crt_source_texture);
    if (SUCCEEDED(result)) {
        result = context->d3d_device->CreateShaderResourceView(context->crt_source_texture, NULL, &context->crt_source_srv);
    }
    if (SUCCEEDED(result)) {
        result = context->crt_source_texture->QueryInterface(IID_PPV_ARGS(&surface));
    }
    if (SUCCEEDED(result)) {
        /* same properties as the back buffer bitmap, so Direct2D draws identically */
        context->dxgi_bitmap->GetDpi(&dpi_x, &dpi_y);
        D2D1_BITMAP_PROPERTIES1 bitmap_properties =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                dpi_x,
                dpi_y
                );
        result = context->d2d_device_context->CreateBitmapFromDxgiSurface(surface, bitmap_properties,
                                                                          &context->crt_source_bitmap);
    }
    DX_RELEASE(surface);

    if (FAILED(result)) {
        vice_directx_impl_log_windows_error("CRT beam simulation offscreen frame");
        DX_RELEASE(context->crt_source_bitmap);
        DX_RELEASE(context->crt_source_srv);
        DX_RELEASE(context->crt_source_texture);
        DX_RELEASE(context->crt_backbuffer_rtv);
        return false;
    }

    return true;
}

/** \brief Release the size dependent CRT simulation resources */
static void crt_destroy_targets(vice_directx_renderer_context_t *context)
{
    DX_RELEASE(context->crt_source_bitmap);
    DX_RELEASE(context->crt_source_srv);
    DX_RELEASE(context->crt_source_texture);
    DX_RELEASE(context->crt_backbuffer_rtv);
}

/** \brief Release the device dependent CRT simulation resources */
static void crt_destroy_shaders(vice_directx_renderer_context_t *context)
{
    DX_RELEASE(context->crt_constants);
    DX_RELEASE(context->crt_pixel_shader);
    DX_RELEASE(context->crt_vertex_shader);
}

/**
 * \brief Draw one subframe of the rolling scan into the back buffer
 *
 * \param[in]   context     renderer context
 * \param[in]   subframe    subframe to draw, 0 .. subframes - 1
 * \param[in]   subframes   subframes per frame
 * \param[in]   flipidx     mirroring used when drawing (bit 0 = x, bit 1 = y)
 */
static void crt_draw_subframe(vice_directx_renderer_context_t *context, int subframe, int subframes, int flipidx)
{
    ID3D11DeviceContext *dc = context->d3d_device_context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_VIEWPORT viewport;
    ID3D11ShaderResourceView *no_srv = NULL;
    FLOAT dpi_x;
    FLOAT dpi_y;
    float scale_x;
    float scale_y;

    context->crt_source_texture->GetDesc(&texture_desc);

    if (SUCCEEDED(dc->Map(context->crt_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        crt_constants_t *constants = (crt_constants_t *)mapped.pData;
        float width  = context->render_dest_rect.right  - context->render_dest_rect.left;
        float height = context->render_dest_rect.bottom - context->render_dest_rect.top;

        /*
         * Where Direct2D actually put the picture, in pixels: the drawing
         * code translates by render_dest_rect in DIPs (scaled by the target
         * DPI), while the scaled picture itself is width x height pixels.
         */
        context->crt_source_bitmap->GetDpi(&dpi_x, &dpi_y);
        scale_x = dpi_x / 96.0f;
        scale_y = dpi_y / 96.0f;

        if (flipidx & 1) {
            constants->rect[2] = context->render_dest_rect.right * scale_x;
            constants->rect[0] = constants->rect[2] - width;
        } else {
            constants->rect[0] = context->render_dest_rect.left * scale_x;
            constants->rect[2] = constants->rect[0] + width;
        }
        if (flipidx & 2) {
            constants->rect[3] = context->render_dest_rect.bottom * scale_y;
            constants->rect[1] = constants->rect[3] - height;
        } else {
            constants->rect[1] = context->render_dest_rect.top * scale_y;
            constants->rect[3] = constants->rect[1] + height;
        }
        constants->subframe  = (float)subframe;
        constants->subframes = (float)subframes;
        constants->gamma     = 2.2f;
        constants->persistence = (float)subframes
                               * (float)context->crt_persistence / 100.0f;
        constants->decay       = (float)subframes
                               * (float)context->crt_decay / 100.0f;
        if (constants->decay < 0.001f) {
            constants->decay = 0.001f;
        }
        constants->pad[0] = constants->pad[1] = constants->pad[2] = 0.0f;
        dc->Unmap(context->crt_constants, 0);
    }

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = (FLOAT)texture_desc.Width;
    viewport.Height   = (FLOAT)texture_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    dc->OMSetRenderTargets(1, &context->crt_backbuffer_rtv, NULL);
    dc->OMSetBlendState(NULL, NULL, 0xffffffff);
    dc->RSSetState(NULL);
    dc->RSSetViewports(1, &viewport);
    dc->IASetInputLayout(NULL);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(context->crt_vertex_shader, NULL, 0);
    dc->PSSetShader(context->crt_pixel_shader, NULL, 0);
    dc->PSSetShaderResources(0, 1, &context->crt_source_srv);
    dc->PSSetConstantBuffers(0, 1, &context->crt_constants);
    dc->Draw(3, 0);

    /* unbind, Direct2D renders into the source texture again for the next frame */
    dc->PSSetShaderResources(0, 1, &no_srv);
    dc->OMSetRenderTargets(0, NULL, NULL);
}

/**
 * \brief How many CRT simulation subframes fit into one frame on this display
 *
 * \return  subframes per frame, 0 when the display is too slow for it
 */
static int crt_subframes(vice_directx_renderer_context_t *context)
{
    /* the nominal frame rate: the measured one jitters, the count must not */
    double refresh = vsync_get_refresh_frequency();
    double period_ms = refresh > 1.0 ? 1000.0 / refresh : context->pacing_period_ms;
    int subframes;

    if (context->pacing_display_hz <= 1 || period_ms <= 0.0) {
        return 0;
    }

    subframes = (int)floor(context->pacing_display_hz * period_ms / (1000.0 * CRT_REFRESH_MARGIN));
    if (subframes > CRT_MAX_SUBFRAMES) {
        subframes = CRT_MAX_SUBFRAMES;
    }

    return subframes >= 2 ? subframes : 0;
}

/** \brief Current time in ms, from the performance counter */
double vice_directx_now_ms(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);

    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

/** \brief Wait until \a target_ms: timer for most of it, then spin for precision */
static void pacing_wait_until(vice_directx_renderer_context_t *context, double target_ms)
{
    double remaining = target_ms - vice_directx_now_ms();

    if (remaining <= 0.0) {
        return;
    }

    if (remaining > 1.5) {
        if (!context->pacing_timer) {
            context->pacing_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }
        if (context->pacing_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((remaining - 1.0) * 10000.0); /* relative, 100 ns units */
            if (SetWaitableTimer(context->pacing_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(context->pacing_timer, INFINITE);
            }
        } else {
            Sleep((DWORD)(remaining - 1.5));
        }
    }

    while (vice_directx_now_ms() < target_ms) {
        YieldProcessor();
    }
}

/** \brief Refresh rate of the display that currently shows the swap chain */
static int pacing_query_display_hz(vice_directx_renderer_context_t *context)
{
    IDXGIOutput *output = NULL;
    DXGI_OUTPUT_DESC desc;
    DEVMODEW mode;
    int hz = 0;

    if (context->d3d_swap_chain && SUCCEEDED(context->d3d_swap_chain->GetContainingOutput(&output))) {
        if (SUCCEEDED(output->GetDesc(&desc))) {
            ZeroMemory(&mode, sizeof(mode));
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                hz = (int)mode.dmDisplayFrequency;
            }
        }
        output->Release();
    }

    return hz;
}

/** \brief Should each frame be presented twice (see PACING_DOUBLE_MIN_RATIO) */
static bool pacing_should_double(vice_directx_renderer_context_t *context)
{
    if (--context->pacing_display_check <= 0) {
        int hz = pacing_query_display_hz(context);
        if (hz != context->pacing_display_hz) {
            context->pacing_display_hz = hz;
            log_message(LOG_DEFAULT, "DirectX: display refresh %d Hz, frame pacing presents each frame %s",
                        hz, (hz > 1 && hz * context->pacing_period_ms >= 1000.0 * PACING_DOUBLE_MIN_RATIO) ? "twice" : "once");
        }
        context->pacing_display_check = PACING_DISPLAY_CHECK_FRAMES;
    }

    /* the display refreshes at least PACING_DOUBLE_MIN_RATIO times per emulated frame */
    return context->pacing_display_hz > 1
        && context->pacing_display_hz * context->pacing_period_ms >= 1000.0 * PACING_DOUBLE_MIN_RATIO;
}

/**
 * \brief Decide when to present a new frame
 *
 * Presents follow a steady clock at the emulated frame rate. The emulator
 * delivers frames in bursts, so the clock runs a little behind them, by the
 * latest a frame arrived in the last PACING_BLOCKS * PACING_BLOCK_FRAMES
 * frames plus a margin.
 *
 * Besides the frame arriving, drawing it can take a while now and then (the
 * GPU busy with something else); pacing_ready() takes that into account too.
 *
 * The clock is never moved in one step, as every jump shows as a hitch in
 * scrolling: unneeded latency is trimmed slowly, and after a late frame (shown
 * at once) the clock moves later over several frames. Clock drift between the
 * audio device (which paces the emulator) and the system timer is absorbed the
 * same way.
 *
 * \param[in]   context         renderer context
 * \param[in]   completed_ms    when the emulator finished the frame
 * \param[in]   queued          frames waiting behind this one
 *
 * \return  time to present the frame at (ms)
 */
static double pacing_schedule(vice_directx_renderer_context_t *context, double completed_ms, unsigned int queued)
{
    double refresh = vsync_get_refresh_frequency();
    double nominal = refresh > 1.0 ? 1000.0 / refresh : 20.0;
    double planned;
    double target;
    double slack;
    double step;

    /* Measure the actual frame rate (slowly), to notice speed changes */
    if (context->pacing_measured_ms <= 0.0) {
        context->pacing_measured_ms = nominal;
    } else if (context->pacing_last_completed_ms > 0.0) {
        double interval = completed_ms - context->pacing_last_completed_ms;
        if (interval > 0.0 && interval < nominal * 4.0) {
            context->pacing_measured_ms += (interval - context->pacing_measured_ms) * 0.002;
        }
    }
    context->pacing_last_completed_ms = completed_ms;

    /* The nominal rate, unless the emulator clearly runs at another speed */
    if (fabs(context->pacing_measured_ms - nominal) > nominal * PACING_SPEED_CHANGE) {
        context->pacing_period_ms = context->pacing_measured_ms;
    } else {
        context->pacing_period_ms = nominal;
    }

    if (context->pacing_next_ms <= 0.0 || completed_ms > context->pacing_next_ms + PACING_RESET_MS) {
        /* (Re)start in step with this frame */
        int i;

        context->pacing_next_ms = completed_ms + PACING_START_MS;
        context->pacing_min_wait_ms = 1e9;
        context->pacing_window_frames = 0;
        for (i = 0; i < PACING_BLOCKS; i++) {
            context->pacing_block_min_ms[i] = 1e9; /* nothing known yet */
        }
        context->pacing_block_index = 0;
        context->pacing_adjust_ms = 0.0;
        context->pacing_shift_ms = 0.0;
        context->pacing_frames = 0;
    }
    if (context->pacing_frames < PACING_WARMUP_FRAMES) {
        context->pacing_frames++;
    }

    planned = context->pacing_next_ms;
    context->pacing_dequeued_ms = vice_directx_now_ms();
    slack = planned - completed_ms;
    target = planned;

    if (slack < 0.0) {
        /* Late: show it as soon as it exists */
        target = completed_ms;
        if (context->pacing_frames < PACING_WARMUP_FRAMES) {
            /* the start is bumpy and a jump doesn't show yet: just follow */
            planned = completed_ms;
            slack = 0.0;
        } else if (context->pacing_adjust_ms < PACING_MARGIN_MS - slack) {
            /* move the clock later, gradually */
            context->pacing_adjust_ms = PACING_MARGIN_MS - slack;
        }
    }
    context->pacing_planned_ms = planned;

    if (queued + 1 >= RENDER_QUEUE_MAX_BACKBUFFERS) {
        /* Queue about to overflow (emulator ran ahead): show it now, or frames get dropped */
        if (target > vice_directx_now_ms()) {
            target = vice_directx_now_ms();
        }
    } else if (queued >= 2 && context->pacing_adjust_ms > -context->pacing_period_ms * 0.5) {
        /* More latency than needed: trim it, faster than usual but still gradually */
        context->pacing_adjust_ms = -context->pacing_period_ms * 0.5;
    }

    /*
     * Least slack in the block, as if all shifts done since had been there
     * from the start, so trimming doesn't count the same latency twice.
     */
    if (slack - context->pacing_shift_ms < context->pacing_min_wait_ms) {
        context->pacing_min_wait_ms = slack - context->pacing_shift_ms;
    }
    if (++context->pacing_window_frames >= PACING_BLOCK_FRAMES) {
        double worst = 1e9;
        double projected;
        int i;

        context->pacing_block_min_ms[context->pacing_block_index] = context->pacing_min_wait_ms;
        context->pacing_block_index = (context->pacing_block_index + 1) % PACING_BLOCKS;
        for (i = 0; i < PACING_BLOCKS; i++) {
            if (context->pacing_block_min_ms[i] < worst) {
                worst = context->pacing_block_min_ms[i];
            }
        }

        /* the least slack once all shifts in progress are done */
        projected = worst + context->pacing_shift_ms + context->pacing_adjust_ms;
        if (projected > PACING_MARGIN_MS + 0.5) {
            context->pacing_adjust_ms -= projected - PACING_MARGIN_MS;
        }
        context->pacing_min_wait_ms = 1e9;
        context->pacing_window_frames = 0;
    }

    /* Move the clock gradually */
    step = context->pacing_adjust_ms;
    if (step > PACING_SLEW_LATER_MS) {
        step = PACING_SLEW_LATER_MS;
    } else if (step < -(queued >= 2 ? PACING_SLEW_LATER_MS : PACING_SLEW_EARLIER_MS)) {
        step = -(queued >= 2 ? PACING_SLEW_LATER_MS : PACING_SLEW_EARLIER_MS);
    }
    context->pacing_adjust_ms -= step;
    context->pacing_shift_ms += step;

    context->pacing_next_ms = planned + context->pacing_period_ms + step;

    return target;
}

/**
 * \brief Note when a paced frame was drawn and ready to be presented
 *
 * Like a frame arriving late, a frame that took long to draw moves the clock
 * later (gradually), and the latency kept covers the slowest draw in the
 * window.
 *
 * \param[in]   context     renderer context
 */
static void pacing_ready(vice_directx_renderer_context_t *context)
{
    if (context->pacing_frames < PACING_WARMUP_FRAMES) {
        return; /* the first frames are slow to draw (shaders, textures) */
    }

    /*
     * As if the frame had arrived that much later: when it was finished plus
     * how long drawing it took, against the steady clock. Not simply "now",
     * that may be late only because the render thread was still busy showing
     * the previous frame (CRT subframes), which the clock already allows for.
     */
    double draw_ms = vice_directx_now_ms() - context->pacing_dequeued_ms;
    double slack = context->pacing_planned_ms - (context->pacing_last_completed_ms + draw_ms);

    if (slack < 0.0 && context->pacing_adjust_ms < PACING_MARGIN_MS - slack) {
        context->pacing_adjust_ms = PACING_MARGIN_MS - slack;
    }
    if (slack - context->pacing_shift_ms < context->pacing_min_wait_ms) {
        context->pacing_min_wait_ms = slack - context->pacing_shift_ms;
    }
}

static void build_directx_resources(vice_directx_renderer_context_t *context)
{
    HRESULT result = S_OK;

    /*
     * Device dependent resources
     */

    /* Direct2D Factory for later */

    if (!context->d2d_factory) {
        D2D1_FACTORY_OPTIONS d2d_factory_options;
        ZeroMemory(&d2d_factory_options, sizeof(d2d_factory_options));

        result =
            D2D1CreateFactory(
                D2D1_FACTORY_TYPE_MULTI_THREADED,
                __uuidof(ID2D1Factory1),
                &d2d_factory_options,
                (void **)&context->d2d_factory
                );
        if (FAILED(result)) {
            vice_directx_impl_log_windows_error("D2D1CreateFactory1");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }

    /* Create the Direct 3D device */

    D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
            D3D_FEATURE_LEVEL_9_2,
            D3D_FEATURE_LEVEL_9_1
        };

    if (!context->d3d_device) {
        ID3D11Device *d3d_device;
        ID3D11DeviceContext *d3d_device_context;
        result =
            D3D11CreateDevice(
                nullptr,                            /* default gpu adapter */
                D3D_DRIVER_TYPE_HARDWARE,
                0,                                  /* no software renderer implementation DLL */
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,   /* needed for Direct2D compatibility */
                featureLevels,
                ARRAYSIZE(featureLevels),
                D3D11_SDK_VERSION,
                &d3d_device,
                NULL,                               /* don't return resulting feature level */
                &d3d_device_context
                );
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("D3D11CreateDevice");
            vice_directx_destroy_context_impl(context);
            return;
        }

        result = d3d_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&context->d3d_device);
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("ID3D11Device1");
            vice_directx_destroy_context_impl(context);
            return;
        }
        d3d_device->Release();

        result = d3d_device_context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&context->d3d_device_context);
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("ID3D11DeviceContext1");
            vice_directx_destroy_context_impl(context);
            return;
        }
        d3d_device_context->Release();
    }

    /* Get the underlying DXGI Device */

    if (!context->dxgi_device) {
        result = context->d3d_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&context->dxgi_device);
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("QueryInterface IDXGIDevice");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }

    /* now get the Direct2D device and device context */

    if (!context->d2d_device) {
        result = context->d2d_factory->CreateDevice(context->dxgi_device, &context->d2d_device);
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("CreateDevice ID2D1Device");
            vice_directx_destroy_context_impl(context);
            return;
        }

        result =
            context->d2d_device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                &context->d2d_device_context
                );
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("CreateDeviceContext ID2D1DeviceContext");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }

    /* Set up some image processing effects */

    if (!context->d2d_effect_strip_alpha) {
        /* Set alpha = 1, wonder if there's a more efficient way */
        D2D1_MATRIX_5X4_F strip_alpha_matrix =
            D2D1::Matrix5x4F(
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 0,
                0, 0, 0, 1
                );
        context->d2d_device_context->CreateEffect(CLSID_D2D1ColorMatrix, &context->d2d_effect_strip_alpha);
        context->d2d_effect_strip_alpha->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, strip_alpha_matrix);
    }

    if (!context->d2d_effect_premultiply_alpha) {
        /* VICE produces straight alpha images, Direct2D works with premultiplied alpha */
        context->d2d_device_context->CreateEffect(CLSID_D2D1Premultiply, &context->d2d_effect_premultiply_alpha);
    }

    if (!context->d2d_effect_combine) {
        /* Combine two bitmaps, used to stitch interlaced bitmaps */
        context->d2d_device_context->CreateEffect(CLSID_D2D1Composite, &context->d2d_effect_combine);
    }

    if (!context->d2d_effect_scale) {
        /* And scale up the final result */
        context->d2d_device_context->CreateEffect(CLSID_D2D1Scale, &context->d2d_effect_scale);
    }

    /*
     * Size dependent resources
     */

    /* Get the DXGI Adapter */

    if (!context->dxgi_adapter) {
        result = context->dxgi_device->GetAdapter(&context->dxgi_adapter);
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("GetAdapter IDXGIAdapter");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }

    /* Don't forget the DXGI Factory! You nearly forgot didn't you. */

    if (!context->dxgi_factory) {
        result = context->dxgi_adapter->GetParent(IID_PPV_ARGS(&context->dxgi_factory));
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("dxgi_factory");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }

    /* Now we need to create a swap chain for our hwnd */

    if (!context->d3d_swap_chain && vice_directx_now_ms() < context->swap_chain_retry_ms) {
        return; /* failed recently, try again later (see swap_chain_retry_ms) */
    }

    if (!context->d3d_swap_chain) {

        DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = { 0 };
        swap_chain_desc.Width                 = 0;                                /* use automatic sizing */
        swap_chain_desc.Height                = 0;
        swap_chain_desc.Format                = DXGI_FORMAT_B8G8R8A8_UNORM;       /* this is the most common swapchain format */
        swap_chain_desc.Stereo                = false;
        swap_chain_desc.SampleDesc.Count      = 1;                                /* don't use multi-sampling */
        swap_chain_desc.SampleDesc.Quality    = 0;
        swap_chain_desc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.BufferCount           = 2;                                /* use double buffering to enable flip */
        swap_chain_desc.Scaling               = DXGI_SCALING_STRETCH;

        context->flip_model    = false;
        context->allow_tearing = false;

        if (!context->flip_model_failed) {
            /*
             * Flip presentation model (Windows 10+). Unlike the old bitblt model
             * it lets the frame go to the display without an extra copy by the
             * compositor, which is what variable refresh rate (G-Sync / FreeSync)
             * needs to work in a window or borderless fullscreen. The whole back
             * buffer is cleared and redrawn every frame, so discarding it is fine.
             */
            BOOL tearing_supported = FALSE;
            IDXGIFactory5 *dxgi_factory5 = NULL;

            if (SUCCEEDED(context->dxgi_factory->QueryInterface(IID_PPV_ARGS(&dxgi_factory5)))) {
                if (FAILED(dxgi_factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                              &tearing_supported,
                                                              sizeof(tearing_supported)))) {
                    tearing_supported = FALSE;
                }
                dxgi_factory5->Release();
            }

            swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swap_chain_desc.Flags      = tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            /*
             * Three buffers: with only two, drawing the next frame (or CRT
             * subframe) has to wait until the previous one is on screen, and
             * a display refresh that starts a little late then delays it by a
             * whole refresh.
             */
            swap_chain_desc.BufferCount = 3;

            result =
                context->dxgi_factory->CreateSwapChainForHwnd(
                    context->d3d_device,
                    context->window,
                    &swap_chain_desc,
                    NULL,                       /* Windowed swap chain */
                    NULL,                       /* Don't restrict output device */
                    &context->d3d_swap_chain
                );
            if (SUCCEEDED(result)) {
                context->flip_model    = true;
                context->allow_tearing = tearing_supported ? true : false;
                log_message(LOG_DEFAULT, "DirectX: using flip model swap chain%s",
                            context->allow_tearing ? " (tearing supported)" : "");
            } else {
                if (!context->swap_chain_failure_logged) {
                    vice_directx_impl_log_windows_error("flip model d3d_swap_chain, falling back to bitblt model");
                }
                context->flip_model_failed = true;
            }
        }

        if (!context->d3d_swap_chain) {
            swap_chain_desc.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
            swap_chain_desc.Flags       = 0;
            swap_chain_desc.BufferCount = 2;

            result =
                context->dxgi_factory->CreateSwapChainForHwnd(
                    context->d3d_device,
                    context->window,
                    &swap_chain_desc,
                    NULL,                       /* Windowed swap chain */
                    NULL,                       /* Don't restrict output device */
                    &context->d3d_swap_chain
                );
            if (FAILED(result))
            {
                /*
                 * Keep the device and retry later: recreating everything
                 * every frame takes a whole core (the driver is reloaded and
                 * its signature checked each time) and, as it is done with the
                 * canvas locked, stalls the emulator too.
                 */
                if (!context->swap_chain_failure_logged) {
                    log_error(LOG_DEFAULT, "DirectX: cannot create a swap chain (0x%08lx), retrying every %d ms",
                              (unsigned long)result, SWAP_CHAIN_RETRY_MS);
                    context->swap_chain_failure_logged = true;
                }
                context->d3d_swap_chain = NULL;
                context->swap_chain_retry_ms = vice_directx_now_ms() + SWAP_CHAIN_RETRY_MS;
                /* neither model works now: try flip model first again next time */
                context->flip_model_failed = false;
                return;
            }
        }
        if (context->swap_chain_failure_logged) {
            log_message(LOG_DEFAULT, "DirectX: swap chain created");
            context->swap_chain_failure_logged = false;
        }

        /* Ensure that DXGI doesn't queue more than one frame at a time. */
        context->dxgi_device->SetMaximumFrameLatency(1);
    }

    /*
     * Frame dependant resources
     */

    /* Get a DXGI surface representing the backbuffer */

    if (!context->dxgi_surface && context->d3d_swap_chain) {
        result = context->d3d_swap_chain->GetBuffer(0, IID_PPV_ARGS(&context->dxgi_surface));
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("dxgi_surface");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }

    /* Create a bitmap from the DXGI surface */
    FLOAT dpiX, dpiY;
    context->d2d_factory->GetDesktopDpi(&dpiX, &dpiY);

    if (!context->dxgi_bitmap) {
        D2D1_BITMAP_PROPERTIES1 bitmap_properties =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                dpiX,
                dpiY
                );

        result =
            context->d2d_device_context->CreateBitmapFromDxgiSurface(
                context->dxgi_surface,
                bitmap_properties,
                &context->dxgi_bitmap
                );
        if (FAILED(result))
        {
            vice_directx_impl_log_windows_error("dxgi_bitmap");
            vice_directx_destroy_context_impl(context);
            return;
        }
    }
}

static void destroy_size_dependent_resources(vice_directx_renderer_context_t *context)
{
    crt_destroy_targets(context);
    DX_RELEASE(context->dxgi_bitmap);
    DX_RELEASE(context->dxgi_surface);
    DX_RELEASE(context->d3d_swap_chain);
    DX_RELEASE(context->dxgi_factory);
    DX_RELEASE(context->dxgi_adapter);
}

static void destroy_device_dependent_resources(vice_directx_renderer_context_t *context)
{
    crt_destroy_shaders(context);
    context->crt_failed = false;

    DX_RELEASE(context->render_bitmap);
    DX_RELEASE(context->previous_frame_render_bitmap);

    DX_RELEASE(context->d2d_effect_scale);
    DX_RELEASE(context->d2d_effect_combine);
    DX_RELEASE(context->d2d_effect_premultiply_alpha);
    DX_RELEASE(context->d2d_effect_strip_alpha);
    DX_RELEASE(context->d2d_device_context);
    DX_RELEASE(context->d2d_device);
    DX_RELEASE(context->dxgi_device);
    DX_RELEASE(context->d3d_device_context);
    DX_RELEASE(context->d3d_device);
    DX_RELEASE(context->d2d_factory);
}

void vice_directx_destroy_context_impl(vice_directx_renderer_context_t *context)
{
    destroy_size_dependent_resources(context);
    destroy_device_dependent_resources(context);
}

static void build_render_bitmap(vice_directx_renderer_context_t *context, backbuffer_t *backbuffer)
{
    HRESULT result = S_OK;

    if (context->d2d_device_context) {
        if (backbuffer->interlace_field != context->current_interlace_field) {
            /* Retain the previous bitmaps to use in interlaced mode */
            ID2D1Bitmap *swap_bitmap              = context->previous_frame_render_bitmap;
            unsigned int swap_width               = context->previous_frame_bitmap_width;
            unsigned int swap_height              = context->previous_frame_bitmap_height;
            context->previous_frame_render_bitmap = context->render_bitmap;
            context->previous_frame_bitmap_width  = context->bitmap_width;
            context->previous_frame_bitmap_height = context->bitmap_height;
            context->render_bitmap                = swap_bitmap;
            context->bitmap_width                 = swap_width;
            context->bitmap_height                = swap_height;
            context->current_interlace_field      = backbuffer->interlace_field;
        }

        /* Is it sill the right size? */
        /* HACK - need to track each bitmap size */
        if (context->render_bitmap && (context->bitmap_width != backbuffer->width || context->bitmap_height != backbuffer->height)) {
            /* Nope, release it and let another be created */
            DX_RELEASE(context->render_bitmap);
        }

        /* Create bitmaps, if needed */
        if (!context->render_bitmap)
        {
            /*
             * Create the Direct2D bitmap used to get the emu display into the gpu
             */

            D2D1_BITMAP_PROPERTIES bitmap_properies;
            bitmap_properies.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
            context->d2d_factory->GetDesktopDpi(&bitmap_properies.dpiX, &bitmap_properies.dpiY);

            result =
                context->d2d_device_context->CreateBitmap(
                    D2D1::SizeU(backbuffer->width, backbuffer->height),
                    bitmap_properies,
                    &context->render_bitmap);

            if (FAILED(result)) {
                vice_directx_impl_log_windows_error("CreateBitmap1");
                return;
            }

            context->bitmap_width  = backbuffer->width;
            context->bitmap_height = backbuffer->height;
        }

        context->interlaced = backbuffer->interlaced;
        context->pixel_aspect_ratio = backbuffer->pixel_aspect_ratio;

        /* Copy the emulated screen to the Bitmap */
        D2D1_RECT_U bitmap_rect = D2D1::RectU(0, 0, backbuffer->width, backbuffer->height);

        result =
            context->render_bitmap->CopyFromMemory(
                &bitmap_rect,
                backbuffer->pixel_data,
                backbuffer->width * 4);
        if (FAILED(result)) {
            vice_directx_impl_log_windows_error("CopyFromMemory");
            DX_RELEASE(context->render_bitmap);
            return;
        }
    }
}

static void recalculate_layout(video_canvas_t *canvas, vice_directx_renderer_context_t *context)
{
    float scale_x;
    float scale_y;
    D2D1_SIZE_U  viewport_size_ddp;
    D2D1_SIZE_U  bitmap_size_ddp;

    if (!context->dxgi_bitmap || !context->render_bitmap) {
        return;
    }

    viewport_size_ddp   = context->dxgi_bitmap->GetPixelSize();
    bitmap_size_ddp     = context->render_bitmap->GetPixelSize();

    if (canvas->videoconfig->aspect_mode != VIDEO_ASPECT_MODE_NONE) {
        float viewport_aspect;
        float emulated_aspect;

        viewport_aspect = (float)viewport_size_ddp.width / viewport_size_ddp.height;
        emulated_aspect = (float)bitmap_size_ddp.width   / bitmap_size_ddp.height;

        if (canvas->videoconfig->aspect_mode == VIDEO_ASPECT_MODE_TRUE) {
            emulated_aspect *= context->pixel_aspect_ratio;
        } else {
            emulated_aspect *= canvas->videoconfig->aspect_ratio;
        }

        if (emulated_aspect < viewport_aspect) {
            scale_x = emulated_aspect / viewport_aspect;
            scale_y = 1.0f;
        } else {
            scale_x = 1.0f;
            scale_y = viewport_aspect / emulated_aspect;
        }
    } else {
        scale_x = 1.0f;
        scale_y = 1.0f;
    }

    /* Calculate the minimum drawing area size to be enforced by gtk */
    if (canvas->videoconfig->aspect_mode == VIDEO_ASPECT_MODE_TRUE) {
        context->window_min_width = ceil((float)context->bitmap_width * context->pixel_aspect_ratio);
        context->window_min_height = context->bitmap_height;
    } else {
        context->window_min_width = context->bitmap_width;
        context->window_min_height = context->bitmap_height;
    }

    /* These values aren't used anywhere other than here in this renderer */
    canvas->screen_display_w = (viewport_size_ddp.width  * scale_x);
    canvas->screen_display_h = (viewport_size_ddp.height * scale_y);
    canvas->screen_origin_x = ((viewport_size_ddp.width  - canvas->screen_display_w) / 2.0);
    canvas->screen_origin_y = ((viewport_size_ddp.height - canvas->screen_display_h) / 2.0);

#if 0
    /*
     * Direct2D thinks in terms of device indepenent pixels,
     * but GTK gives us pixel sizes in device dependent pixel sizes.
     *
     * When rendering, we convert from DDP to DIP, this ensures that
     * our filter choice is applied rather than whatever D2D is doing
     * under the hood when you set dpi on the bitmap itself.
     */

    context->d2d_factory->GetDesktopDpi(&dpi_x, &dpi_y);

    /* Update the detination rect used directly by the renderer */
    context->render_dest_rect.left   =  canvas->screen_origin_x                             * 96.0f / dpi_x;
    context->render_dest_rect.right  = (canvas->screen_origin_x + canvas->screen_display_w) * 96.0f / dpi_x;
    context->render_dest_rect.top    =  canvas->screen_origin_y                             * 96.0f / dpi_y;
    context->render_dest_rect.bottom = (canvas->screen_origin_y + canvas->screen_display_h) * 96.0f / dpi_y;
#endif

    /* Update the detination rect used directly by the renderer */
    context->render_dest_rect.left   = canvas->screen_origin_x;
    context->render_dest_rect.right  = canvas->screen_origin_x + canvas->screen_display_w;
    context->render_dest_rect.top    = canvas->screen_origin_y;
    context->render_dest_rect.bottom = canvas->screen_origin_y + canvas->screen_display_h;
}

void vice_directx_impl_async_render(void *job_data, void *pool_data)
{
    render_job_t job = (render_job_t)vice_ptr_to_int(job_data);
    video_canvas_t *canvas = (video_canvas_t *)pool_data;
    vice_directx_renderer_context_t *context = (vice_directx_renderer_context_t *)canvas->renderer_context;
    backbuffer_t *backbuffer;
    HRESULT result = S_OK;
    bool interlaced;
    int vsync = canvas->videoconfig->vsync;
    int filter = canvas->videoconfig->glfilter;
    int flipidx = canvas->videoconfig->flipx | (canvas->videoconfig->flipy << 1);
    DXGI_PRESENT_PARAMETERS present_parameters = { 0 };
    double present_at_ms = 0.0;
    double repeat_at_ms = 0.0;
    int crt_subframe_count = 0;
    int subframe;
    bool show_twice = false;
    bool showing_repeat = false;

    if (job == render_thread_init) {
        archdep_thread_init();

        /* Make sure the render thread wakes up and does its thing asap. */
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        log_verbose(LOG_DEFAULT, "Render thread initialised");
        return;
    }

    if (job == render_thread_shutdown) {
        archdep_thread_shutdown();
        log_verbose(LOG_DEFAULT, "Render thread shutdown");
        return;
    }

    CANVAS_LOCK();

    if (context->resized) {
        /*
         * The Direct2D context keeps the back buffer alive while it is the
         * render target, and D3D defers destroying swap chains. Only one flip
         * model swap chain may exist per window, so let go of everything now
         * or recreating it fails. Done here, on the render thread, because
         * the device contexts are not safe to use from other threads.
         */
        if (context->d2d_device_context) {
            context->d2d_device_context->SetTarget(NULL);
        }
        if (context->d3d_device_context) {
            context->d3d_device_context->ClearState();
            context->d3d_device_context->Flush();
        }
        destroy_size_dependent_resources(context);
        context->resized = false;
    }

    RENDER_LOCK();

    /*
     * Update the bitmaps. All need to be processed for correct interlace rendering,
     * especially when the monitor is open and stepping through code.
     */

    backbuffer = render_queue_dequeue_for_display(context->render_queue);
    if (backbuffer && vsync && !vsync_get_warp_mode()) {
        present_at_ms = pacing_schedule(context, backbuffer->completed_time_ms,
                                        render_queue_length(context->render_queue));
        /* also keeps the display refresh rate up to date */
        show_twice = pacing_should_double(context);

        if (canvas->videoconfig->crt_beam_sim) {
            context->crt_persistence = canvas->videoconfig->crt_beam_persistence;
            context->crt_decay = canvas->videoconfig->crt_beam_decay;
            crt_subframe_count = crt_subframes(context);
            if (crt_subframe_count != context->crt_logged_subframes) {
                if (crt_subframe_count) {
                    log_message(LOG_DEFAULT, "DirectX: CRT beam simulation, %d subframes per frame (display %d Hz)",
                                crt_subframe_count, context->pacing_display_hz);
                } else {
                    log_message(LOG_DEFAULT, "DirectX: CRT beam simulation needs a display of at least %.0f Hz, not used",
                                2000.0 * CRT_REFRESH_MARGIN / context->pacing_period_ms);
                }
                context->crt_logged_subframes = crt_subframe_count;
            }
        }
        if (!crt_subframe_count && show_twice) {
            repeat_at_ms = present_at_ms + context->pacing_period_ms * 0.5;
        }
    } else if (backbuffer) {
        context->pacing_next_ms = 0.0;
    }
    if (backbuffer) {
        build_render_bitmap(context, backbuffer);
        render_queue_return_to_pool(context->render_queue, backbuffer);
    }

    build_directx_resources(context);

    recalculate_layout(canvas, context);

    interlaced = context->interlaced;

    CANVAS_UNLOCK();

    if (!context->d2d_device_context || !context->dxgi_bitmap) {
        /* no device or no swap chain (yet): nothing to draw into */
        if (!context->swap_chain_failure_logged) {
            log_message(LOG_DEFAULT, "no render target, not rendering this frame");
        }
        RENDER_UNLOCK();
        return;
    }

    if (crt_subframe_count && !(crt_create_shaders(context) && crt_create_targets(context))) {
        /* CRT simulation not possible, show the frame normally */
        crt_subframe_count = 0;
        if (show_twice) {
            repeat_at_ms = present_at_ms + context->pacing_period_ms * 0.5;
        }
    }

draw_frame:
    /*
     * Each frame, set the backbuffer bitmap as the Direct2D render target.
     * With CRT beam simulation the frame goes to the offscreen texture
     * instead, and the shader draws the subframes into the back buffer.
     */
    context->d2d_device_context->SetTarget(crt_subframe_count ? context->crt_source_bitmap : context->dxgi_bitmap);

    context->d2d_device_context->BeginDraw();
    context->d2d_device_context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->d2d_device_context->Clear(&context->render_bg_colour);

    if (interlaced && context->previous_frame_render_bitmap && context->render_bitmap) {
        /* Render the previous frame ignoring alpha */
        context->d2d_effect_strip_alpha->SetInput(0, context->previous_frame_render_bitmap);

        /* Premultiply the alpha on the new frame */
        context->d2d_effect_premultiply_alpha->SetInput(0, context->render_bitmap);

        /* Combine the two processed bitmaps */
        context->d2d_effect_combine->SetInputEffect(0, context->d2d_effect_strip_alpha);
        context->d2d_effect_combine->SetInputEffect(1, context->d2d_effect_premultiply_alpha);

        context->d2d_effect_scale->SetInputEffect(0, context->d2d_effect_combine);
    } else {
        /* Render the current frame ignoring alpha */
        context->d2d_effect_strip_alpha->SetInput(0, context->render_bitmap);

        context->d2d_effect_scale->SetInputEffect(0, context->d2d_effect_strip_alpha);
    }

    context->d2d_effect_scale->SetValue(
        2, /* D2D1_SCALE_PROP_INTERPOLATION_MODE, For some reason this enum value isn't picked up on msys */
        filter == 0
            ? 0   /* D2D1_SCALE_INTERPOLATION_MODE_NEAREST_NEIGHBOR */
            : filter == 2
                ? 5 /* D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC */
                : 1 /* D2D1_SCALE_INTERPOLATION_MODE_LINEAR */
        );
    context->d2d_effect_scale->SetValue(
        0, /* D2D1_SCALE_PROP_SCALE, For some reason this enum value isn't picked up on msys */
        D2D1::Vector2F(
            (float)(context->render_dest_rect.right - context->render_dest_rect.left) / context->bitmap_width,
            (float)(context->render_dest_rect.bottom - context->render_dest_rect.top) / context->bitmap_height
            )
        );

    /* FIXME: add support for rotate */

    switch (flipidx) {
        default:
        case 0:
            context->d2d_device_context->SetTransform(
                D2D1::Matrix3x2F::Translation(
                    context->render_dest_rect.left,
                    context->render_dest_rect.top
                    )
            );
            break;
        case 1:
            context->d2d_device_context->SetTransform(
                D2D1::Matrix3x2F(-1,0,0,1,0,0) *
                D2D1::Matrix3x2F::Translation(
                    context->render_dest_rect.right,
                    context->render_dest_rect.top
                    )
            );
            break;
        case 2:
            context->d2d_device_context->SetTransform(
                D2D1::Matrix3x2F(1,0,0,-1,0,0) *
                D2D1::Matrix3x2F::Translation(
                    context->render_dest_rect.left,
                    context->render_dest_rect.bottom
                    )
            );
            break;
        case 3:
            context->d2d_device_context->SetTransform(
                D2D1::Matrix3x2F(-1,0,0,-1,0,0) *
                D2D1::Matrix3x2F::Translation(
                    context->render_dest_rect.right,
                    context->render_dest_rect.bottom
                    )
            );
            break;
    }
    context->d2d_device_context->DrawImage(context->d2d_effect_scale);

    result = context->d2d_device_context->EndDraw();

    if (result == D2DERR_RECREATE_TARGET) {
        printf("Must recreate resources\n");
        destroy_size_dependent_resources(context);
        destroy_device_dependent_resources(context);
    } else {
        /*
         * With VSync on, the frame waits for the next refresh. On a VRR (G-Sync /
         * FreeSync) display that refresh starts when the frame arrives, so the
         * display follows the emulated frame rate (e.g. 50.125 Hz for PAL).
         * With VSync off, tearing is allowed when supported, otherwise the flip
         * model would still wait for the compositor.
         */
        UINT present_flags = (!vsync && context->allow_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;

        if (crt_subframe_count) {
            /* CRT beam simulation: the subframes, evenly spread over the frame */
            for (subframe = 0; subframe < crt_subframe_count; subframe++) {
                if (subframe == 0) {
                    pacing_ready(context);
                }
                if (subframe > 0 && vice_directx_now_ms() > context->pacing_next_ms) {
                    break; /* running late, the next frame is due */
                }
                crt_draw_subframe(context, subframe, crt_subframe_count, flipidx);
                /* let the GPU draw it while waiting, so it is ready when presented */
                context->d3d_device_context->Flush();
                pacing_wait_until(context, present_at_ms + subframe * context->pacing_period_ms / crt_subframe_count);
                context->d3d_swap_chain->Present1(1, 0, &present_parameters);
            }
            RENDER_UNLOCK();
            return;
        }

        if (present_at_ms > 0.0) {
            /* Frame pacing: the frame is drawn, show it on the steady clock */
            if (!showing_repeat) {
                pacing_ready(context);
            }
            /* let the GPU draw it while waiting, so it is ready when presented */
            context->d3d_device_context->Flush();
            pacing_wait_until(context, present_at_ms);
        }
        context->d3d_swap_chain->Present1(vsync ? 1 : 0, present_flags, &present_parameters);

        /*
         * Show the same frame again half a frame later (see
         * PACING_DOUBLE_MIN_RATIO), unless running late and the next frame
         * is due. The flip model discards the back buffer, so it is drawn again.
         */
        if (repeat_at_ms > 0.0 && vice_directx_now_ms() < context->pacing_next_ms) {
            present_at_ms = repeat_at_ms;
            repeat_at_ms = 0.0;
            showing_repeat = true;
            goto draw_frame;
        }
    }

    RENDER_UNLOCK();
}

/*
 * Implemented here because we can't include the vice headers in a cpp file
 */
void vice_directx_impl_log_windows_error(const char *prefix)
{
    char *error_message;
    DWORD last_error = GetLastError();

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        last_error,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        (LPSTR)&error_message,
        0,
        NULL);

    log_error(LOG_DEFAULT, "%s: %s", prefix, error_message);

    LocalFree(error_message);
}

#endif /* #ifdef WINDOWS_COMPILE */
