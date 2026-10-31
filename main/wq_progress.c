// SPDX-License-Identifier: Apache-2.0
//
// Per-wordbook learning progress: a learned-bitmap (one bit per word) plus a
// cursor, persisted in NVS on device. The native simulator keeps progress in
// process memory only (no NVS there), surviving book switches until exit.

#include "wq_progress.h"

#include "wq_settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#if !defined(ESP_PLATFORM)
#include <stdlib.h>
#endif

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "nvs.h"
static const char *TAG = "wq_prog";
#define WQ_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#else
#define WQ_LOGE(...) fprintf(stderr, "E wq_prog: "), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#endif

#define WQ_NVS_NAMESPACE "wq_prog"
#define WQ_BITMAP_MAX 5200 // headroom for ~41k words

static uint8_t s_book;
static size_t s_total;
static size_t s_bitmap_bytes;
static size_t s_learned;
static size_t s_start;
static size_t s_pick_cursor;
static uint8_t s_bitmap[WQ_BITMAP_MAX];
// Stubborn words: answered wrong at least once. Review serves these first,
// which is what turns "already seen" into "actually remembered".
static uint8_t s_wrong[WQ_BITMAP_MAX];
static size_t s_wrong_count;
// Favourites: words the user explicitly saved. Kept through the verified blob
// path so a dropped write cannot silently empty the list.
static uint8_t s_fav[WQ_BITMAP_MAX];
static size_t s_fav_count;
// Words the user chose to skip (rare or known already): never picked as new
// words, but still visible in the word list.
static uint8_t s_skip[WQ_BITMAP_MAX];
static size_t s_skip_count;
// Spaced repetition: 2 bits of stage per word (0..2) plus the day each stage
// becomes due again. Stage intervals follow the classic 1/3/7-day ladder.
#define WQ_SRS_STAGES 3
#define WQ_SRS_MAGIC 0x31535253u // "SRS1"
typedef struct {
    uint32_t magic;
    uint16_t total;
    uint16_t reserved;
    uint32_t due_day[WQ_SRS_STAGES];
    uint8_t stages[1012]; // 2 bits per word: enough for 4048 words
} wq_srs_t;
static wq_srs_t s_srs;
static uint32_t s_today; // 0 = clock unknown, fall back to plain review
static const uint16_t WQ_SRS_INTERVAL[WQ_SRS_STAGES] = {1, 3, 7};

static uint8_t srs_stage(size_t idx)
{
    const unsigned shift = (unsigned)((idx & 3) * 2);
    return (uint8_t)((s_srs.stages[idx >> 2] >> shift) & 3U);
}

static void srs_set_stage(size_t idx, uint8_t stage)
{
    const unsigned shift = (unsigned)((idx & 3) * 2);
    s_srs.stages[idx >> 2] = (uint8_t)((s_srs.stages[idx >> 2] & ~(3U << shift)) |
                                       ((unsigned)stage << shift));
}

// A word is due when the clock is known and its stage interval has elapsed.
static bool srs_due(size_t idx)
{
    if (s_today == 0) return true; // no clock: serve everything, as before
    return s_srs.due_day[srs_stage(idx)] <= s_today;
}

#if defined(ESP_PLATFORM)
static void persist(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WQ_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        WQ_LOGE("nvs open failed; progress not saved");
        return;
    }
    char key[12];
    snprintf(key, sizeof(key), "bm%u", (unsigned)s_book);
    if (nvs_set_blob(nvs, key, s_bitmap, s_bitmap_bytes) != ESP_OK) {
        WQ_LOGE("bitmap save failed");
    }
    snprintf(key, sizeof(key), "st%u", (unsigned)s_book);
    if (nvs_set_u32(nvs, key, (uint32_t)s_start) != ESP_OK) {
        WQ_LOGE("cursor save failed");
    }
    snprintf(key, sizeof(key), "wr%u", (unsigned)s_book);
    if (nvs_set_blob(nvs, key, s_wrong, s_bitmap_bytes) != ESP_OK) {
        WQ_LOGE("stubborn bitmap save failed");
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    // Favourites use the verified path: a lost write must be reported, not
    // silently empty the user's saved words.
    snprintf(key, sizeof(key), "fav%u", (unsigned)s_book);
    if (wq_settings_save_blob(key, s_fav, s_bitmap_bytes) != 0) {
        WQ_LOGE("favourites save did not verify");
    }
    snprintf(key, sizeof(key), "srs%u", (unsigned)s_book);
    if (wq_settings_save_blob(key, &s_srs, sizeof(s_srs)) != 0) {
        WQ_LOGE("review schedule save did not verify");
    }
    snprintf(key, sizeof(key), "skip%u", (unsigned)s_book);
    if (wq_settings_save_blob(key, s_skip, s_bitmap_bytes) != 0) {
        WQ_LOGE("skip bitmap save did not verify");
    }
}
#else
// Single UI-task owner, matching the device module. Simulator records live for
// the process lifetime and are isolated by book, just like the NVS keys.
typedef struct {
    size_t total;
    size_t start;
    uint8_t bitmap[]; // learned, stubborn, favourite, skipped bits
} wq_ram_book_t;
static wq_ram_book_t *s_books[UINT8_MAX + 1U];

static void persist(void)
{
    wq_ram_book_t *saved = s_books[s_book];
    if (saved && saved->total == s_total) {
        saved->start = s_start;
        memcpy(saved->bitmap, s_bitmap, s_bitmap_bytes);
        memcpy(saved->bitmap + s_bitmap_bytes, s_wrong, s_bitmap_bytes);
        memcpy(saved->bitmap + 2 * s_bitmap_bytes, s_fav, s_bitmap_bytes);
        memcpy(saved->bitmap + 3 * s_bitmap_bytes, s_skip, s_bitmap_bytes);
        char srs_key[12];
        snprintf(srs_key, sizeof(srs_key), "srs%u", (unsigned)s_book);
        (void)wq_settings_save_blob(srs_key, &s_srs, sizeof(s_srs));
    }
}
#endif

int wq_progress_load(uint8_t book, size_t total_words)
{
    if (total_words == 0 || total_words > WQ_BITMAP_MAX * 8) {
        WQ_LOGE("book too large (%u words)", (unsigned)total_words);
        return -1;
    }
#if !defined(ESP_PLATFORM)
    wq_ram_book_t *saved = s_books[book];
    if (!saved || saved->total != total_words) {
        wq_ram_book_t *replacement = calloc(1, sizeof(*replacement) +
                                           4 * ((total_words + 7) / 8));
        if (!replacement) {
            WQ_LOGE("progress allocation failed");
            return -1;
        }
        replacement->total = total_words;
        free(saved);
        s_books[book] = saved = replacement;
    }
#endif
    s_book = book;
    s_total = total_words;
    s_bitmap_bytes = (total_words + 7) / 8;
    s_learned = 0;
    s_wrong_count = 0;
    s_fav_count = 0;
    s_skip_count = 0;
    s_start = 0;
    s_pick_cursor = 0;
    memset(s_bitmap, 0, sizeof(s_bitmap));
    memset(s_wrong, 0, sizeof(s_wrong));
    memset(s_fav, 0, sizeof(s_fav));
    memset(s_skip, 0, sizeof(s_skip));

#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    if (nvs_open(WQ_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char key[12];
        size_t len = s_bitmap_bytes;
        snprintf(key, sizeof(key), "bm%u", (unsigned)book);
        if (nvs_get_blob(nvs, key, s_bitmap, &len) != ESP_OK || len != s_bitmap_bytes) {
            memset(s_bitmap, 0, s_bitmap_bytes);
        }
        len = s_bitmap_bytes;
        snprintf(key, sizeof(key), "wr%u", (unsigned)book);
        if (nvs_get_blob(nvs, key, s_wrong, &len) != ESP_OK || len != s_bitmap_bytes) {
            memset(s_wrong, 0, s_bitmap_bytes);
        }
        size_t fav_len = s_bitmap_bytes;
        snprintf(key, sizeof(key), "fav%u", (unsigned)book);
        if (wq_settings_load_blob(key, s_fav, fav_len, &fav_len) != 0) {
            memset(s_fav, 0, s_bitmap_bytes);
        }
        size_t skip_len = s_bitmap_bytes;
        snprintf(key, sizeof(key), "skip%u", (unsigned)book);
        if (wq_settings_load_blob(key, s_skip, skip_len, &skip_len) != 0) {
            memset(s_skip, 0, s_bitmap_bytes);
        }
        size_t srs_len = sizeof(s_srs);
        snprintf(key, sizeof(key), "srs%u", (unsigned)book);
        if (wq_settings_load_blob(key, &s_srs, srs_len, &srs_len) != 0 ||
            s_srs.magic != WQ_SRS_MAGIC || s_srs.total != (uint16_t)total_words) {
            memset(&s_srs, 0, sizeof(s_srs));
            s_srs.magic = WQ_SRS_MAGIC;
            s_srs.total = (uint16_t)total_words;
        }
        uint32_t start = 0;
        snprintf(key, sizeof(key), "st%u", (unsigned)book);
        if (nvs_get_u32(nvs, key, &start) == ESP_OK && start < s_total) {
            s_start = start;
        }
        nvs_close(nvs);
    }
#else
    memcpy(s_bitmap, saved->bitmap, s_bitmap_bytes);
    memcpy(s_wrong, saved->bitmap + s_bitmap_bytes, s_bitmap_bytes);
    memcpy(s_fav, saved->bitmap + 2 * s_bitmap_bytes, s_bitmap_bytes);
    memcpy(s_skip, saved->bitmap + 3 * s_bitmap_bytes, s_bitmap_bytes);
    {
        char srs_key[12];
        size_t len = sizeof(s_srs);
        snprintf(srs_key, sizeof(srs_key), "srs%u", (unsigned)book);
        if (wq_settings_load_blob(srs_key, &s_srs, len, &len) != 0 ||
            s_srs.magic != WQ_SRS_MAGIC || s_srs.total != (uint16_t)total_words) {
            memset(&s_srs, 0, sizeof(s_srs));
            s_srs.magic = WQ_SRS_MAGIC;
            s_srs.total = (uint16_t)total_words;
        }
    }
    s_start = saved->start;
#endif
    s_pick_cursor = s_start;
    for (size_t i = 0; i < s_total; ++i) {
        s_learned += (s_bitmap[i >> 3] >> (i & 7)) & 1;
        s_wrong_count += (s_wrong[i >> 3] >> (i & 7)) & 1;
        s_fav_count += (s_fav[i >> 3] >> (i & 7)) & 1;
        s_skip_count += (s_skip[i >> 3] >> (i & 7)) & 1;
    }
    return 0;
}

uint8_t wq_progress_book(void)
{
    return s_book;
}

size_t wq_progress_total(void)
{
    return s_total;
}

size_t wq_progress_learned(void)
{
    return s_learned;
}

bool wq_progress_is_learned(size_t idx)
{
    return idx < s_total && ((s_bitmap[idx >> 3] >> (idx & 7)) & 1);
}

size_t wq_progress_start(void)
{
    return s_start;
}

size_t wq_progress_pick_new(size_t n, size_t *out)
{
    if (!out) return 0;
    size_t found = 0;
    for (size_t tried = 0; tried < s_total && found < n; ++tried) {
        const size_t i = s_pick_cursor;
        // Advance first: a skipped word must not stall the scan.
        s_pick_cursor = (s_pick_cursor + 1) % s_total;
        if ((s_skip[i >> 3] >> (i & 7)) & 1) continue; // user skipped it
        if (!((s_bitmap[i >> 3] >> (i & 7)) & 1)) {
            out[found++] = i;
        }
    }
    return found;
}

size_t wq_progress_pick_review(size_t n, size_t *out)
{
    if (!out || !n || !s_learned) return 0;
    if (n > s_learned) n = s_learned;
    size_t found = 0;
    uint32_t rng = (uint32_t)(s_start * 2654435761u + s_learned) | 1u;
    // Stubborn words first when their interval has elapsed: a word the user got
    // wrong is the most valuable thing to serve again, but re-serving it every
    // day would crowd out the rest of the ladder.
    for (size_t i = 0; i < s_total && found < n; ++i) {
        if (((s_wrong[i >> 3] >> (i & 7)) & 1) &&
            ((s_bitmap[i >> 3] >> (i & 7)) & 1) && srs_due(i)) {
            out[found++] = i;
        }
    }
    size_t seen = 0;
    const size_t stubborn_n = found;
    // Reservoir sampling visits every learned word once: sparse books always
    // supply min(n, learned) distinct reviews without a rejection-attempt cap.
    for (size_t i = 0; i < s_total && found < n; ++i) {
        if (!((s_bitmap[i >> 3] >> (i & 7)) & 1)) continue;
        if ((s_wrong[i >> 3] >> (i & 7)) & 1) continue; // already queued
        if (!srs_due(i)) continue;                      // still resting
        ++seen;
        if (found < n) {
            out[found++] = i;
        } else {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            size_t slot = rng % seen;
            if (slot + stubborn_n < n) out[slot + stubborn_n] = i;
        }
    }
    // Shuffle only the optional tail so the stubborn words keep the front.
    for (size_t i = found; i > stubborn_n + 1; --i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const size_t j = stubborn_n + rng % (i - stubborn_n);
        const size_t tmp = out[i - 1];
        out[i - 1] = out[j];
        out[j] = tmp;
    }
    return found;
}

void wq_progress_mark_learned(const size_t *idx, size_t n)
{
    if (!idx) return;
    bool changed = false;
    for (size_t k = 0; k < n; ++k) {
        size_t i = idx[k];
        if (i < s_total && !((s_bitmap[i >> 3] >> (i & 7)) & 1)) {
            s_bitmap[i >> 3] |= (uint8_t)(1 << (i & 7));
            ++s_learned;
            changed = true;
        }
    }
    if (changed && s_today > 0) {
        // A word learned today enters the ladder at stage 0, whose first review
        // is tomorrow — the same-session quiz already covered today.
        if (s_srs.due_day[0] <= s_today) s_srs.due_day[0] = s_today + 1;
    }
    if (changed) {
        persist();
    }
}

void wq_progress_commit_start(size_t start)
{
    if (s_total) {
        s_start = start % s_total;
    }
    s_pick_cursor = s_start;
    persist();
}

void wq_progress_mark_wrong(size_t idx)
{
    if (idx >= s_total || ((s_wrong[idx >> 3] >> (idx & 7)) & 1)) return;
    s_wrong[idx >> 3] |= (uint8_t)(1 << (idx & 7));
    ++s_wrong_count;
    persist();
}

// Recall succeeded: the word graduates off the stubborn list, so mistakes do
// not pile up forever and keep starving other review candidates.
void wq_progress_clear_wrong(size_t idx)
{
    if (idx >= s_total || !((s_wrong[idx >> 3] >> (idx & 7)) & 1)) return;
    s_wrong[idx >> 3] &= (uint8_t)~(1 << (idx & 7));
    if (s_wrong_count) --s_wrong_count;
    persist();
}

size_t wq_progress_wrong_count(void)
{
    return s_wrong_count;
}

// Toggle a favourite. Returns the new state (true = saved).
bool wq_progress_toggle_fav(size_t idx)
{
    if (idx >= s_total) return false;
    const uint8_t bit = (uint8_t)(1 << (idx & 7));
    const bool now = !((s_fav[idx >> 3] & bit) != 0);
    if (now) {
        s_fav[idx >> 3] |= bit;
        ++s_fav_count;
    } else {
        s_fav[idx >> 3] &= (uint8_t)~bit;
        if (s_fav_count) --s_fav_count;
    }
    persist();
    return now;
}

bool wq_progress_is_skipped(size_t idx)
{
    return idx < s_total && ((s_skip[idx >> 3] >> (idx & 7)) & 1);
}

bool wq_progress_toggle_skip(size_t idx)
{
    if (idx >= s_total) return false;
    const uint8_t bit = (uint8_t)(1 << (idx & 7));
    const bool now = !((s_skip[idx >> 3] & bit) != 0);
    if (now) {
        s_skip[idx >> 3] |= bit;
        ++s_skip_count;
    } else {
        s_skip[idx >> 3] &= (uint8_t)~bit;
        if (s_skip_count) --s_skip_count;
    }
    persist();
    return now;
}

size_t wq_progress_skip_count(void)
{
    return s_skip_count;
}

bool wq_progress_is_fav(size_t idx)
{
    return idx < s_total && ((s_fav[idx >> 3] >> (idx & 7)) & 1);
}

size_t wq_progress_fav_count(void)
{
    return s_fav_count;
}

// Copy up to `n` favourite indexes (ascending). Returns how many were copied.
size_t wq_progress_pick_favs(size_t n, size_t *out)
{
    if (!out || !n || !s_fav_count) return 0;
    size_t found = 0;
    for (size_t i = 0; i < s_total && found < n; ++i) {
        if ((s_fav[i >> 3] >> (i & 7)) & 1) out[found++] = i;
    }
    return found;
}

size_t wq_progress_review_due(size_t cap)
{
    return s_learned < cap ? s_learned : cap;
}

void wq_progress_set_today(uint32_t day)
{
    s_today = day;
}

void wq_progress_record_answer(size_t idx, bool correct)
{
    if (idx >= s_total) return;
    if (!((s_bitmap[idx >> 3] >> (idx & 7)) & 1)) return; // not learned yet
    uint8_t stage = srs_stage(idx);
    if (correct) {
        if (stage + 1 < WQ_SRS_STAGES) ++stage;
    } else {
        stage = 0; // a miss restarts the ladder
    }
    srs_set_stage(idx, stage);
    persist();
}

size_t wq_progress_due_count(void)
{
    size_t due = 0;
    for (size_t i = 0; i < s_total; ++i) {
        if (((s_bitmap[i >> 3] >> (i & 7)) & 1) && srs_due(i)) ++due;
    }
    return due;
}

void wq_progress_advance_schedule(void)
{
    if (s_today == 0) return;
    // The batch just served has been handled: push each stage's next due day out
    // so tomorrow's session works on the next rung of the ladder.
    for (size_t st = 0; st < WQ_SRS_STAGES; ++st) {
        if (s_srs.due_day[st] <= s_today) {
            s_srs.due_day[st] = s_today + WQ_SRS_INTERVAL[st];
        }
    }
    persist();
}

uint8_t wq_progress_stage(size_t idx)
{
    return idx < s_total ? srs_stage(idx) : 0;
}
