// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "wq_sfx.h"

// WordQuest audio: one-shot retro SFX plus looping chip-style background
// music (MIDI-style note tables rendered in real time). Effects render on a
// dedicated low-priority task fed by a small queue; callers never block.
// Stubbed out on the native simulator (no BSP there).

typedef enum {
    WQ_AUDIO_CLICK = 0,
    WQ_AUDIO_ATTACK,
    WQ_AUDIO_HURT,
    WQ_AUDIO_VICTORY,
    WQ_AUDIO_CHEST,
    WQ_AUDIO_START,
    WQ_AUDIO_COUNT,
} wq_audio_effect_t;

typedef enum {
    WQ_BGM_NONE = 0, // silence (victory fanfare only)
    WQ_BGM_HOME,     // camp theme: calm, slow
    WQ_BGM_BATTLE,   // battle theme: driving, fast
} wq_bgm_t;

// Initialize the speaker chain, player task and BGM renderer. Safe to call
// once; returns 0 on success. On the simulator this is a no-op.
int wq_audio_init(void);

// Queue an SFX for playback. Drops the request if the player is busy
// with two queued effects (never blocks the UI).
void wq_audio_play(wq_audio_effect_t effect);

// Switch the looping background track; takes effect at the next render
// chunk (~40 ms). WQ_BGM_NONE silences the music.
void wq_audio_bgm(wq_bgm_t bgm);

