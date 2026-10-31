// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_gsp.h"

// WordQuest UI state shared with the platform entry point. Everything here
// is owned by wordquest_ui.c; the platform layer only forwards the handle.
// Persisted values (level, streak, quota, book) live in wq_settings instead.
typedef struct {
    void *timer;          // 100 ms animator stepper
    // Sprite animator (a single 100 ms timer drives all of it).
    uint8_t page;         // wq_page_t; gates input and background animation
    uint8_t mode;         // wq_anim_mode_t
    uint8_t seq;          // wq_anim_seq_t, valid while MODE_SEQ
    uint8_t step;         // current step index in the active sequence
    uint8_t step_left;    // ticks remaining in the current step
    uint8_t idle_tick;    // free-running idle phase
    uint8_t chest_left;   // ticks until the reward chest opens
} wordquest_ui_t;

esp_gsp_err_t wordquest_ui_init(esp_gsp_handle_t ui, wordquest_ui_t *state);
void wordquest_ui_deinit(esp_gsp_handle_t ui, wordquest_ui_t *state);
