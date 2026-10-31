// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Small, durable application settings (wordbook choice, daily quota, streak).
// Every save writes, commits and then READS BACK each field: a silent NVS
// failure is what made the selected wordbook fall back to CET-4 after an
// update, so durability is verified rather than assumed.

#define WQ_DAILY_MIN 5U
#define WQ_DAILY_MAX 20U
#define WQ_DAILY_DEFAULT 10U

typedef struct {
    uint32_t book;       // selected wordbook index
    uint32_t music_on;   // 0/1
    uint32_t daily;      // new words per day, clamped to [WQ_DAILY_MIN, MAX]
    uint32_t level;      // adventure level, at least 1
    uint32_t streak;     // consecutive study days
    uint32_t last_day;   // day number of the last counted session
    uint32_t days_total; // total days with a finished session
} wq_settings_t;

// Field defaults for a blank device (book 0, music on, 10 words, level 1).
void wq_settings_defaults(wq_settings_t *out);

// Read settings, filling defaults for missing/invalid values. Always returns 0;
// `out` is usable even when nothing has ever been stored.
int wq_settings_load(wq_settings_t *out);

// Clamp values into their valid ranges. `book_count` is the number of books the
// dictionary actually exposes; 0 keeps the current book (unknown dictionary).
void wq_settings_sanitize(wq_settings_t *s, uint32_t book_count);

// Persist, then verify by reading every field back. Returns 0 when the stored
// values match what was written, -1 when the write could not be made durable.
int wq_settings_save(const wq_settings_t *s);

// Verified blob persistence for per-book bitmaps (favourites, mistakes).
// The byte count is fixed per key, so the read-back compares whole contents.
#define WQ_BLOB_MAX 1088
int wq_settings_save_blob(const char *key, const void *data, size_t len);
// Requires the stored blob to be exactly `cap` bytes.
int wq_settings_load_blob(const char *key, void *data, size_t cap, size_t *out_len);
