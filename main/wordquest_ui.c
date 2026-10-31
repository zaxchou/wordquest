// SPDX-License-Identifier: Apache-2.0
//
// WordQuest UI: GSP scene wiring for the daily boss-battle word session.
// The session itself lives in wq_model.c; this file renders state, feeds
// input back into the model and drives the sprite animation system.
//
// Animation model: one persistent 100 ms timer steps a tiny state machine
// (idle / sequence / delay / chest). Sprite frames are stacked image widgets
// toggled through their generated visible setters; every visual change goes
// through apply_frame() so a frame is fully described by one struct.

#include "wordquest_ui.h"

#include "wq_audio.h"

#include <stdio.h>
#include <string.h>

#include "bundle_gsp.h"
#include "wq_dict.h"
#include "wq_progress.h"
#include "wq_model.h"

#if !defined(ESP_PLATFORM)
// Native simulator build: no ESP-IDF logging, timers or NVS. Logging falls
// back to stderr, NVS persistence becomes a per-process stub and the clock
// comes from libc.
#include <time.h>
static const char *TAG = "wq_ui";
#define WQ_LOGE(...) fprintf(stderr, "E wq_ui: "), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
static int nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    (void)ns;
    (void)mode;
    *out = 1;
    return ESP_OK;
}
static int nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    (void)key;
    *out = 0;
    return -1; // simulator always starts a fresh session
}
static int nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    (void)h;
    (void)key;
    (void)value;
    return ESP_OK;
}
static int nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h)
{
    (void)h;
}
static int64_t sim_time_us(void)
{
    return (int64_t)time(NULL) * 1000000;
}
#define esp_timer_get_time sim_time_us
#else
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
static const char *TAG = "wq_ui";
#define WQ_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#endif

#define TICK_MS 100
#define IDLE_FRAME_TICKS 6       // idle pose holds 600 ms
#define CHEST_OPEN_TICKS 5       // chest stays closed 500 ms on the win page
#define FEEDBACK_IDLE "选择正确的意思，攻击 Boss！"
#define NVS_NAMESPACE "wordquest"

// The session is a module-owned singleton: GSP delivers events from a single
// UI task, so no extra locking is required (documented single-owner design).
static wq_session_t s_session;
static wq_task_t s_tasks[WQ_NEW_WORDS_PER_DAY * 5];
static char s_option_text[WQ_OPTIONS][80];
static size_t s_new_words[WQ_NEW_WORDS_PER_DAY];
static size_t s_new_count;
static bool s_music_on = true;
static const wq_word_entry_t *s_pack;
static size_t s_count;

// ---------------------------------------------------------------------------
// Sprite animator
// ---------------------------------------------------------------------------

typedef enum { MODE_IDLE = 0, MODE_SEQ, MODE_CHEST } wq_anim_mode_t;

// One animation frame: sprite indices per layer (0 = layer hidden) plus the
// damage-popup flag and how many 100 ms ticks to hold the frame.
typedef struct {
    uint8_t player; // player_1..7
    uint8_t boss;   // boss_1..5
    uint8_t slash;  // slash_1..3 (over the boss)
    uint8_t star;   // star_1..2 (over the boss)
    uint8_t beam;   // beam_1..2
    uint8_t pstar;  // star_1..2 (over the player: boss counterattack)
    uint8_t dmg;
    uint8_t ticks;
} wq_frame_t;

typedef enum { WQ_SEQ_ATTACK = 0, WQ_SEQ_DEFEAT, WQ_SEQ_BOSS_ATTACK } wq_anim_seq_t;

// Correct answer: raise sword, slash with boss hit flash, recover.
static const wq_frame_t SEQ_ATTACK[] = {
    {3, 1, 0, 0, 0, 0, 0, 2},
    {4, 3, 1, 0, 0, 0, 0, 2},
    {4, 3, 1, 0, 0, 0, 1, 2},
    {4, 3, 1, 0, 0, 0, 1, 2},
    {5, 2, 0, 0, 0, 0, 1, 1},
};
// Final blow: slash, boss kneels and collapses, adventurer victory poses in
// the level-up light beam, then the win overlay takes over.
static const wq_frame_t SEQ_DEFEAT[] = {
    {3, 1, 0, 0, 0, 0, 0, 2},
    {4, 3, 1, 0, 0, 0, 0, 2},
    {4, 3, 1, 0, 0, 0, 1, 2},
    {5, 4, 0, 0, 0, 0, 1, 4},
    {6, 5, 0, 0, 0, 0, 0, 4},
    {6, 5, 0, 0, 0, 0, 0, 3},
    {7, 5, 0, 0, 0, 0, 0, 3},
};
// Wrong answer: the boss strikes back — arms raise, a slash lands on the
// adventurer, then both return to idle. Pure feedback: the re-queue of the
// missed word stays the only mechanical punishment.
static const wq_frame_t SEQ_BOSS_ATTACK[] = {
    {1, 3, 0, 0, 0, 0, 0, 2},
    {1, 3, 0, 0, 0, 1, 0, 3},
    {1, 3, 0, 0, 0, 2, 0, 2},
    {1, 1, 0, 0, 0, 0, 0, 2},
};

static void remember(esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        WQ_LOGE("GSP call failed: 0x%x", (unsigned)err);
    }
}

// Text setters enqueue onto the GSP UI task and can report TIMEOUT when the
// queue is momentarily full (observed during the init burst on device).
// Yield briefly and retry instead of silently dropping the update.
#if !defined(ESP_PLATFORM)
#include <time.h>
static void wq_yield_10ms(void)
{
    struct timespec ts = {0, 10 * 1000 * 1000};
    nanosleep(&ts, NULL);
}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void wq_yield_10ms(void)
{
    vTaskDelay(pdMS_TO_TICKS(10));
}
#endif

static void set_text_retry(esp_gsp_handle_t ui,
                           esp_gsp_err_t (*fn)(esp_gsp_handle_t, const char *),
                           const char *text)
{
    esp_gsp_err_t err = fn(ui, text);
    for (int i = 0; err == ESP_GSP_ERR_TIMEOUT && i < 5; ++i) {
        wq_yield_10ms();
        err = fn(ui, text);
    }
    remember(err);
}

static void apply_frame(esp_gsp_handle_t ui, const wq_frame_t *f)
{
    static const struct
    {
        esp_gsp_err_t (*set)(esp_gsp_handle_t, bool);
    } layers[4][8] = {
        // player: 7 poses
        {{gsp_wq_player_1_set_visible}, {gsp_wq_player_2_set_visible}, {gsp_wq_player_3_set_visible},
         {gsp_wq_player_4_set_visible}, {gsp_wq_player_5_set_visible}, {gsp_wq_player_6_set_visible},
         {gsp_wq_player_7_set_visible}},
        // boss: 5 frames
        {{gsp_wq_boss_1_set_visible}, {gsp_wq_boss_2_set_visible}, {gsp_wq_boss_3_set_visible},
         {gsp_wq_boss_4_set_visible}, {gsp_wq_boss_5_set_visible}},
        // slash: single wide arc shared by hits and the boss counterattack
        {{gsp_wq_slash_1_set_visible}},
        // slash over the player (boss counterattack): 2 frames
        {{gsp_wq_pstar_1_set_visible}, {gsp_wq_pstar_1_set_visible}},
    };
    static const uint8_t counts[4] = {7, 5, 1, 2};
    const uint8_t want[4] = {f->player, f->boss, f->slash, f->pstar};

    for (uint8_t layer = 0; layer < 4; ++layer) {
        for (uint8_t i = 0; i < counts[layer]; ++i) {
            remember(layers[layer][i].set(ui, want[layer] == i + 1));
        }
    }
    remember(gsp_wq_dmg_set_visible(ui, f->dmg != 0));
}

static const wq_frame_t *seq_frames(wq_anim_seq_t seq, size_t *len)
{
    switch (seq) {
    case WQ_SEQ_DEFEAT:
        *len = sizeof(SEQ_DEFEAT) / sizeof(SEQ_DEFEAT[0]);
        return SEQ_DEFEAT;
    case WQ_SEQ_BOSS_ATTACK:
        *len = sizeof(SEQ_BOSS_ATTACK) / sizeof(SEQ_BOSS_ATTACK[0]);
        return SEQ_BOSS_ATTACK;
    case WQ_SEQ_ATTACK:
    default:
        *len = sizeof(SEQ_ATTACK) / sizeof(SEQ_ATTACK[0]);
        return SEQ_ATTACK;
    }
}

static void apply_idle(wordquest_ui_t *state, esp_gsp_handle_t ui)
{
    const uint8_t pose = (uint8_t)(1 + (state->idle_tick / IDLE_FRAME_TICKS) % 2);
    const wq_frame_t idle = {pose, pose, 0, 0, 0, 0, 0, 0};
    apply_frame(ui, &idle);
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// Truncate a UTF-8 string to at most max_chars characters (never splitting a
// multi-byte sequence), appending an ellipsis when anything was cut.
static void trunc_utf8(char *out, size_t cap, const char *src, size_t max_chars)
{
    size_t used = 0;
    size_t chars = 0;
    const char *p = src;
    while (*p && chars < max_chars) {
        size_t len = ((*p & 0x80U) == 0) ? 1U : (((*p & 0xE0U) == 0xC0U) ? 2U : (((*p & 0xF0U) == 0xE0U) ? 3U : 4U));
        if (used + len + 4U >= cap) {
            break;
        }
        for (size_t k = 0; k < len; ++k) {
            out[used + k] = p[k];
        }
        used += len;
        p += len;
        ++chars;
    }
    if (*p) {
        out[used + 0] = '.';
        out[used + 1] = '.';
        out[used + 2] = '.';
        used += 3;
    }
    out[used] = '\0';
}

static void refresh_hp(esp_gsp_handle_t ui)
{
    char text[24];
    snprintf(text, sizeof(text), "HP %d/%d", wq_session_hp(&s_session),
             wq_session_hp_max(&s_session));
    remember(gsp_wq_hp_bar_set_value(ui, wq_session_hp(&s_session)));
    set_text_retry(ui, gsp_wq_hp_text_set_text, text);
}

static void refresh_day(esp_gsp_handle_t ui)
{
    char text[32];
    snprintf(text, sizeof(text), "今日 %u/%u",
             (unsigned)wq_session_done_count(&s_session),
             (unsigned)s_session.task_count);
    set_text_retry(ui, gsp_wq_day_text_set_text, text);
}


// Non-button children of hidden overlays still swallow touches (only button
// widgets are exempt in the runtime hit-test), so each page switch toggles
// every child's visibility explicitly through its bind setter.
typedef esp_gsp_err_t (*wq_vis_fn)(esp_gsp_handle_t, bool);

static const wq_vis_fn WQ_HOME_CHILDREN[] = {
    gsp_wq_home_bg_set_visible,
};

static const wq_vis_fn WQ_LEARN_CHILDREN[] = {};

static const wq_vis_fn WQ_WIN_CHILDREN[] = {
    gsp_wq_win_bg_set_visible,
};

static void set_group_visible(const wq_vis_fn *fns, size_t n,
                              esp_gsp_handle_t ui, bool on)
{
    for (size_t i = 0; i < n; ++i) {
        remember(fns[i](ui, on));
    }
}

static void set_home_page(esp_gsp_handle_t ui, bool on)
{
    set_group_visible(WQ_HOME_CHILDREN, sizeof(WQ_HOME_CHILDREN) / sizeof(WQ_HOME_CHILDREN[0]), ui, on);
}

static void set_learn_page(esp_gsp_handle_t ui, bool on)
{
    set_group_visible(WQ_LEARN_CHILDREN, sizeof(WQ_LEARN_CHILDREN) / sizeof(WQ_LEARN_CHILDREN[0]), ui, on);
}

static void set_win_page(esp_gsp_handle_t ui, bool on)
{
    set_group_visible(WQ_WIN_CHILDREN, sizeof(WQ_WIN_CHILDREN) / sizeof(WQ_WIN_CHILDREN[0]), ui, on);
}

static void set_feedback(esp_gsp_handle_t ui, const char *text)
{
    set_text_retry(ui, gsp_wq_feedback_set_text, text);
}



static void show_illustration(esp_gsp_handle_t ui, const char *root)
{
    remember(gsp_wq_illo_fix_set_visible(ui, strcmp(root, "fix") == 0));
    remember(gsp_wq_illo_ride_set_visible(ui, strcmp(root, "ride") == 0));
    remember(gsp_wq_illo_err_set_visible(ui, strcmp(root, "err") == 0));
}

// Replace typographic quotes with ASCII so the English example fits the
// label's baked charset (the CJK font corpus already carries the originals).
// The sequences below are the UTF-8 encodings of U+201C/U+201D/U+2019.
static void sanitize_quotes(char *text)
{
    const char *p = text;
    char *w = text;
    while (*p) {
        const unsigned char c0 = (unsigned char)p[0];
        const unsigned char c1 = (unsigned char)p[1];
        const unsigned char c2 = (unsigned char)p[2];
        if (c0 == 0xE2 && c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D)) {
            *w++ = '"';
            p += 3;
        } else if (c0 == 0xE2 && c1 == 0x80 && c2 == 0x99) {
            *w++ = '\'';
            p += 3;
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
}

static void show_learn(esp_gsp_handle_t ui, const wq_word_entry_t *word)
{
    char text[96];
    trunc_utf8(text, sizeof(text), word->meaning, 20);
    set_text_retry(ui, gsp_wq_learn_word_set_text, word->word);
    set_text_retry(ui, gsp_wq_learn_ipa_set_text, word->ipa);
    set_text_retry(ui, gsp_wq_learn_meaning_set_text, text);
    trunc_utf8(text, sizeof(text), word->example_en, 42);
    sanitize_quotes(text);
    set_text_retry(ui, gsp_wq_learn_ex_en_set_text, text);
    trunc_utf8(text, sizeof(text), word->example_zh, 26);
    set_text_retry(ui, gsp_wq_learn_ex_zh_set_text, text);
    show_illustration(ui, word->root);

    remember(gsp_wq_learn_overlay_set_visible(ui, true));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    set_learn_page(ui, true);
    set_win_page(ui, false);
}

static void show_quiz(esp_gsp_handle_t ui, const wq_word_entry_t *word)
{
    size_t slots[WQ_OPTIONS];

    wq_session_build_options(&s_session, slots);
    for (size_t i = 0; i < WQ_OPTIONS; ++i) {
        trunc_utf8(s_option_text[i], sizeof(s_option_text[i]),
                   WQ_WORD_PACK[slots[i]].gloss, 12);
    }
    set_text_retry(ui, gsp_wq_word_label_set_text, word->word);
    set_text_retry(ui, gsp_wq_ipa_label_set_text, word->ipa);
    set_text_retry(ui, gsp_wq_opt_lbl_0_set_text, s_option_text[0]);
    set_text_retry(ui, gsp_wq_opt_lbl_1_set_text, s_option_text[1]);
    set_text_retry(ui, gsp_wq_opt_lbl_2_set_text, s_option_text[2]);
    set_text_retry(ui, gsp_wq_opt_lbl_3_set_text, s_option_text[3]);
    set_feedback(ui, FEEDBACK_IDLE);
    refresh_hp(ui);
    refresh_day(ui);

    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    set_learn_page(ui, false);
    set_win_page(ui, false);
}

static void show_win(esp_gsp_handle_t ui, wordquest_ui_t *state);

static void show_task(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    const wq_task_t *task = wq_session_current(&s_session);
    if (!task) {
        show_win(ui, state);
        return;
    }
    const wq_word_entry_t *word = &WQ_WORD_PACK[task->word];
    if (task->kind == WQ_TASK_LEARN) {
        show_learn(ui, word);
    } else {
        show_quiz(ui, word);
    }
}

// Camp / profile page: the hub the day starts and ends on.
static void show_home(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    char text[48];
    snprintf(text, sizeof(text), "Lv.%d · %d 天", (int)state->level,
             (int)state->level - 1);
    set_text_retry(ui, gsp_wq_home_lv_set_text, text);
    snprintf(text, sizeof(text), "徽章 x%u", (unsigned)(state->level - 1));
    set_text_retry(ui, gsp_wq_home_badge_n_set_text, text);
    snprintf(text, sizeof(text), "词书: %s", wq_dict_book_name(wq_progress_book()));
    set_text_retry(ui, gsp_wq_home_days_set_text, text);
    snprintf(text, sizeof(text), "已斩 %u/%u",
             (unsigned)wq_progress_learned(), (unsigned)wq_progress_total());
    set_text_retry(ui, gsp_wq_home_learned_set_text, text);
    snprintf(text, sizeof(text), "待复习 %u",
             (unsigned)wq_progress_review_due(WQ_NEW_WORDS_PER_DAY));
    set_text_retry(ui, gsp_wq_home_review_set_text, text);
    wq_audio_bgm(s_music_on ? WQ_BGM_HOME : WQ_BGM_NONE);
    set_text_retry(ui, gsp_wq_music_lbl_set_text,
                   s_music_on ? "音乐:开" : "音乐:关");

    remember(gsp_wq_home_overlay_set_visible(ui, true));
    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    set_home_page(ui, true);
    set_learn_page(ui, false);
    set_win_page(ui, false);
    wq_audio_bgm(WQ_BGM_HOME);
    state->mode = MODE_IDLE;
}

static void show_win(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    const int previous = (int)state->level;
    ++state->level;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, "level", (uint32_t)state->level);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    char text[48];
    snprintf(text, sizeof(text), "冒险等级 Lv.%d → Lv.%d", previous,
             (int)state->level);
    set_text_retry(ui, gsp_wq_win_level_set_text, text);
    snprintf(text, sizeof(text), "第 %d 关", (int)state->level);
    set_text_retry(ui, gsp_wq_lvl_text_set_text, text);
    snprintf(text, sizeof(text), "今日任务 %u · 答对 %u",
             (unsigned)s_session.task_count,
             (unsigned)wq_session_correct_count(&s_session));
    set_text_retry(ui, gsp_wq_win_sub_set_text, text);
    (void)state;
    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, true));
    set_learn_page(ui, false);
    set_win_page(ui, true);
    wq_audio_bgm(WQ_BGM_NONE);
    wq_audio_play(WQ_AUDIO_VICTORY);
    // reward moment: the chest is painted open in the victory art; only the
    // badge pops into it after a short delay
    remember(gsp_wq_badge_forest_set_visible(ui, false));
    state->mode = MODE_CHEST;
    state->chest_left = CHEST_OPEN_TICKS;
}

// ---------------------------------------------------------------------------
// Animation stepping
// ---------------------------------------------------------------------------

static void finish_sequence(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    state->mode = MODE_IDLE;
    state->idle_tick = 0;
    apply_idle(state, ui);
    if (state->seq == WQ_SEQ_DEFEAT) {
        show_win(ui, state);
    } else {
        (void)wq_session_advance(&s_session);
        show_task(ui, state);
    }
}

static void stepper_tick(esp_gsp_handle_t ui, void *ctx)
{
    wordquest_ui_t *state = ctx;

    switch (state->mode) {
    case MODE_SEQ: {
        size_t len;
        const wq_frame_t *seq = seq_frames(state->seq, &len);
        if (state->step_left > 0) {
            --state->step_left;
        }
        if (state->step_left > 0) {
            return; // hold the current frame
        }
        ++state->step;
        if (state->step >= len) {
            finish_sequence(ui, state);
            return;
        }
        apply_frame(ui, &seq[state->step]);
        state->step_left = seq[state->step].ticks;
        return;
    }
    case MODE_CHEST:
        if (--state->chest_left == 0) {
            remember(gsp_wq_badge_forest_set_visible(ui, true));
            wq_audio_play(WQ_AUDIO_CHEST);
            state->mode = MODE_IDLE;
        }
        return;
    case MODE_IDLE:
    default: {
        const uint8_t phase = (uint8_t)(state->idle_tick / IDLE_FRAME_TICKS);
        ++state->idle_tick;
        if (phase != (uint8_t)(state->idle_tick / IDLE_FRAME_TICKS)) {
            apply_idle(state, ui); // breathing pose flip
        }
        return;
    }
    }
}

static void start_sequence(esp_gsp_handle_t ui, wordquest_ui_t *state,
                           wq_anim_seq_t seq)
{
    size_t len;
    const wq_frame_t *frames = seq_frames(seq, &len);
    (void)len;
    state->mode = MODE_SEQ;
    state->seq = seq;
    state->step = 0;
    state->step_left = frames[0].ticks;
    apply_frame(ui, &frames[0]);
    if (!state->timer) {
        // No animator available: skip straight to the outcome.
        finish_sequence(ui, state);
    }
}

// ---------------------------------------------------------------------------
// Session flow
// ---------------------------------------------------------------------------

static void restart_session(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    s_new_count = wq_progress_pick_new(WQ_NEW_WORDS_PER_DAY, s_new_words);
    size_t review[WQ_NEW_WORDS_PER_DAY * 2];
    size_t review_count =
        wq_progress_pick_review(wq_progress_review_due(WQ_NEW_WORDS_PER_DAY),
                                review);
    wq_session_start(&s_session, s_pack, s_count, s_new_words, s_new_count,
                     review, review_count, s_tasks,
                     sizeof(s_tasks) / sizeof(s_tasks[0]),
                     (uint32_t)esp_timer_get_time());
    char text[32];
    snprintf(text, sizeof(text), "第 %d 关", (int)state->level);
    set_text_retry(ui, gsp_wq_lvl_text_set_text, text);
    wq_audio_bgm(s_music_on ? WQ_BGM_BATTLE : WQ_BGM_NONE);
    state->mode = MODE_IDLE;
    state->idle_tick = 0;
    apply_idle(state, ui);
    show_task(ui, state);
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event,
                     void *ctx)
{
    wordquest_ui_t *state = ctx;
    const wq_task_t *task = wq_session_current(&s_session);
    bool scored = false;
    bool correct = false;

    if (gsp_wq_event_is_opt_0(event)) {
        scored = true;
        correct = wq_session_answer(&s_session, 0);
    } else if (gsp_wq_event_is_opt_1(event)) {
        scored = true;
        correct = wq_session_answer(&s_session, 1);
    } else if (gsp_wq_event_is_opt_2(event)) {
        scored = true;
        correct = wq_session_answer(&s_session, 2);
    } else if (gsp_wq_event_is_opt_3(event)) {
        scored = true;
        correct = wq_session_answer(&s_session, 3);
    } else if (gsp_wq_event_is_idk(event)) {
        if (task && task->kind == WQ_TASK_QUIZ && !s_session.answered) {
            // Slot out of range: never matches, so the answer scores as wrong.
            scored = true;
            correct = wq_session_answer(&s_session, WQ_OPTIONS);
        }
    } else if (gsp_wq_event_is_speak(event)) {
        wq_audio_play(WQ_AUDIO_CLICK);
        set_feedback(ui, "发音功能即将上线");
        return;
    } else if (gsp_wq_event_is_hint(event)) {
        if (task && task->kind == WQ_TASK_QUIZ && !s_session.answered) {
            char hint[24];
            char text[64];
            trunc_utf8(hint, sizeof(hint), WQ_WORD_PACK[task->word].gloss, 2);
            snprintf(text, sizeof(text), "提示: %s··", hint);
            set_feedback(ui, text);
            wq_audio_play(WQ_AUDIO_CLICK);
        }
        return;
    } else if (gsp_wq_event_is_learned(event)) {
        wq_audio_play(WQ_AUDIO_CLICK);
        wq_session_complete_current(&s_session);
        (void)wq_session_advance(&s_session);
        show_task(ui, state);
        return;
    } else if (gsp_wq_event_is_again(event)) {
        show_home(ui, state);
        return;
    } else if (gsp_wq_event_is_start(event)) {
        wq_audio_play(WQ_AUDIO_START);
        remember(gsp_wq_home_overlay_set_visible(ui, false));
        set_home_page(ui, false);
        restart_session(ui, state);
        return;
    } else if (gsp_wq_event_is_music(event)) {
        s_music_on = !s_music_on;
        nvs_handle_t nvs;
        if (nvs_open("wordquest", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u32(nvs, "music", s_music_on ? 1 : 0);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        set_text_retry(ui, gsp_wq_music_lbl_set_text,
                       s_music_on ? "音乐:开" : "音乐:关");
        wq_audio_bgm(s_music_on ? WQ_BGM_HOME : WQ_BGM_NONE);
        if (!s_music_on) {
            wq_audio_play(WQ_AUDIO_CLICK);
        }
        return;
    } else if (gsp_wq_event_is_book4(event) || gsp_wq_event_is_book6(event)) {
        const uint8_t next = gsp_wq_event_is_book4(event) ? 0 : 1;
        if (next != wq_progress_book() && next < wq_dict_book_count()) {
            wq_audio_play(WQ_AUDIO_CLICK);
            wq_progress_load(next, wq_dict_word_count(next));
            show_home(ui, state);
        }
        return;
    } else if (gsp_wq_event_is_go_home(event)) {
        wq_audio_play(WQ_AUDIO_CLICK);
        show_home(ui, state);
        return;
    } else {
        return;
    }

    if (!scored) {
        return; // ignored double-tap on an already answered quiz
    }

    refresh_hp(ui);
    refresh_day(ui);
    if (correct) {
        set_feedback(ui, "答对了！攻击 -1");
        wq_audio_play(WQ_AUDIO_ATTACK);
        start_sequence(ui, state,
                       wq_session_hp(&s_session) <= 0 ? WQ_SEQ_DEFEAT : WQ_SEQ_ATTACK);
    } else {
        const size_t correct_word = s_session.options[s_session.correct_slot];
        char text[96];
        snprintf(text, sizeof(text), "答错了，Boss 反击！正确答案: %s",
                 WQ_WORD_PACK[correct_word].gloss);
        set_feedback(ui, text);
        wq_audio_play(WQ_AUDIO_HURT);
        if (!state->timer) {
            (void)wq_session_advance(&s_session);
            show_task(ui, state); // no timer: degrade to instant advance
        } else {
            start_sequence(ui, state, WQ_SEQ_BOSS_ATTACK);
        }
    }
}

esp_gsp_err_t wordquest_ui_init(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    if (!ui || !state) {
        return ESP_GSP_ERR_INVALID_ARG;
    }

    *state = (wordquest_ui_t){0};
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t value = 1;
        if (nvs_get_u32(nvs, "level", &value) == ESP_OK && value > 0) {
            state->level = value;
        }
        nvs_close(nvs);
    }

    if (state->level == 0) {
        state->level = 1; // first run: NVS empty, start at level 1
    }

    if (wq_dict_init() != 0) {
        WQ_LOGE("dictionary load failed");
    }
    uint8_t book = 0;
    nvs_handle_t nvs_cfg;
    if (nvs_open("wordquest", NVS_READONLY, &nvs_cfg) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(nvs_cfg, "book", &stored) == ESP_OK) {
            book = (uint8_t)stored;
        }
        uint32_t music = 1;
        if (nvs_get_u32(nvs_cfg, "music", &music) == ESP_OK) {
            s_music_on = music != 0;
        }
        nvs_close(nvs_cfg);
    }
    if (wq_progress_load(book, wq_dict_word_count(book)) != 0) {
        WQ_LOGE("progress load failed");
    }
    s_pack = wq_dict_entry(book, 0);
    s_count = wq_dict_word_count(book);
    if (wq_audio_init() != 0) {
        WQ_LOGE("sound effects unavailable");
    }

    remember(esp_gsp_on_event(ui, on_event, state));
    state->timer = esp_gsp_timer_create(ui, TICK_MS, stepper_tick, state);
    if (!state->timer) {
        WQ_LOGE("animator timer unavailable; animations disabled");
    }
    show_home(ui, state);
    return ESP_GSP_OK;
}

void wordquest_ui_deinit(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    if (ui && state) {
        remember(esp_gsp_on_event(ui, NULL, NULL));
        if (state->timer) {
            remember(esp_gsp_timer_delete(ui, state->timer));
            state->timer = NULL;
        }
    }
}
