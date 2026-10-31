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
    float lpf_alpha; // pulse low-pass coefficient (1 = off)
} wq_bgm_track_t;

typedef struct
{
    size_t note;        // current note index
    int32_t left;       // samples remaining in the note
    int32_t total;      // samples of the whole note (for the envelope)
    float freq;         // 0 = rest
    float phase;
    float lpf_state;
} wq_voice_state_t;

// Camp theme: gentle C-major stroll over a I-vi-IV-V bass, triangle waves.
static const wq_note_t WQ_HOME_MELODY[] = {
    {64, 2}, {67, 2}, {72, 2}, {67, 2}, {69, 2}, {67, 2}, {64, 2}, {62, 2},
    {64, 2}, {67, 2}, {72, 2}, {74, 2}, {72, 2}, {67, 2}, {60, 2}, {0, 2},
};
static const wq_note_t WQ_HOME_BASS[] = {
    {48, 2}, {43, 2}, {45, 2}, {52, 2},
    {41, 2}, {48, 2}, {43, 2}, {48, 2},
};

// Battle theme: driving A-minor run, bass on Am/Am/F/G.
static const wq_note_t WQ_BATTLE_MELODY[] = {
    {69, 1}, {69, 1}, {72, 1}, {69, 1}, {76, 1}, {72, 1}, {69, 1}, {67, 1},
    {65, 1}, {65, 1}, {69, 1}, {72, 1}, {71, 1}, {72, 1}, {69, 1}, {0, 1},
};
static const wq_note_t WQ_BATTLE_BASS[] = {
    {45, 2}, {45, 2}, {45, 2}, {45, 2},
    {41, 2}, {41, 2}, {43, 2}, {43, 2},
};

static const wq_bgm_track_t WQ_TRACKS[] = {
    [WQ_BGM_HOME] = {
        .melody = WQ_HOME_MELODY, .melody_count = 16,
        .bass = WQ_HOME_BASS, .bass_count = 8,
        .step_ms = 300, .bass_step_mult = 2,
        .melody_amp = 0.15f, .bass_amp = 0.11f,
        .melody_wave = WQ_WAVE_TRIANGLE, .bass_wave = WQ_WAVE_TRIANGLE,
        .lpf_alpha = 1.0f,
    },
    [WQ_BGM_BATTLE] = {
        .melody = WQ_BATTLE_MELODY, .melody_count = 16,
        .bass = WQ_BATTLE_BASS, .bass_count = 8,
        .step_ms = 160, .bass_step_mult = 2,
        .melody_amp = 0.14f, .bass_amp = 0.10f,
        .melody_wave = WQ_WAVE_PULSE, .bass_wave = WQ_WAVE_TRIANGLE,
        .lpf_alpha = 0.45f,
    },
};

typedef struct
{
    wq_bgm_t track;
    wq_voice_state_t melody;
    wq_voice_state_t bass;
} wq_bgm_state_t;

static QueueHandle_t s_queue;
static esp_codec_dev_handle_t s_speaker;
static volatile wq_bgm_state_t s_bgm = {.track = WQ_BGM_NONE};

static float midi_freq(uint8_t midi)
{
    return 440.0f * powf(2.0f, ((int)midi - 69) / 12.0f);
}

// Start the next note of a voice (loops at the end of the table).
static void voice_next_note(wq_voice_state_t *v, const wq_note_t *notes,
                            size_t count, uint32_t note_samples)
{
    v->note = (v->note + 1) % count;
    const wq_note_t *note = &notes[v->note];
    v->total = (int32_t)note->steps * (int32_t)note_samples;
    v->left = v->total;
    v->freq = note->midi ? midi_freq(note->midi) : 0.0f;
    v->phase = 0.0f;
}

// One sample of a voice with a soft attack/release envelope.
static float voice_sample(wq_voice_state_t *v, const wq_note_t *notes,
                          size_t count, const wq_bgm_track_t *track,
                          wq_wave_t wave, float amp)
{
    if (v->left <= 0) {
        voice_next_note(v, notes, count, track->step_ms * 8); // 1 step = ms*8
    }
    if (--v->left <= 0) {
        voice_next_note(v, notes, count, track->step_ms * 8);
    }
    if (v->freq <= 0.0f) {
        return 0.0f;
    }

    v->phase += v->freq / WQ_AUDIO_SAMPLE_RATE;
    if (v->phase >= 1.0f) {
        v->phase -= 1.0f;
    }

    float v_out;
    if (wave == WQ_WAVE_TRIANGLE) {
        v_out = 4.0f * fabsf(v->phase - 0.5f) - 1.0f; // -1..1 triangle
    } else {
        const float raw = (v->phase < 0.5f) ? 1.0f : -1.0f;
        v->lpf_state += track->lpf_alpha * (raw - v->lpf_state);
        v_out = v->lpf_state;
    }

    // per-note envelope: quick attack, gentle release at the tail
    const float attack = (v->total / 8 > 24) ? 24.0f : (float)(v->total / 8);
    const float release = (float)(v->total / 4);
    const float pos = (float)(v->total - v->left);
    float env = 1.0f;
    if (pos < attack) {
        env = pos / attack;
    } else if (v->left < release) {
        env = (float)v->left / release;
    }
    return v_out * env * amp;
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
