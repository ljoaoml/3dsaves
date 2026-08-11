#include "sound.h"

#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Not verified against a real compiler (no devkitARM in the sandbox this
// was written in -- see README/CLAUDE notes on that constraint) -- the
// ndspWaveBuf field names/shape here match every published libctru audio
// example (3ds-examples' audio/sfx, ctrmus, ...): a union data pointer +
// nsamples + looping are the only fields callers set, everything else is
// zeroed and left for ndsp itself to manage. If a build ever reports a
// field mismatch here, that's the first thing to check.
#define SOUND_SAMPLE_RATE 22050
#define SOUND_CHANNEL 0

static bool s_soundInited = false;
static s16 *s_successBuf = NULL;
static s16 *s_errorBuf = NULL;
static ndspWaveBuf s_successWaveBuf;
static ndspWaveBuf s_errorWaveBuf;

// Fills `buf` with `numSamples` of a sine tone at `freqHz`, faded in and
// out over the first/last eighth of the buffer so the tone doesn't click
// at its start/end (an abrupt sample discontinuity is audible as a pop).
static void fill_tone(s16 *buf, u32 numSamples, float freqHz, float amplitude) {
    u32 fadeSamples = numSamples / 8 > 0 ? numSamples / 8 : 1;
    for (u32 i = 0; i < numSamples; i++) {
        float t = (float)i / (float)SOUND_SAMPLE_RATE;
        float env = 1.0f;
        if (i < fadeSamples) env = (float)i / (float)fadeSamples;
        else if (i > numSamples - fadeSamples) env = (float)(numSamples - i) / (float)fadeSamples;
        buf[i] = (s16)(amplitude * env * sinf(2.0f * (float)M_PI * freqHz * t) * 30000.0f);
    }
}

// Allocates (in linear memory -- required for the DSP's own DMA to read
// it), generates, and flushes-to-cache one tone, ready to be queued with
// ndspChnWaveBufAdd(). Returns false (leaving *bufOut untouched) on
// allocation failure -- sound_init() treats that as "no sound" overall
// rather than half-working.
static bool setup_tone(ndspWaveBuf *wb, s16 **bufOut, float freqHz, float durationSec, float amplitude) {
    u32 numSamples = (u32)(SOUND_SAMPLE_RATE * durationSec);
    s16 *buf = (s16 *)linearAlloc(numSamples * sizeof(s16));
    if (!buf) return false;

    fill_tone(buf, numSamples, freqHz, amplitude);
    DSP_FlushDataCache(buf, numSamples * sizeof(s16));

    memset(wb, 0, sizeof(*wb));
    wb->data_vaddr = buf;
    wb->nsamples = numSamples;
    wb->looping = false;

    *bufOut = buf;
    return true;
}

void sound_init(void) {
    Result rc = ndspInit();
    if (R_FAILED(rc)) return;

    ndspChnReset(SOUND_CHANNEL);
    ndspChnSetInterp(SOUND_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(SOUND_CHANNEL, SOUND_SAMPLE_RATE);
    ndspChnSetFormat(SOUND_CHANNEL, NDSP_FORMAT_MONO_PCM16);

    // A short bright two-tone-ish rising beep would need two buffers
    // queued back to back; a single clean tone reads as "confirm" just
    // fine and keeps this simple. Error is lower-pitched and a bit
    // longer, reading as distinctly negative without being harsh.
    bool ok1 = setup_tone(&s_successWaveBuf, &s_successBuf, 880.0f, 0.10f, 0.5f);
    bool ok2 = setup_tone(&s_errorWaveBuf, &s_errorBuf, 220.0f, 0.18f, 0.6f);

    s_soundInited = ok1 && ok2;
    if (!s_soundInited) {
        if (s_successBuf) { linearFree(s_successBuf); s_successBuf = NULL; }
        if (s_errorBuf) { linearFree(s_errorBuf); s_errorBuf = NULL; }
        ndspExit();
    }
}

void sound_exit(void) {
    if (!s_soundInited) return;
    ndspChnWaveBufClear(SOUND_CHANNEL);
    if (s_successBuf) linearFree(s_successBuf);
    if (s_errorBuf) linearFree(s_errorBuf);
    ndspExit();
    s_soundInited = false;
}

static void play(ndspWaveBuf *wb) {
    if (!s_soundInited) return;
    // Re-queuing a buffer that's still playing is harmless (ndsp just
    // restarts it) -- no need to track "is this one already playing".
    ndspChnWaveBufAdd(SOUND_CHANNEL, wb);
}

void sound_play_success(void) { play(&s_successWaveBuf); }
void sound_play_error(void) { play(&s_errorWaveBuf); }
