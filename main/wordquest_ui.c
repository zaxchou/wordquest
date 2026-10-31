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
#include <time.h>

#include "bundle_gsp.h"
#include "wq_dict.h"
#include "wq_progress.h"
#include "wq_model.h"
#include "wq_settings.h"
#include "wq_stats.h"
#include "wq_text_metrics.h"

#if !defined(ESP_PLATFORM)
// Native simulator build: no ESP-IDF logging or timers. Logging falls back to
// stderr and the clock comes from libc; settings live in wq_settings' RAM store.
#include <time.h>
static const char *TAG = "wq_ui";
#define WQ_LOGE(...) fprintf(stderr, "E wq_ui: "), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
static int64_t sim_time_us(void)
{
    return (int64_t)time(NULL) * 1000000;
}
#define esp_timer_get_time sim_time_us
#else
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
static const char *TAG = "wq_ui";
#define WQ_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#endif

#define TICK_MS 100
#define IDLE_FRAME_TICKS 6       // idle pose holds 600 ms
#define CHEST_OPEN_TICKS 5       // chest stays closed 500 ms on the win page
#define FEEDBACK_IDLE "选择正确的意思，攻击 Boss！"

// The session is a module-owned singleton: GSP delivers events from a single
// UI task, so no extra locking is required (documented single-owner design).
static wq_session_t s_session;
static wq_task_t s_tasks[WQ_NEW_WORDS_PER_DAY * 5];
static char s_option_text[WQ_OPTIONS][80];
static size_t s_new_words[WQ_NEW_WORDS_PER_DAY];
static size_t s_new_count;
static bool s_review_only; // current session reviews learned words only
static bool s_fav_only;    // current session practises saved words only
// Live settings mirror: read at startup, verified-saved on every change.
static wq_settings_t s_cfg;
static const wq_word_entry_t *s_pack;
static size_t s_count;

// Persist through wq_settings, which re-reads every field after committing and
// logs any field that did not survive. A failure here means the user's choice
// will be lost on reboot, so it is reported rather than ignored.
static void save_cfg(void)
{
    if (wq_settings_save(&s_cfg) != 0) {
        WQ_LOGE("settings save did not verify; keeping runtime state");
    }
}

// ---------------------------------------------------------------------------
// Sprite animator
// ---------------------------------------------------------------------------

typedef enum { MODE_IDLE = 0, MODE_SEQ, MODE_CHEST } wq_anim_mode_t;
typedef enum { PAGE_HOME = 0, PAGE_LEARN, PAGE_QUIZ, PAGE_WIN,
               PAGE_SETTINGS, PAGE_BROWSE } wq_page_t;

#define WQ_BROWSE_ROWS 5
typedef enum {
    BROWSE_ALL = 0,
    BROWSE_UNLEARNED,
    BROWSE_LEARNED,
    BROWSE_SKIPPED,
    BROWSE_FILTERS,
} wq_browse_filter_t;

static struct {
    wq_browse_filter_t filter;
    char range_lo, range_hi; // inclusive, 'A'..'Z'; 0 = no letter restriction
    size_t first;            // first visible word on this page
    size_t selected;         // highlighted word, SIZE_MAX = none
    size_t page_row[WQ_BROWSE_ROWS]; // word index shown on each row
} s_browse;

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
    static const uint8_t counts[4] = {7, 5, 1, 1};
    const uint8_t want[4] = {f->player, f->boss, f->slash, f->pstar ? 1 : 0};

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
    if (!cap) return;
    out[0] = '\0';
    if (cap < 4) return;
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

// Widths are generated from the actual subset fonts. Chinese body glyphs use
// a 16px cell; Latin advances are rounded up to avoid clipping at the edge.
static unsigned text_cell_width(const char *p)
{
    const unsigned char c = (unsigned char)*p;
    if (c < 32 || c > 126) return WQ_CJK16_WIDTH;
    const unsigned latin = WQ_BODY16_WIDTHS[c - 32];
    const unsigned mixed = WQ_CJK16_ASCII_WIDTHS[c - 32];
    return latin > mixed ? latin : mixed;
}

static void wrap_text(char *out, size_t cap, const char *src,
                      unsigned width, unsigned max_lines)
{
    if (!cap) return;
    out[0] = '\0';
    if (cap < 4 || !max_lines || width < 3U * WQ_BODY16_WIDTHS['.' - 32]) return;
    size_t used = 0;
    unsigned line = 1, pixels = 0;
    // Latin prose breaks on the last space of the line, never mid-word: the
    // saved space byte simply becomes the newline.
    size_t line_start = 0, space_at = 0;
    unsigned pixels_at_space = 0, space_w = 0;
    bool has_space = false;
    while (*src && used + 8 < cap) {
        const unsigned char c = (unsigned char)*src;
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const unsigned advance = text_cell_width(src);
        if (advance > width && *src != '\n') break;
        if (*src == '\n' || pixels + advance > width) {
            if (line == max_lines) break;
            if (*src != '\n' && has_space && space_at >= line_start) {
                // Text after the space already sits on the new line, so the
                // remaining width is the tail, not the head.
                out[space_at] = '\n';
                pixels = pixels > pixels_at_space + space_w
                             ? pixels - pixels_at_space - space_w : 0;
                ++line;
                line_start = space_at + 1;
                has_space = false;
                continue; // the current character starts the next line
            }
            out[used++] = '\n';
            ++line;
            pixels = 0;
            line_start = used;
            has_space = false;
            if (*src == '\n' || *src == ' ') { ++src; continue; }
        }
        if (c == ' ') {
            space_at = used;
            pixels_at_space = pixels;
            space_w = advance;
            has_space = true;
        }
        memcpy(out + used, src, len);
        used += len;
        src += len;
        pixels += advance;
    }
    if (*src) {
        // Leave room for the ellipsis even when the last line is already full.
        while (used && pixels + 3U * WQ_BODY16_WIDTHS['.' - 32] > width) {
            do { --used; } while (used && ((unsigned char)out[used] & 0xC0) == 0x80);
            const unsigned w = (unsigned char)out[used] == '\n'
                                   ? 0 : text_cell_width(out + used);
            pixels = pixels >= w ? pixels - w : 0;
        }
        memcpy(out + used, "...", 3);
        used += 3;
    }
    out[used] = '\0';
}

static void set_word(esp_gsp_handle_t ui, const char *word, bool learn)
{
    unsigned width = 0;
    for (const unsigned char *p = (const unsigned char *)word; *p; ++p) {
        width += *p >= 32 && *p <= 126 ? WQ_PIXEL26_WIDTHS[*p - 32] : 26;
    }
    const bool wide = width > WQ_WORD_WIDTH;
    set_text_retry(ui, learn ? (wide ? gsp_wq_learn_word_long_set_text :
                                      gsp_wq_learn_word_set_text) :
                               (wide ? gsp_wq_word_label_long_set_text :
                                       gsp_wq_word_label_set_text), word);
    remember(esp_gsp_component_set_visible(ui, learn ? GSP_WQ_OBJ_KEY_LEARN_WORD :
                                                       GSP_WQ_OBJ_KEY_WORD_LABEL, !wide));
    remember(esp_gsp_component_set_visible(ui, learn ? GSP_WQ_OBJ_KEY_LEARN_WORD_LONG :
                                                       GSP_WQ_OBJ_KEY_WORD_LABEL_LONG, wide));
}

static void refresh_hp(esp_gsp_handle_t ui)
{
    char text[24];
    snprintf(text, sizeof(text), "HP %d/%d", wq_session_hp(&s_session),
             wq_session_hp_max(&s_session));
    const int maximum = wq_session_hp_max(&s_session);
    remember(gsp_wq_hp_bar_set_value(ui, maximum > 0 ?
                                    wq_session_hp(&s_session) * 100 / maximum : 0));
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
    // The scrim and the deck are non-button widgets: while the home page is
    // hidden they still take part in hit-testing, so they must be switched off
    // with the page or they swallow taps on the battle options below them.
    gsp_wq_home_scrim_set_visible,
    gsp_wq_home_deck_set_visible,
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

// Favourite state for a word drives both the quiz and the learn button.
static void refresh_fav_labels(esp_gsp_handle_t ui, wordquest_ui_t *state,
                               size_t word)
{
    (void)state;
    const bool saved = wq_progress_is_fav(word);
    // The quiz labels are visibility-bound twins (a hidden button does not hide
    // its child label in GSP); the learn label lives in its overlay and only
    // needs its text updated.
    remember(gsp_wq_fav_lbl_quiz_set_visible(ui, !saved));
    remember(gsp_wq_fav_lbl_quiz_on_set_visible(ui, saved));
    set_text_retry(ui, gsp_wq_fav_lbl_learn_set_text, saved ? "已收藏" : "收藏");
}

static void set_feedback(esp_gsp_handle_t ui, const char *text)
{
    char lines[160];
    wrap_text(lines, sizeof(lines), text, WQ_FEEDBACK_WIDTH, 2);
    set_text_retry(ui, gsp_wq_feedback_set_text, lines);
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

static void set_ipa(esp_gsp_handle_t ui,
                    esp_gsp_err_t (*fn)(esp_gsp_handle_t, const char *),
                    const char *ipa)
{
    char text[96];
    trunc_utf8(text, sizeof(text), ipa, 60);
    char *read = text;
    char *write = text;
    while (*read) {
        if ((unsigned char)read[0] == 0xEF &&
            (unsigned char)read[1] == 0xBC &&
            ((unsigned char)read[2] == 0x8C || (unsigned char)read[2] == 0x9B)) {
            *write++ = (unsigned char)read[2] == 0x8C ? ',' : ';';
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    set_text_retry(ui, fn, text);
}

static void hide_browse(esp_gsp_handle_t ui);
static void show_learn(esp_gsp_handle_t ui, wordquest_ui_t *state,
                       const wq_word_entry_t *word)
{
    state->page = PAGE_LEARN;
    size_t learn_done = 0, learn_total = 0;
    for (size_t i = 0; i < s_session.task_count; ++i) {
        if (s_session.tasks[i].kind != WQ_TASK_LEARN) continue;
        ++learn_total;
        if (i < s_session.cursor || s_session.tasks[i].done) ++learn_done;
    }
    char title[48];
    snprintf(title, sizeof(title), "%s · 新词 %u/%u",
             wq_dict_book_name(wq_progress_book()),
             (unsigned)(learn_done + 1), (unsigned)learn_total);
    set_text_retry(ui, gsp_wq_learn_title_set_text, title);
    remember(gsp_wq_home_btn_set_visible(ui, true));
    remember(gsp_wq_fav_btn_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_on_set_visible(ui, false));
    char text[96];
    wrap_text(text, sizeof(text), word->meaning, WQ_LEARN_WIDTH, 2);
    set_word(ui, word->word, true);
    set_ipa(ui, gsp_wq_learn_ipa_set_text, word->ipa);
    set_text_retry(ui, gsp_wq_learn_meaning_set_text, text);
    trunc_utf8(text, sizeof(text), word->example_en, 80);
    sanitize_quotes(text);
    char example[128];
    wrap_text(example, sizeof(example), text, WQ_LEARN_WIDTH, 2);
    set_text_retry(ui, gsp_wq_learn_ex_en_set_text, example);
    wrap_text(text, sizeof(text), word->example_zh, WQ_LEARN_WIDTH, 2);
    set_text_retry(ui, gsp_wq_learn_ex_zh_set_text, text);

    refresh_fav_labels(ui, state, (size_t)(word - s_session.pack));
    remember(gsp_wq_learn_overlay_set_visible(ui, true));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    remember(gsp_wq_settings_overlay_set_visible(ui, false));
    hide_browse(ui);
    set_learn_page(ui, true);
    set_win_page(ui, false);
}

static void show_quiz(esp_gsp_handle_t ui, wordquest_ui_t *state,
                      const wq_word_entry_t *word)
{
    state->page = PAGE_QUIZ;
    remember(gsp_wq_home_btn_set_visible(ui, true));
    // The victory overlay owns the top band; restore the battle HUD for play.
    remember(gsp_wq_fav_btn_quiz_set_visible(ui, true));
    remember(esp_gsp_component_set_visible(ui, GSP_WQ_OBJ_KEY_INFO_CHIP, true));
    remember(esp_gsp_component_set_visible(ui, GSP_WQ_OBJ_KEY_HP_CHIP, true));
    size_t slots[WQ_OPTIONS];

    wq_session_build_options(&s_session, slots);
    for (size_t i = 0; i < WQ_OPTIONS; ++i) {
        wrap_text(s_option_text[i], sizeof(s_option_text[i]),
                  s_session.pack[slots[i]].gloss, WQ_OPTION_WIDTH, 2);
    }
    set_word(ui, word->word, false);
    set_ipa(ui, gsp_wq_ipa_label_set_text, word->ipa);
    set_text_retry(ui, gsp_wq_opt_lbl_0_set_text, s_option_text[0]);
    set_text_retry(ui, gsp_wq_opt_lbl_1_set_text, s_option_text[1]);
    set_text_retry(ui, gsp_wq_opt_lbl_2_set_text, s_option_text[2]);
    set_text_retry(ui, gsp_wq_opt_lbl_3_set_text, s_option_text[3]);
    set_feedback(ui, FEEDBACK_IDLE);
    refresh_hp(ui);
    refresh_day(ui);
    refresh_fav_labels(ui, state, (size_t)(word - s_session.pack));

    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    remember(gsp_wq_settings_overlay_set_visible(ui, false));
    hide_browse(ui);
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
    const wq_word_entry_t *word = &s_session.pack[task->word];
    if (task->kind == WQ_TASK_LEARN) {
        show_learn(ui, state, word);
    } else {
        show_quiz(ui, state, word);
    }
}

// Camp / profile page: the hub the day starts and ends on.
// Days since the epoch for check-in bookkeeping; 0 while the clock has not
// been set (the device syncs time from the host, the simulator uses libc).
static long wq_day_number(void)
{
    const time_t now = time(NULL);
    if (now < 1700000000) {
        return 0; // clock unsynced: keep the stored streak untouched
    }
    return (long)now / 86400;
}

// --- word browser -----------------------------------------------------------
//
// The dictionary is alphabetical, so a page is simply a run of visible words
// starting at s_browse.first. Filters cover progress state; the letter buttons
// jump to the first word of a range.

static char first_letter(size_t idx)
{
    const wq_word_entry_t *e = &s_pack[idx];
    const char c = e->word ? e->word[0] : '?';
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

static bool browse_visible(size_t idx)
{
    if (idx >= s_count) return false;
    if (s_browse.range_lo) {
        const char c = first_letter(idx);
        if (c == '#' || c < s_browse.range_lo || c > s_browse.range_hi) return false;
    }
    switch (s_browse.filter) {
    case BROWSE_UNLEARNED:
        return !wq_progress_is_learned(idx);
    case BROWSE_LEARNED:
        return wq_progress_is_learned(idx);
    case BROWSE_SKIPPED:
        return wq_progress_is_skipped(idx);
    case BROWSE_ALL:
    default:
        return true;
    }
}

static size_t browse_seek_prev(size_t before)
{
    for (size_t i = before; i-- > 0;) {
        if (browse_visible(i)) return i;
    }
    return 0;
}

static size_t browse_seek(size_t from)
{
    for (size_t i = from; i < s_count; ++i) {
        if (browse_visible(i)) return i;
    }
    return s_count;
}

static void browse_set_text(esp_gsp_handle_t ui, uint32_t key, const char *text)
{
    esp_gsp_err_t err = esp_gsp_component_set_text(ui, key, text);
    for (int i = 0; err == ESP_GSP_ERR_TIMEOUT && i < 5; ++i) {
        wq_yield_10ms();
        err = esp_gsp_component_set_text(ui, key, text);
    }
}

static void show_browse(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    static const char *const FILTER_NAMES[BROWSE_FILTERS] = {
        "全部", "未学", "已学", "跳过"
    };
    state->page = PAGE_BROWSE;
    s_browse.first = browse_seek(s_browse.first);
    if (s_browse.first >= s_count) s_browse.first = browse_seek(0);

    char text[48];
    snprintf(text, sizeof(text), "词表 %s · %s", wq_dict_book_name(wq_progress_book()),
             FILTER_NAMES[s_browse.filter]);
    set_text_retry(ui, gsp_wq_browse_title_set_text, text);
    snprintf(text, sizeof(text), "筛选:%s", FILTER_NAMES[s_browse.filter]);
    set_text_retry(ui, gsp_wq_br_filter_lbl_set_text, text);

    // Each row button owns a label: buttons with label children are the
    // structure every other page uses, so they render reliably.
    static esp_gsp_err_t (*const ROW_SET[5])(esp_gsp_handle_t, const char *) = {
        gsp_wq_browse_row0_lbl_set_text, gsp_wq_browse_row1_lbl_set_text,
        gsp_wq_browse_row2_lbl_set_text, gsp_wq_browse_row3_lbl_set_text,
        gsp_wq_browse_row4_lbl_set_text,
    };
    size_t cursor = s_browse.first;
    for (size_t row = 0; row < WQ_BROWSE_ROWS; ++row) {
        const size_t idx = browse_seek(cursor);
        s_browse.page_row[row] = idx;
        char line[48];
        if (idx >= s_count) {
            snprintf(line, sizeof(line), "—");
        } else {
            const char *mark = wq_progress_is_skipped(idx) ? "跳过"
                             : wq_progress_is_learned(idx) ? "已学" : "未学";
            snprintf(line, sizeof(line), "%s  %s", s_pack[idx].word, mark);
            cursor = idx + 1;
        }
        set_text_retry(ui, ROW_SET[row], line);
    }

    if (s_browse.selected < s_count) {
        trunc_utf8(text, sizeof(text), s_pack[s_browse.selected].gloss, 18);
        char detail[64];
        snprintf(detail, sizeof(detail), "%s - %s", s_pack[s_browse.selected].word, text);
        set_text_retry(ui, gsp_wq_browse_detail_set_text, detail);
        set_text_retry(ui, gsp_wq_br_skip_lbl_set_text,
                       wq_progress_is_skipped(s_browse.selected) ? "取消跳过" : "跳过该词");
    } else {
        set_text_retry(ui, gsp_wq_browse_detail_set_text, "点一行查看释义");
        set_text_retry(ui, gsp_wq_br_skip_lbl_set_text, "跳过该词");
    }

    // Page widgets are root-level and therefore need explicit show/hide.
    static const uint32_t SHOW[] = {
        GSP_WQ_OBJ_KEY_BROWSE_SCRIM, GSP_WQ_OBJ_KEY_BROWSE_TITLE,
        GSP_WQ_OBJ_KEY_BR_LETTER0, GSP_WQ_OBJ_KEY_BR_LETTER1, GSP_WQ_OBJ_KEY_BR_LETTER2,
        GSP_WQ_OBJ_KEY_BR_LETTER3, GSP_WQ_OBJ_KEY_BR_FILTER, GSP_WQ_OBJ_KEY_BR_FILTER_LBL,
        GSP_WQ_OBJ_KEY_BR_PREV, GSP_WQ_OBJ_KEY_BR_NEXT, GSP_WQ_OBJ_KEY_BROWSE_ROW0,
        GSP_WQ_OBJ_KEY_BROWSE_ROW1, GSP_WQ_OBJ_KEY_BROWSE_ROW2, GSP_WQ_OBJ_KEY_BROWSE_ROW3,
        GSP_WQ_OBJ_KEY_BROWSE_ROW4, GSP_WQ_OBJ_KEY_BROWSE_ROW0_LBL,
        GSP_WQ_OBJ_KEY_BROWSE_ROW1_LBL, GSP_WQ_OBJ_KEY_BROWSE_ROW2_LBL,
        GSP_WQ_OBJ_KEY_BROWSE_ROW3_LBL, GSP_WQ_OBJ_KEY_BROWSE_ROW4_LBL,
        GSP_WQ_OBJ_KEY_BROWSE_DETAIL,
        GSP_WQ_OBJ_KEY_BR_LETTER0_LBL, GSP_WQ_OBJ_KEY_BR_LETTER1_LBL,
        GSP_WQ_OBJ_KEY_BR_LETTER2_LBL, GSP_WQ_OBJ_KEY_BR_LETTER3_LBL,
        GSP_WQ_OBJ_KEY_BR_PREV_LBL, GSP_WQ_OBJ_KEY_BR_NEXT_LBL,
        GSP_WQ_OBJ_KEY_BR_BACK_LBL, GSP_WQ_OBJ_KEY_BR_BACK, GSP_WQ_OBJ_KEY_BR_SKIP, GSP_WQ_OBJ_KEY_BR_SKIP_LBL,
    };
    for (size_t i = 0; i < sizeof(SHOW) / sizeof(SHOW[0]); ++i) {
        remember(esp_gsp_component_set_visible(ui, SHOW[i], true));
    }

    remember(gsp_wq_fav_btn_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_on_set_visible(ui, false));
    remember(gsp_wq_home_btn_set_visible(ui, false));
    remember(gsp_wq_home_overlay_set_visible(ui, false));
    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    remember(gsp_wq_settings_overlay_set_visible(ui, false));
    set_home_page(ui, false);
    set_learn_page(ui, false);
    set_win_page(ui, false);
    state->mode = MODE_IDLE;
}

static void hide_browse(esp_gsp_handle_t ui)
{
    static const uint32_t HIDE[] = {
        GSP_WQ_OBJ_KEY_BROWSE_SCRIM, GSP_WQ_OBJ_KEY_BROWSE_TITLE,
        GSP_WQ_OBJ_KEY_BR_LETTER0, GSP_WQ_OBJ_KEY_BR_LETTER1, GSP_WQ_OBJ_KEY_BR_LETTER2,
        GSP_WQ_OBJ_KEY_BR_LETTER3, GSP_WQ_OBJ_KEY_BR_FILTER, GSP_WQ_OBJ_KEY_BR_FILTER_LBL,
        GSP_WQ_OBJ_KEY_BR_PREV, GSP_WQ_OBJ_KEY_BR_NEXT, GSP_WQ_OBJ_KEY_BROWSE_ROW0,
        GSP_WQ_OBJ_KEY_BROWSE_ROW1, GSP_WQ_OBJ_KEY_BROWSE_ROW2, GSP_WQ_OBJ_KEY_BROWSE_ROW3,
        GSP_WQ_OBJ_KEY_BROWSE_ROW4, GSP_WQ_OBJ_KEY_BROWSE_ROW0_LBL,
        GSP_WQ_OBJ_KEY_BROWSE_ROW1_LBL, GSP_WQ_OBJ_KEY_BROWSE_ROW2_LBL,
        GSP_WQ_OBJ_KEY_BROWSE_ROW3_LBL, GSP_WQ_OBJ_KEY_BROWSE_ROW4_LBL,
        GSP_WQ_OBJ_KEY_BROWSE_DETAIL,
        GSP_WQ_OBJ_KEY_BR_LETTER0_LBL, GSP_WQ_OBJ_KEY_BR_LETTER1_LBL,
        GSP_WQ_OBJ_KEY_BR_LETTER2_LBL, GSP_WQ_OBJ_KEY_BR_LETTER3_LBL,
        GSP_WQ_OBJ_KEY_BR_PREV_LBL, GSP_WQ_OBJ_KEY_BR_NEXT_LBL,
        GSP_WQ_OBJ_KEY_BR_BACK_LBL, GSP_WQ_OBJ_KEY_BR_BACK, GSP_WQ_OBJ_KEY_BR_SKIP, GSP_WQ_OBJ_KEY_BR_SKIP_LBL,
    };
    for (size_t i = 0; i < sizeof(HIDE) / sizeof(HIDE[0]); ++i) {
        remember(esp_gsp_component_set_visible(ui, HIDE[i], false));
    }
}

// Wordbook settings page: pick the daily new-word quota.
static void show_settings(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    state->page = PAGE_SETTINGS;
    remember(gsp_wq_home_overlay_set_visible(ui, false));
    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    remember(gsp_wq_settings_overlay_set_visible(ui, false));
    hide_browse(ui);
    remember(gsp_wq_settings_overlay_set_visible(ui, true));
    set_home_page(ui, false);
    set_learn_page(ui, false);
    set_win_page(ui, false);
    const uint32_t keys[4] = {GSP_WQ_OBJ_KEY_DAY5_BTN, GSP_WQ_OBJ_KEY_DAY10_BTN,
                              GSP_WQ_OBJ_KEY_DAY15_BTN,
                              GSP_WQ_OBJ_KEY_DAY20_BTN};
    const uint32_t counts[4] = {5, 10, 15, 20};
    for (size_t i = 0; i < 4; ++i) {
        remember(esp_gsp_component_set_color_rgb888(
            ui, keys[i], counts[i] == s_cfg.daily ? 0x2E5D34 : 0x14231A));
    }
    char text[64];
    set_text_retry(ui, gsp_wq_music_lbl_set_text,
                   s_cfg.music_on ? "音乐:开" : "音乐:关");
    snprintf(text, sizeof(text), "顽固词 %u · 待复习 %u",
             (unsigned)wq_progress_wrong_count(),
             (unsigned)wq_progress_due_count() < wq_progress_learned()
                 ? (unsigned)wq_progress_due_count()
                 : (unsigned)wq_progress_review_due(s_cfg.daily));
    set_text_retry(ui, gsp_wq_settings_stats_set_text, text);

    const uint32_t today = (uint32_t)wq_day_number();
    wq_stats_sum_t week, month;
    wq_stats_window(today, 7, &week);
    wq_stats_window(today, 30, &month);
    snprintf(text, sizeof(text), "本周 学 %u · 复习 %u",
             (unsigned)week.learned, (unsigned)(week.correct + week.wrong));
    set_text_retry(ui, gsp_wq_stats_week_set_text, text);
    const unsigned answers = (unsigned)(month.correct + month.wrong);
    const unsigned pct = answers ? (unsigned)(month.correct * 100 / answers) : 0;
    snprintf(text, sizeof(text), "本月 学 %u · 正确 %u%%",
             (unsigned)month.learned, pct);
    set_text_retry(ui, gsp_wq_stats_month_set_text, text);
    state->mode = MODE_IDLE;
}

static void show_home(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    state->page = PAGE_HOME;
    hide_browse(ui);
    remember(gsp_wq_home_btn_set_visible(ui, false));
    remember(gsp_wq_fav_btn_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_on_set_visible(ui, false));
    remember(gsp_wq_badge_forest_set_visible(ui, false));
    const bool cet4 = wq_progress_book() == 0;
    remember(esp_gsp_component_set_color_rgb888(ui, GSP_WQ_OBJ_KEY_BOOK4_BTN,
                                             cet4 ? 0x2E5D34 : 0x14231A));
    remember(esp_gsp_component_set_color_rgb888(ui, GSP_WQ_OBJ_KEY_BOOK6_BTN,
                                             cet4 ? 0x14231A : 0x2E5D34));
    set_text_retry(ui, gsp_wq_book4_lbl_set_text, cet4 ? "四级 · 已选" : "四级");
    set_text_retry(ui, gsp_wq_book6_lbl_set_text, cet4 ? "六级" : "六级 · 已选");
    set_text_retry(ui, gsp_wq_start_lbl_set_text, cet4 ? "开始四级挑战" : "开始六级挑战");
    char text[64];
    const size_t due = wq_progress_review_due(s_cfg.daily);
    snprintf(text, sizeof(text), "复习 %u", (unsigned)due);
    set_text_retry(ui, gsp_wq_review_lbl_set_text, text);
    snprintf(text, sizeof(text), "生词本 %u", (unsigned)wq_progress_fav_count());
    set_text_retry(ui, gsp_wq_fav_lbl_home_set_text, text);
    // A streak broken by a missed full day reads as zero until the next
    // finished session restarts it.
    const long today = wq_day_number();
    uint32_t shown_streak = s_cfg.streak;
    if (today > 0 && s_cfg.last_day != 0 &&
        (uint32_t)today > s_cfg.last_day + 1) {
        shown_streak = 0;
    }
    snprintf(text, sizeof(text), "已斩 %u/%u · Lv.%d · 打卡 %u 天",
             (unsigned)wq_progress_learned(), (unsigned)wq_progress_total(),
             (int)s_cfg.level, (unsigned)shown_streak);
    set_text_retry(ui, gsp_wq_home_stats_set_text, text);
    wq_audio_bgm(s_cfg.music_on ? WQ_BGM_HOME : WQ_BGM_NONE);

    remember(gsp_wq_home_overlay_set_visible(ui, true));
    remember(gsp_wq_learn_overlay_set_visible(ui, false));
    remember(gsp_wq_win_overlay_set_visible(ui, false));
    remember(gsp_wq_settings_overlay_set_visible(ui, false));
    set_home_page(ui, true);
    set_learn_page(ui, false);
    set_win_page(ui, false);
    state->mode = MODE_IDLE;
}

static void show_win(esp_gsp_handle_t ui, wordquest_ui_t *state)
{
    if (state->page == PAGE_WIN) {
        return;
    }
    state->page = PAGE_WIN;
    remember(gsp_wq_home_btn_set_visible(ui, false));
    // The HUD and the quiz-only buttons belong to the battle page.
    remember(gsp_wq_fav_btn_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_set_visible(ui, false));
    remember(gsp_wq_fav_lbl_quiz_on_set_visible(ui, false));
    remember(esp_gsp_component_set_visible(ui, GSP_WQ_OBJ_KEY_INFO_CHIP, false));
    remember(esp_gsp_component_set_visible(ui, GSP_WQ_OBJ_KEY_HP_CHIP, false));
    wq_progress_mark_learned(s_new_words, s_new_count);
    wq_progress_commit_start(wq_progress_start() + s_new_count);
    const int previous = (int)s_cfg.level;
    if (!s_review_only) {
        ++s_cfg.level; // reviewing does not advance the adventure
        save_cfg();
    }

    char text[48];
    if (s_review_only) {
        snprintf(text, sizeof(text), s_fav_only ? "生词本练习 · Lv.%d"
                                                : "复习完成 · Lv.%d",
                 (int)s_cfg.level);
    } else {
        snprintf(text, sizeof(text), "冒险等级 Lv.%d → Lv.%d", previous,
                 (int)s_cfg.level);
    }
    set_text_retry(ui, gsp_wq_win_level_set_text, text);
    snprintf(text, sizeof(text), "第 %d 关", (int)s_cfg.level);
    set_text_retry(ui, gsp_wq_lvl_text_set_text, text);
    snprintf(text, sizeof(text), "今日任务 %u · 答对 %u",
             (unsigned)s_session.task_count,
             (unsigned)wq_session_correct_count(&s_session));
    set_text_retry(ui, gsp_wq_win_sub_set_text, text);
    // Check-in: the first finished session of a (synced) day extends or
    // restarts the streak; later replays keep it unchanged.
    const long day = wq_day_number();
    {
        // Weekly/monthly report: one record per finished session.
        size_t learned_done = 0;
        for (size_t i = 0; i < s_session.task_count; ++i) {
            if (s_session.tasks[i].kind == WQ_TASK_LEARN &&
                s_session.tasks[i].done) {
                ++learned_done;
            }
        }
        if (day > 0) {
            (void)wq_stats_add((uint32_t)day, (uint16_t)learned_done,
                               (uint16_t)wq_session_correct_count(&s_session),
                               (uint16_t)wq_session_wrong_count(&s_session));
        }
    }
    if (day > 0 && (uint32_t)day != s_cfg.last_day) {
        s_cfg.streak = s_cfg.last_day + 1 == (uint32_t)day ? s_cfg.streak + 1 : 1;
        s_cfg.last_day = (uint32_t)day;
        s_cfg.days_total += 1;
        save_cfg();
    }
    snprintf(text, sizeof(text), "连续打卡 %u 天 · 共 %u 天",
             (unsigned)s_cfg.streak, (unsigned)s_cfg.days_total);
    set_text_retry(ui, gsp_wq_win_reward_set_text, text);
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
    if (state->page == PAGE_HOME || state->page == PAGE_LEARN ||
        state->page == PAGE_SETTINGS ||
        (state->page == PAGE_WIN && state->mode != MODE_CHEST)) {
        return;
    }

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

// Session sources: a normal day, review-only, or favourites-only.
typedef enum { SRC_DAILY = 0, SRC_REVIEW, SRC_FAV } wq_source_t;

// Refresh whichever favourite labels the current page shows.
static void refresh_fav_labels(esp_gsp_handle_t ui, wordquest_ui_t *state,
                               size_t word);

static void restart_session(esp_gsp_handle_t ui, wordquest_ui_t *state,
                            wq_source_t source)
{
    const bool review_only = source != SRC_DAILY;
    s_review_only = review_only;
    s_fav_only = source == SRC_FAV;
    s_pack = wq_dict_entry(wq_progress_book(), 0);
    s_count = wq_dict_word_count(wq_progress_book());
    s_new_count = review_only ? 0 : wq_progress_pick_new(s_cfg.daily, s_new_words);
    size_t review[WQ_NEW_WORDS_PER_DAY * 2];
    size_t review_count;
    if (s_fav_only) {
        review_count = wq_progress_pick_favs(WQ_NEW_WORDS_PER_DAY * 2U, review);
    } else {
        review_count =
            wq_progress_pick_review(wq_progress_review_due(s_cfg.daily), review);
    }
    WQ_LOGE("restart: daily=%u new=%u review=%u%s", (unsigned)s_cfg.daily,
            (unsigned)s_new_count, (unsigned)review_count,
            s_fav_only ? " (favourites)" : review_only ? " (review only)" : "");
    if (review_count) {
        wq_progress_advance_schedule(); // served words rest until their interval
    }
    wq_session_start(&s_session, s_pack, s_count, s_new_words, s_new_count,
                     review, review_count, s_tasks,
                     sizeof(s_tasks) / sizeof(s_tasks[0]),
                     (uint32_t)esp_timer_get_time());
    char text[32];
    if (s_fav_only) {
        snprintf(text, sizeof(text), "生词本");
    } else if (review_only) {
        snprintf(text, sizeof(text), "复习");
    } else {
        snprintf(text, sizeof(text), "第 %d 关", (int)s_cfg.level);
    }
    set_text_retry(ui, gsp_wq_lvl_text_set_text, text);
    wq_audio_bgm(s_cfg.music_on ? WQ_BGM_BATTLE : WQ_BGM_NONE);
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
    const bool answer_event = gsp_wq_event_is_opt_0(event) ||
        gsp_wq_event_is_opt_1(event) || gsp_wq_event_is_opt_2(event) ||
        gsp_wq_event_is_opt_3(event) || gsp_wq_event_is_idk(event);
    if (answer_event && (state->page != PAGE_QUIZ ||
        state->mode != MODE_IDLE || !task || s_session.answered)) {
        return;
    }

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
    } else if (gsp_wq_event_is_hint(event)) {
        if (task && task->kind == WQ_TASK_QUIZ && !s_session.answered) {
            char hint[24];
            char text[64];
            trunc_utf8(hint, sizeof(hint), s_session.pack[task->word].gloss, 2);
            snprintf(text, sizeof(text), "提示: %s··", hint);
            set_feedback(ui, text);
            wq_audio_play(WQ_AUDIO_CLICK);
        }
        return;
    } else if (gsp_wq_event_is_learned(event)) {
        if (state->page != PAGE_LEARN || !task || task->kind != WQ_TASK_LEARN) {
            return;
        }
        wq_audio_play(WQ_AUDIO_CLICK);
        // A studied word counts immediately: quitting mid-session must not
        // re-teach it tomorrow.
        wq_progress_mark_learned(&task->word, 1);
        wq_session_complete_current(&s_session);
        (void)wq_session_advance(&s_session);
        show_task(ui, state);
        return;
    } else if (gsp_wq_event_is_again(event)) {
        if (state->page != PAGE_WIN) return;
        show_home(ui, state);
        return;
    } else if (gsp_wq_event_is_start(event)) {
        if (state->page != PAGE_HOME || !s_pack || s_count < WQ_OPTIONS) return;
        wq_audio_play(WQ_AUDIO_START);
        remember(gsp_wq_home_overlay_set_visible(ui, false));
        set_home_page(ui, false);
        restart_session(ui, state, SRC_DAILY);
        return;
    } else if (gsp_wq_event_is_review(event)) {
        if (state->page != PAGE_HOME) return;
        if (wq_progress_review_due(s_cfg.daily) == 0) {
            // Nothing due yet: say so on the review pill instead of starting
            // an empty battle.
            set_text_retry(ui, gsp_wq_review_lbl_set_text, "先学新词");
            wq_audio_play(WQ_AUDIO_CLICK);
            return;
        }
        wq_audio_play(WQ_AUDIO_START);
        remember(gsp_wq_home_overlay_set_visible(ui, false));
        set_home_page(ui, false);
        restart_session(ui, state, SRC_REVIEW);
        return;
    } else if (gsp_wq_event_is_browse(event)) {
        if (state->page != PAGE_HOME) return;
        wq_audio_play(WQ_AUDIO_CLICK);
        s_browse.selected = SIZE_MAX;
        show_browse(ui, state);
        return;
    } else if (gsp_wq_event_is_filter(event)) {
        if (state->page != PAGE_BROWSE) return;
        s_browse.filter = (wq_browse_filter_t)((s_browse.filter + 1) % BROWSE_FILTERS);
        s_browse.first = 0;
        s_browse.selected = SIZE_MAX;
        wq_audio_play(WQ_AUDIO_CLICK);
        show_browse(ui, state);
        return;
    } else if (gsp_wq_event_is_page_prev(event) ||
               gsp_wq_event_is_page_next(event)) {
        if (state->page != PAGE_BROWSE) return;
        if (gsp_wq_event_is_page_next(event)) {
            // Advance five visible words from the current page start.
            size_t cursor = s_browse.first;
            for (int i = 0; i < WQ_BROWSE_ROWS; ++i) {
                const size_t idx = browse_seek(cursor);
                if (idx >= s_count) return; // already on the last page
                cursor = idx + 1;
            }
            s_browse.first = browse_seek(cursor);
            if (s_browse.first >= s_count) s_browse.first = cursor - 1;
        } else {
            size_t back = s_browse.first;
            for (int i = 0; i < WQ_BROWSE_ROWS; ++i) {
                if (back == 0) break;
                back = browse_seek_prev(back);
            }
            s_browse.first = back;
        }
        s_browse.selected = SIZE_MAX;
        wq_audio_play(WQ_AUDIO_CLICK);
        show_browse(ui, state);
        return;
    } else if (gsp_wq_event_is_letter0(event) || gsp_wq_event_is_letter1(event) ||
               gsp_wq_event_is_letter2(event) || gsp_wq_event_is_letter3(event)) {
        if (state->page != PAGE_BROWSE) return;
        static const char LO[4] = {'A', 'G', 'N', 'U'};
        static const char HI[4] = {'F', 'M', 'T', 'Z'};
        const int slot = gsp_wq_event_is_letter0(event) ? 0 :
                         gsp_wq_event_is_letter1(event) ? 1 :
                         gsp_wq_event_is_letter2(event) ? 2 : 3;
        s_browse.range_lo = LO[slot];
        s_browse.range_hi = HI[slot];
        s_browse.first = 0;
        s_browse.selected = SIZE_MAX;
        wq_audio_play(WQ_AUDIO_CLICK);
        show_browse(ui, state);
        return;
    } else if (gsp_wq_event_is_row0(event) || gsp_wq_event_is_row1(event) ||
               gsp_wq_event_is_row2(event) || gsp_wq_event_is_row3(event) ||
               gsp_wq_event_is_row4(event)) {
        if (state->page != PAGE_BROWSE) return;
        const int slot = gsp_wq_event_is_row0(event) ? 0 :
                         gsp_wq_event_is_row1(event) ? 1 :
                         gsp_wq_event_is_row2(event) ? 2 :
                         gsp_wq_event_is_row3(event) ? 3 : 4;
        const size_t idx = s_browse.page_row[slot];
        if (idx >= s_count) return;
        s_browse.selected = idx;
        wq_audio_play(WQ_AUDIO_CLICK);
        show_browse(ui, state);
        return;
    } else if (gsp_wq_event_is_skip(event)) {
        if (state->page != PAGE_BROWSE || s_browse.selected >= s_count) return;
        const bool now = wq_progress_toggle_skip(s_browse.selected);
        WQ_LOGE("skip %s: %s", s_pack[s_browse.selected].word, now ? "on" : "off");
        wq_audio_play(WQ_AUDIO_CLICK);
        show_browse(ui, state);
        return;
    } else if (gsp_wq_event_is_fav(event)) {
        // Save or unsave the word currently on screen (learn or quiz page).
        if (!task || (state->page != PAGE_LEARN && state->page != PAGE_QUIZ)) {
            return;
        }
        const bool saved = wq_progress_toggle_fav(task->word);
        refresh_fav_labels(ui, state, task->word);
        char text[32];
        snprintf(text, sizeof(text), "生词本 %u", (unsigned)wq_progress_fav_count());
        set_text_retry(ui, gsp_wq_fav_lbl_home_set_text, text);
        wq_audio_play(WQ_AUDIO_CLICK);
        if (state->page == PAGE_QUIZ) {
            set_feedback(ui, saved ? "已加入生词本" : "已移出生词本");
        }
        return;
    } else if (gsp_wq_event_is_fav_practice(event)) {
        if (state->page != PAGE_HOME) return;
        if (wq_progress_fav_count() == 0) {
            set_text_retry(ui, gsp_wq_fav_lbl_home_set_text, "暂无生词");
            wq_audio_play(WQ_AUDIO_CLICK);
            return;
        }
        wq_audio_play(WQ_AUDIO_START);
        remember(gsp_wq_home_overlay_set_visible(ui, false));
        set_home_page(ui, false);
        restart_session(ui, state, SRC_FAV);
        return;
    } else if (gsp_wq_event_is_music(event)) {
        if (state->page != PAGE_SETTINGS) return;
        s_cfg.music_on = s_cfg.music_on ? 0 : 1;
        save_cfg();
        set_text_retry(ui, gsp_wq_music_lbl_set_text,
                       s_cfg.music_on ? "音乐:开" : "音乐:关");
        wq_audio_bgm(s_cfg.music_on ? WQ_BGM_HOME : WQ_BGM_NONE);
        if (!s_cfg.music_on) {
            wq_audio_play(WQ_AUDIO_CLICK);
        }
        return;
    } else if (gsp_wq_event_is_book4(event) || gsp_wq_event_is_book6(event)) {
        if (state->page != PAGE_HOME) return;
        const uint8_t next = gsp_wq_event_is_book4(event) ? 0 : 1;
        if (next != wq_progress_book() && next < wq_dict_book_count()) {
            if (wq_progress_load(next, wq_dict_word_count(next)) != 0) return;
            s_pack = wq_dict_entry(next, 0);
            s_count = wq_dict_word_count(next);
            s_cfg.book = next;
            save_cfg();
            wq_audio_play(WQ_AUDIO_CLICK);
            show_home(ui, state);
        }
        return;
    } else if (gsp_wq_event_is_settings_open(event)) {
        if (state->page != PAGE_HOME) return;
        wq_audio_play(WQ_AUDIO_CLICK);
        show_settings(ui, state);
        return;
    } else if (gsp_wq_event_is_day5(event) || gsp_wq_event_is_day10(event) ||
               gsp_wq_event_is_day15(event) || gsp_wq_event_is_day20(event)) {
        if (state->page != PAGE_SETTINGS) return;
        s_cfg.daily = gsp_wq_event_is_day5(event) ? 5 :
                      gsp_wq_event_is_day10(event) ? 10 :
                      gsp_wq_event_is_day15(event) ? 15 : 20;
        WQ_LOGE("daily set: %u", (unsigned)s_cfg.daily);
        save_cfg();
        wq_audio_play(WQ_AUDIO_CLICK);
        show_settings(ui, state);
        return;
    } else if (gsp_wq_event_is_go_home(event)) {
        if (state->page != PAGE_LEARN && state->page != PAGE_QUIZ &&
            state->page != PAGE_SETTINGS && state->page != PAGE_BROWSE) {
            return;
        }
        wq_progress_commit_start(wq_progress_start());
        const wq_frame_t hidden = {0};
        apply_frame(ui, &hidden);
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
        if (task) {
            wq_progress_clear_wrong(task->word);
            wq_progress_record_answer(task->word, true);
        }
        set_feedback(ui, "答对了！攻击 -1");
        wq_audio_play(WQ_AUDIO_ATTACK);
        start_sequence(ui, state,
                       wq_session_hp(&s_session) <= 0 ? WQ_SEQ_DEFEAT : WQ_SEQ_ATTACK);
    } else {
        const size_t correct_word = s_session.options[s_session.correct_slot];
        // Remember the miss so the ladder restarts and review returns to it.
        if (task) {
            wq_progress_mark_wrong(task->word);
            wq_progress_record_answer(task->word, false);
        }
        char text[96];
        snprintf(text, sizeof(text), "Boss 反击！正确答案:\n%s",
                 s_session.pack[correct_word].gloss);
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

    // One verified load of every persisted field. The wordbook index is only
    // trusted once the dictionary is open, and is clamped below if the stored
    // index does not exist in this build.
    wq_settings_load(&s_cfg);
    wq_progress_set_today((uint32_t)(wq_day_number() > 0 ? wq_day_number() : 0));

    if (wq_dict_init() != 0) {
        WQ_LOGE("dictionary load failed");
    }
    wq_settings_sanitize(&s_cfg, (uint32_t)wq_dict_book_count());
    const uint8_t book = (uint8_t)s_cfg.book;
    if (wq_progress_load(book, wq_dict_word_count(book)) != 0) {
        WQ_LOGE("progress load failed");
    }
#if defined(ESP_PLATFORM)
    // Headroom evidence: the retained layout keeps a legacy `factory` slot that
    // this application never boots from. Log the slot actually running so the
    // size warning about `factory` can be judged against reality.
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        WQ_LOGE("running partition: %s @0x%05x size %u KiB", running->label,
                (unsigned)running->address, (unsigned)(running->size / 1024));
    }
#endif
    WQ_LOGE("restored: book=%u daily=%u level=%u streak=%u",
            (unsigned)s_cfg.book, (unsigned)s_cfg.daily,
            (unsigned)s_cfg.level, (unsigned)s_cfg.streak);
    // Boot self-check: rewrite the whole record and verify it. This upgrades
    // records written by older builds (which only stored a subset of keys) and
    // turns a non-durable store into an explicit log line instead of a silently
    // forgotten wordbook choice.
    if (wq_settings_save(&s_cfg) == 0) {
        WQ_LOGE("settings verified on boot");
    } else {
        WQ_LOGE("settings NOT durable; choices will not survive a restart");
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
