// SPDX-License-Identifier: Apache-2.0
//
// Per-wordbook learning progress: a learned-bitmap (one bit per word) plus a
// cursor, persisted in NVS on device. The native simulator keeps progress in
// static memory only (no NVS there), so sessions start fresh.

#include "wq_progress.h"

#include <stdio.h>
#include <string.h>

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
    if (nvs_commit(nvs) != ESP_OK) {
        WQ_LOGE("nvs commit failed");
    }
    nvs_close(nvs);
}
#else
static void persist(void)
{
    // simulator: progress lives in RAM only
}
#endif

int wq_progress_load(uint8_t book, size_t total_words)
{
    if (total_words == 0 || total_words > WQ_BITMAP_MAX * 8) {
        WQ_LOGE("book too large (%u words)", (unsigned)total_words);
        return -1;
    }
    s_book = book;
    s_total = total_words;
    s_bitmap_bytes = (total_words + 7) / 8;
    s_learned = 0;
    s_start = 0;
    s_pick_cursor = 0;
    memset(s_bitmap, 0, sizeof(s_bitmap));

#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    if (nvs_open(WQ_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char key[12];
        size_t len = s_bitmap_bytes;
        snprintf(key, sizeof(key), "bm%u", (unsigned)book);
        if (nvs_get_blob(nvs, key, s_bitmap, &len) != ESP_OK || len != s_bitmap_bytes) {
            memset(s_bitmap, 0, s_bitmap_bytes);
        }
        uint32_t start = 0;
        snprintf(key, sizeof(key), "st%u", (unsigned)book);
        if (nvs_get_u32(nvs, key, &start) == ESP_OK && start < s_total) {
            s_start = start;
        }
        nvs_close(nvs);
    }
#endif
    for (size_t i = 0; i < s_total; ++i) {
        s_learned += (s_bitmap[i >> 3] >> (i & 7)) & 1;
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

size_t wq_progress_start(void)
{
    return s_start;
}

size_t wq_progress_pick_new(size_t n, size_t *out)
{
    size_t found = 0;
    for (size_t tried = 0; tried < s_total && found < n; ++tried) {
        size_t i = s_pick_cursor % s_total;
        if (!((s_bitmap[i >> 3] >> (i & 7)) & 1)) {
            out[found++] = i;
        }
        ++s_pick_cursor;
    }
    return found;
}

size_t wq_progress_pick_review(size_t n, size_t *out)
{
    if (s_learned == 0) {
        return 0;
    }
    size_t found = 0;
    uint32_t rng = (uint32_t)(s_start * 2654435761u + s_learned) | 1u;
    for (int tries = 0; tries < 400 && found < n; ++tries) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        size_t i = rng % s_total;
        if (!((s_bitmap[i >> 3] >> (i & 7)) & 1)) {
            continue;
        }
        bool dup = false;
        for (size_t k = 0; k < found; ++k) {
            if (out[k] == i) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out[found++] = i;
        }
    }
    return found;
}

void wq_progress_mark_learned(const size_t *idx, size_t n)
{
    bool changed = false;
    for (size_t k = 0; k < n; ++k) {
        size_t i = idx[k];
        if (i < s_total && !((s_bitmap[i >> 3] >> (i & 7)) & 1)) {
            s_bitmap[i >> 3] |= (uint8_t)(1 << (i & 7));
            ++s_learned;
            changed = true;
        }
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

size_t wq_progress_review_due(size_t cap)
{
    return s_learned < cap ? s_learned : cap;
}
