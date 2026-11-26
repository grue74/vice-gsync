/*
 * soundwasapi.c - Implementation of the WASAPI sound device (Windows).
 *
 * Written by
 *  vice-gsync contributors
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
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

/*
 * Why a WASAPI driver
 *
 * With sound on, the emulator is paced by the sound device: sound.c writes a
 * fragment whenever bufferspace() says there is room, and waits otherwise. So
 * frames reach the screen exactly as smoothly as bufferspace() grows. The
 * DirectSound driver's free space moves in ~10 ms steps, which makes frames
 * arrive 10/20/30 ms apart, and the video frame pacing then has to hold that
 * much extra latency.
 *
 * WASAPI's own buffer fill (GetCurrentPadding) also moves in steps of the
 * audio engine period (usually 10 ms). This driver instead takes the play
 * position from the audio clock (IAudioClock::GetPosition, which comes with
 * the performance counter time it was sampled at) and extrapolates it to
 * "now", so the free space grows continuously, sample by sample. The padding
 * is only used as a sanity window around that estimate.
 *
 * The stream is opened in shared mode with automatic format conversion, so
 * VICE's sample rate and 16-bit samples are used as they are. The endpoint
 * buffer is made much larger than VICE's buffer, so writes never wait for
 * WASAPI; how full VICE keeps it is decided by bufferspace() alone.
 */

#include "vice.h"

#ifdef WINDOWS_COMPILE

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string.h>

#include "lib.h"
#include "log.h"
#include "sound.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* The interface IDs, defined here so no extra import library is needed */
static const GUID wasapi_clsid_mmdeviceenumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID wasapi_iid_immdeviceenumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID wasapi_iid_iaudioclient =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID wasapi_iid_iaudiorenderclient =
    { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };
static const GUID wasapi_iid_iaudioclock =
    { 0xcd63314f, 0x3fba, 0x4a1b, { 0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7 } };

/** \brief Endpoint buffer asked for (100 ns units): far more than VICE keeps queued */
#define WASAPI_ENDPOINT_BUFFER_HNS  (2000000)   /* 200 ms */

static log_t wasapi_log = LOG_DEFAULT;

static IMMDevice *device = NULL;
static IAudioClient *audio_client = NULL;
static IAudioRenderClient *render_client = NULL;
static IAudioClock *audio_clock = NULL;

/** \brief CoInitializeEx() succeeded and needs a CoUninitialize() */
static int com_initialized = 0;

/** \brief the stream has been started (IAudioClient::Start) */
static int started = 0;

static int num_channels = 2;
static int sample_rate = 48000;

/** \brief frames VICE wants queued at most (its buffer size) */
static UINT32 target_frames = 0;

/** \brief size of the endpoint buffer (frames) */
static UINT32 endpoint_frames = 0;

/** \brief audio engine period (frames) */
static UINT32 period_frames = 0;

/** \brief stream latency reported by WASAPI (frames) */
static UINT32 latency_frames = 0;

/** \brief times the estimate had to be corrected (logged on close) */
static unsigned long corrections_low = 0;
static unsigned long corrections_high = 0;

/** \brief frames given to WASAPI since the stream was opened */
static UINT64 written_frames = 0;

/** \brief audio clock: units per second, and the performance counter frequency */
static UINT64 clock_frequency = 0;
static LARGE_INTEGER qpc_frequency;

/** \brief correction added to the clock position, after an underrun (frames) */
static INT64 position_correction = 0;


static void wasapi_release_all(void)
{
    if (audio_client != NULL && started) {
        IAudioClient_Stop(audio_client);
    }
    started = 0;
    if (audio_clock != NULL) {
        IAudioClock_Release(audio_clock);
        audio_clock = NULL;
    }
    if (render_client != NULL) {
        IAudioRenderClient_Release(render_client);
        render_client = NULL;
    }
    if (audio_client != NULL) {
        IAudioClient_Release(audio_client);
        audio_client = NULL;
    }
    if (device != NULL) {
        IMMDevice_Release(device);
        device = NULL;
    }
    if (com_initialized) {
        CoUninitialize();
        com_initialized = 0;
    }
}


/** \brief Frames given to WASAPI that haven't been played yet */
static UINT32 wasapi_queued_frames(void)
{
    UINT64 position = 0;
    UINT64 qpc_position = 0;
    UINT32 padding = 0;
    INT64 played;
    INT64 queued;
    INT64 low;
    INT64 high;
    LARGE_INTEGER now;

    if (audio_client == NULL || FAILED(IAudioClient_GetCurrentPadding(audio_client, &padding))) {
        return target_frames; /* device gone: report no room, writes are dropped anyway */
    }
    if (!started || audio_clock == NULL || clock_frequency == 0
        || FAILED(IAudioClock_GetPosition(audio_clock, &position, &qpc_position))) {
        return padding;
    }

    /* the play position now: the clock's position plus the time since it was sampled */
    played = (INT64)((double)position * (double)sample_rate / (double)clock_frequency);
    QueryPerformanceCounter(&now);
    if (qpc_position > 0) {
        double sampled_s = (double)qpc_position / 10000000.0;           /* 100 ns units */
        double now_s = (double)now.QuadPart / (double)qpc_frequency.QuadPart;
        double ahead = (now_s - sampled_s) * (double)sample_rate;
        if (ahead > 0.0) {
            /* the clock is updated at least once per period; no further than that */
            if (ahead > (double)period_frames) {
                ahead = (double)period_frames;
            }
            played += (INT64)ahead;
        }
    }
    played += position_correction;

    queued = (INT64)written_frames - played;

    /*
     * Sanity window: the frames not yet played are at least those WASAPI
     * hasn't taken from the buffer (padding), and at most those plus what the
     * engine has taken but not yet played (about two periods). Outside of it
     * (underrun, a device that counts its clock differently), follow the
     * padding.
     */
    low = (INT64)padding;
    high = (INT64)padding + 2 * (INT64)period_frames + (INT64)latency_frames;
    if (queued < low) {
        position_correction -= low - queued;
        queued = low;
        corrections_low++;
    } else if (queued > high) {
        position_correction += queued - high;
        queued = high;
        corrections_high++;
    }

    return (UINT32)queued;
}


static int wasapi_init(const char *param, int *speed, int *fragsize, int *fragnr, int *channels)
{
    IMMDeviceEnumerator *enumerator = NULL;
    WAVEFORMATEX format;
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    HRESULT hr;

    if (wasapi_log == LOG_DEFAULT) {
        wasapi_log = log_open("WASAPI");
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        com_initialized = 1;  /* S_OK or S_FALSE: both need CoUninitialize() */
    } else if (hr != RPC_E_CHANGED_MODE) {
        log_error(wasapi_log, "CoInitializeEx failed (0x%08lx).", (unsigned long)hr);
        return -1;
    }

    hr = CoCreateInstance(&wasapi_clsid_mmdeviceenumerator, NULL, CLSCTX_ALL,
                          &wasapi_iid_immdeviceenumerator, (void **)&enumerator);
    if (FAILED(hr)) {
        log_error(wasapi_log, "no device enumerator (0x%08lx).", (unsigned long)hr);
        wasapi_release_all();
        return -1;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr)) {
        log_error(wasapi_log, "no default playback device (0x%08lx).", (unsigned long)hr);
        wasapi_release_all();
        return -1;
    }

    hr = IMMDevice_Activate(device, &wasapi_iid_iaudioclient, CLSCTX_ALL, NULL, (void **)&audio_client);
    if (FAILED(hr)) {
        log_error(wasapi_log, "cannot activate the audio client (0x%08lx).", (unsigned long)hr);
        wasapi_release_all();
        return -1;
    }

    num_channels = (*channels >= 2) ? 2 : 1;
    sample_rate = *speed;

    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = (WORD)num_channels;
    format.nSamplesPerSec = (DWORD)sample_rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (WORD)(num_channels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    hr = IAudioClient_Initialize(audio_client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 WASAPI_ENDPOINT_BUFFER_HNS, 0, &format, NULL);
    if (FAILED(hr)) {
        log_error(wasapi_log, "cannot open a %d Hz %d channel stream (0x%08lx).",
                  sample_rate, num_channels, (unsigned long)hr);
        wasapi_release_all();
        return -1;
    }

    if (FAILED(IAudioClient_GetBufferSize(audio_client, &endpoint_frames))
        || FAILED(IAudioClient_GetService(audio_client, &wasapi_iid_iaudiorenderclient, (void **)&render_client))
        || FAILED(IAudioClient_GetService(audio_client, &wasapi_iid_iaudioclock, (void **)&audio_clock))
        || FAILED(IAudioClock_GetFrequency(audio_clock, &clock_frequency))) {
        log_error(wasapi_log, "cannot set up the stream.");
        wasapi_release_all();
        return -1;
    }
    QueryPerformanceFrequency(&qpc_frequency);

    if (FAILED(IAudioClient_GetDevicePeriod(audio_client, &default_period, &min_period)) || default_period <= 0) {
        default_period = 100000; /* 10 ms */
    }
    period_frames = (UINT32)((default_period * sample_rate + 5000000) / 10000000);
    {
        REFERENCE_TIME latency = 0;
        if (FAILED(IAudioClient_GetStreamLatency(audio_client, &latency)) || latency < 0) {
            latency = 0;
        }
        latency_frames = (UINT32)((latency * sample_rate + 5000000) / 10000000);
    }
    corrections_low = 0;
    corrections_high = 0;

    /* VICE's buffer, but never more than the endpoint buffer can hold */
    target_frames = (UINT32)(*fragsize * *fragnr);
    if (target_frames > endpoint_frames) {
        target_frames = endpoint_frames;
    }

    written_frames = 0;
    position_correction = 0;
    *channels = num_channels;

    hr = IAudioClient_Start(audio_client);
    if (FAILED(hr)) {
        log_error(wasapi_log, "cannot start the stream (0x%08lx).", (unsigned long)hr);
        wasapi_release_all();
        return -1;
    }
    started = 1;

    log_message(wasapi_log, "shared mode, %d Hz, %d channel(s), engine period %.2f ms, stream latency %.2f ms, endpoint buffer %u frames.",
                sample_rate, num_channels, (double)default_period / 10000.0,
                (double)latency_frames * 1000.0 / (double)sample_rate, (unsigned int)endpoint_frames);
    return 0;
}


static int wasapi_write(int16_t *pbuf, size_t nr)
{
    UINT32 frames = (UINT32)(nr / (size_t)num_channels);
    UINT32 done = 0;

    if (render_client == NULL || frames == 0) {
        return 0;
    }

    while (done < frames) {
        UINT32 padding = 0;
        UINT32 room;
        UINT32 chunk;
        BYTE *data = NULL;

        if (FAILED(IAudioClient_GetCurrentPadding(audio_client, &padding))) {
            return -1;
        }
        room = endpoint_frames - padding;
        if (room == 0) {
            Sleep(1); /* only if VICE wrote far more than its buffer */
            continue;
        }
        chunk = frames - done;
        if (chunk > room) {
            chunk = room;
        }
        if (FAILED(IAudioRenderClient_GetBuffer(render_client, chunk, &data))) {
            return -1;
        }
        memcpy(data, pbuf + (size_t)done * (size_t)num_channels, (size_t)chunk * (size_t)num_channels * sizeof(int16_t));
        IAudioRenderClient_ReleaseBuffer(render_client, chunk, 0);
        done += chunk;
    }

    written_frames += frames;
    return 0;
}


static int wasapi_bufferspace(void)
{
    UINT32 queued = wasapi_queued_frames();

    return (queued >= target_frames) ? 0 : (int)(target_frames - queued);
}


static void wasapi_close(void)
{
    if (audio_client != NULL) {
        log_verbose(wasapi_log, "play position corrected %lu times (underrun) and %lu times (ahead).",
                    corrections_low, corrections_high);
    }
    wasapi_release_all();
    written_frames = 0;
}


/* Pause: stop the stream; what is queued plays on resume */
static int wasapi_suspend(void)
{
    if (audio_client != NULL && started) {
        IAudioClient_Stop(audio_client);
        started = 0;
    }
    return 0;
}


static int wasapi_resume(void)
{
    if (audio_client != NULL && !started) {
        if (SUCCEEDED(IAudioClient_Start(audio_client))) {
            started = 1;
        }
    }
    return 0;
}


static const sound_device_t wasapi_device =
{
    "wasapi",
    wasapi_init,
    wasapi_write,
    NULL,
    NULL,
    wasapi_bufferspace,
    wasapi_close,
    wasapi_suspend,
    wasapi_resume,
    0,
    2,
    true
};

int sound_init_wasapi_device(void)
{
    return sound_register_device(&wasapi_device);
}

#endif /* WINDOWS_COMPILE */
