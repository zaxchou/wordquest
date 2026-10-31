// SPDX-License-Identifier: Apache-2.0
//
// WordQuest audio: BSP speaker (ES8311 over I2S) driven by a dedicated
// low-priority renderer task. The renderer mixes a looping two-voice
// chip-style background track (MIDI-style note tables, real-time synthesis
// with per-note envelopes) with one-shot effects, so the GSP UI task never
// blocks on audio.

#include "wq_audio.h"

#if !defined(ESP_PLATFORM)
// Native simulator: no BSP speaker; every call is a no-op.
int wq_audio_init(void)
{
    return 0;
}

void wq_audio_play(wq_audio_effect_t effect)
{
    (void)effect;
}

void wq_audio_bgm(wq_bgm_t bgm)
{
    (void)bgm;
}

#else
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "wq_audio";
#define WQ_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)

#define WQ_AUDIO_SAMPLE_RATE 8000
#define WQ_AUDIO_VOLUME      60
#define WQ_AUDIO_QUEUE_LEN   2
#define WQ_AUDIO_STACK_BYTES 4096
#define WQ_RENDER_CHUNK      320 // samples per write (40 ms at 8 kHz)

static const wq_sfx_t *const WQ_EFFECTS[WQ_AUDIO_COUNT] = {
    &WQ_SFX_CLICK, &WQ_SFX_ATTACK, &WQ_SFX_HURT,
    &WQ_SFX_VICTORY, &WQ_SFX_CHEST, &WQ_SFX_START,
};

// ---------------------------------------------------------------------------
// Background tracks: MIDI-style note tables, two voices (melody + bass).
// Note = MIDI number (0 = rest), steps = length in 16th notes.
// freq(midi) = 440 * 2^((midi-69)/12). Timing runs in samples, not ms.
// The note tables, step lengths, amplitudes and waveforms are the values that
// were tuned by ear earlier; keep them together when editing.
// ---------------------------------------------------------------------------

typedef struct
{
    uint8_t midi;
    uint8_t steps;
} wq_note_t;

typedef enum
{
    WQ_WAVE_TRIANGLE,
    WQ_WAVE_PULSE, // pulse through a one-pole low-pass (tames aliasing)
} wq_wave_t;

typedef struct
{
    const wq_note_t *melody;
    size_t melody_count;
    const wq_note_t *bass;
    size_t bass_count;
    uint16_t step_ms;         // duration of one 16th note (melody)
    uint8_t bass_step_mult;   // bass note length = step_ms * this
    float melody_amp, bass_amp;
    wq_wave_t melody_wave, bass_wave;
} wq_bgm_track_t;

typedef struct
{
    size_t index;    // current note
    size_t samples;  // samples rendered into the current note
    size_t length;   // samples this note lasts
    float phase;     // oscillator phase in [0, 1)
    float state;     // one-pole state for the pulse wave
} wq_voice_state_t;

typedef struct
{
    wq_bgm_t track;
    wq_voice_state_t melody;
    wq_voice_state_t bass;
} wq_bgm_state_t;

// Camp theme: gentle, slow, mostly stepwise motion.
static const wq_note_t WQ_HOME_MELODY[] = {
    {64, 2}, {67, 2}, {72, 2}, {67, 2}, {69, 2}, {67, 2}, {64, 2}, {62, 2},
    {64, 2}, {67, 2}, {72, 2}, {74, 2}, {72, 2}, {67, 2}, {60, 2}, {0, 2},
};
static const wq_note_t WQ_HOME_BASS[] = {
    {48, 2}, {43, 2}, {45, 2}, {52, 2}, {41, 2}, {48, 2}, {43, 2}, {48, 2},
};
// Battle theme: faster, denser, pulse lead over a driving bass.
static const wq_note_t WQ_BATTLE_MELODY[] = {
    {69, 1}, {69, 1}, {72, 1}, {69, 1}, {76, 1}, {72, 1}, {69, 1}, {67, 1},
    {65, 1}, {65, 1}, {69, 1}, {72, 1}, {71, 1}, {72, 1}, {69, 1}, {0, 1},
};
static const wq_note_t WQ_BATTLE_BASS[] = {
    {45, 2}, {45, 2}, {45, 2}, {45, 2}, {41, 2}, {41, 2}, {43, 2}, {43, 2},
};

static const wq_bgm_track_t WQ_TRACKS[] = {
    [WQ_BGM_NONE] = {0},
    [WQ_BGM_HOME] = {
        WQ_HOME_MELODY, sizeof(WQ_HOME_MELODY) / sizeof(WQ_HOME_MELODY[0]),
        WQ_HOME_BASS, sizeof(WQ_HOME_BASS) / sizeof(WQ_HOME_BASS[0]),
        300, 2, 0.15f, 0.11f, WQ_WAVE_TRIANGLE, WQ_WAVE_TRIANGLE,
    },
    [WQ_BGM_BATTLE] = {
        WQ_BATTLE_MELODY, sizeof(WQ_BATTLE_MELODY) / sizeof(WQ_BATTLE_MELODY[0]),
        WQ_BATTLE_BASS, sizeof(WQ_BATTLE_BASS) / sizeof(WQ_BATTLE_BASS[0]),
        160, 2, 0.14f, 0.10f, WQ_WAVE_PULSE, WQ_WAVE_TRIANGLE,
    },
};

static esp_codec_dev_handle_t s_speaker;
static QueueHandle_t s_queue;
static wq_bgm_state_t s_bgm;

static float midi_freq(uint8_t midi)
{
    return 440.0f * powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

static size_t note_samples(const wq_bgm_track_t *track, const wq_note_t *note,
                           bool bass)
{
    const unsigned ms = bass ? (unsigned)track->step_ms * track->bass_step_mult
                             : (unsigned)track->step_ms;
    size_t n = (size_t)note->steps * ms * WQ_AUDIO_SAMPLE_RATE / 1000;
    return n < 32 ? 32 : n;
}

// One sample of a note: triangle or low-passed pulse, with a short attack and a
// release that ends before the note does, so repeats stay articulated.
static float voice_sample(wq_voice_state_t *v, const wq_note_t *notes,
                          size_t count, const wq_bgm_track_t *track,
                          wq_wave_t wave, float amp)
{
    if (count == 0) return 0.0f;
    if (v->length == 0 || v->samples >= v->length) {
        if (v->length != 0) v->index = (v->index + 1) % count;
        v->samples = 0;
        v->length = note_samples(track, &notes[v->index], notes == track->bass);
    }
    const wq_note_t *note = &notes[v->index];

    float out = 0.0f;
    if (note->midi != 0) {
        v->phase += midi_freq(note->midi) / (float)WQ_AUDIO_SAMPLE_RATE;
        if (v->phase >= 1.0f) v->phase -= 1.0f;

        float raw;
        if (wave == WQ_WAVE_TRIANGLE) {
            const float t = v->phase;
            raw = 4.0f * (t < 0.5f ? t : 1.0f - t) - 1.0f;
        } else {
            raw = v->phase < 0.5f ? 1.0f : -1.0f;
            v->state += (raw - v->state) * 0.18f; // one-pole low-pass
            raw = v->state * 2.0f;
        }

        // Envelope: 12 ms attack, release over the last 30% of the note.
        const size_t attack = WQ_AUDIO_SAMPLE_RATE * 12 / 1000;
        const size_t release = v->length * 3 / 10;
        float env = 1.0f;
        if (v->samples < attack) {
            env = (float)v->samples / (float)attack;
        } else if (v->length > release && v->samples + release >= v->length) {
            env = (float)(v->length - v->samples) / (float)release;
            if (env > 1.0f) env = 1.0f;
        }
        out = raw * env * amp;
    }
    ++v->samples;
    return out;
}

static void player_task(void *arg)
{
    static int16_t mix[WQ_RENDER_CHUNK];
    wq_audio_effect_t effect;
    size_t sfx_pos = 0;
    const wq_sfx_t *sfx = NULL;

    for (;;) {
        // pick up newest queued effect; drop stale backlog
        while (s_queue && xQueueReceive(s_queue, &effect, 0) == pdTRUE) {
            if (effect < WQ_AUDIO_COUNT) {
                sfx = WQ_EFFECTS[effect];
                sfx_pos = 0;
            }
        }

        wq_bgm_state_t bgm = s_bgm;
        for (size_t i = 0; i < WQ_RENDER_CHUNK; ++i) {
            float mix_v = 0.0f;
            if (bgm.track != WQ_BGM_NONE) {
                const wq_bgm_track_t *track = &WQ_TRACKS[bgm.track];
                mix_v = voice_sample(&bgm.melody, track->melody,
                                     track->melody_count, track,
                                     track->melody_wave, track->melody_amp);
                mix_v += voice_sample(&bgm.bass, track->bass,
                                      track->bass_count, track,
                                      track->bass_wave, track->bass_amp);
            }
            if (sfx) {
                mix_v += sfx->pcm[sfx_pos] / 32767.0f * 1.1f;
                if (++sfx_pos >= sfx->samples) {
                    sfx = NULL;
                    sfx_pos = 0;
                }
            }
            mix[i] = (int16_t)(mix_v * 32767.0f);
        }
        s_bgm = bgm;

        if (s_speaker) {
            esp_codec_dev_write(s_speaker, mix, sizeof(mix));
        } else {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}

int wq_audio_init(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        WQ_LOGE("speaker init failed; sound disabled");
        return -1;
    }
    esp_codec_dev_set_out_vol(s_speaker, WQ_AUDIO_VOLUME);
    esp_codec_dev_sample_info_t info = {
        .sample_rate = WQ_AUDIO_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    esp_err_t err = esp_codec_dev_open(s_speaker, &info);
    if (err != ESP_OK) {
        WQ_LOGE("speaker open failed (0x%x); sound disabled", err);
        s_speaker = NULL;
        return -1;
    }

    s_queue = xQueueCreate(WQ_AUDIO_QUEUE_LEN, sizeof(wq_audio_effect_t));
    if (!s_queue || xTaskCreate(player_task, "wq_audio", WQ_AUDIO_STACK_BYTES,
                                NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        WQ_LOGE("audio player task failed; sound disabled");
        return -1;
    }
    return 0;
}

void wq_audio_play(wq_audio_effect_t effect)
{
    if (!s_queue || effect >= WQ_AUDIO_COUNT) {
        return;
    }
    // Best effort: drop when the player is already two effects behind.
    (void)xQueueSend(s_queue, &effect, 0);
}

void wq_audio_bgm(wq_bgm_t bgm)
{
    wq_voice_state_t reset = {0};
    s_bgm.track = bgm;
    s_bgm.melody = reset;
    s_bgm.bass = reset;
}
#endif
