// SPDX-License-Identifier: Apache-2.0
//
// Daily study history for the weekly/monthly report. One fixed-size record per
// day (last 31 days) written through wq_settings' verified blob path, so a
// dropped write is reported instead of silently flattening the report.

#include "wq_stats.h"

#include <string.h>

#include "wq_settings.h"

#define WQ_STATS_DAYS 31
#define WQ_STATS_KEY "hist"

// 12 bytes per day keeps the whole window in one small NVS blob.
typedef struct {
    uint32_t day;     // epoch day number (0 = empty slot)
    uint16_t learned; // new words finished that day
    uint16_t correct; // quiz answers scored correct
    uint16_t wrong;   // quiz answers scored wrong (including "forgot")
    uint16_t reserved;
} wq_day_stat_t;

typedef struct {
    wq_day_stat_t days[WQ_STATS_DAYS];
} wq_stats_blob_t;

static wq_stats_blob_t s_stats;

static uint16_t add_sat(uint16_t value, uint16_t delta)
{
    const uint32_t sum = (uint32_t)value + delta;
    return (uint16_t)(sum > 0xFFFFU ? 0xFFFFU : sum);
}
static bool s_loaded;

static void ensure_loaded(void)
{
    if (s_loaded) return;
    size_t len = 0;
    if (wq_settings_load_blob(WQ_STATS_KEY, &s_stats, sizeof(s_stats), &len) != 0) {
        memset(&s_stats, 0, sizeof(s_stats));
    }
    s_loaded = true;
}

static int slot_for(uint32_t day)
{
    int empty = -1;
    for (int i = 0; i < WQ_STATS_DAYS; ++i) {
        if (s_stats.days[i].day == day) return i;
        if (s_stats.days[i].day == 0 && empty < 0) empty = i;
    }
    if (empty >= 0) return empty;
    // Window is full: overwrite the oldest entry.
    int oldest = 0;
    for (int i = 1; i < WQ_STATS_DAYS; ++i) {
        if (s_stats.days[i].day < s_stats.days[oldest].day) oldest = i;
    }
    return oldest;
}

int wq_stats_add(uint32_t day, uint16_t learned, uint16_t correct, uint16_t wrong)
{
    if (day == 0) return -1; // clock not synced: do not invent a day
    ensure_loaded();
    const int slot = slot_for(day);
    wq_day_stat_t *d = &s_stats.days[slot];
    if (d->day != day) {
        memset(d, 0, sizeof(*d));
        d->day = day;
    }
    // Clamp (not skip) so a marathon session saturates instead of wrapping.
    d->learned = add_sat(d->learned, learned);
    d->correct = add_sat(d->correct, correct);
    d->wrong = add_sat(d->wrong, wrong);

    if (wq_settings_save_blob(WQ_STATS_KEY, &s_stats, sizeof(s_stats)) != 0) {
        return -1; // reported; in-RAM values still reflect this session
    }
    return 0;
}

void wq_stats_window(uint32_t today, uint32_t days, wq_stats_sum_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (today == 0 || days == 0) return;
    ensure_loaded();
    const uint32_t from = today >= days ? today - days + 1 : 0;
    for (int i = 0; i < WQ_STATS_DAYS; ++i) {
        const wq_day_stat_t *d = &s_stats.days[i];
        if (d->day == 0 || d->day < from || d->day > today) continue;
        out->days += 1;
        out->learned += d->learned;
        out->correct += d->correct;
        out->wrong += d->wrong;
    }
}
