// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Daily study history used by the weekly/monthly report. Records live in one
// small verified blob (last 31 days); the report itself is a pure summation.

typedef struct {
    uint32_t days;    // days with a finished session
    uint32_t learned; // new words finished
    uint32_t correct; // quiz answers correct
    uint32_t wrong;   // quiz answers wrong (includes "forgot")
} wq_stats_sum_t;

// Add one finished session to `day` (epoch day number). Returns 0 when the
// record was written and verified, -1 when the clock is unset or the store
// could not be made durable.
int wq_stats_add(uint32_t day, uint16_t learned, uint16_t correct, uint16_t wrong);

// Sum the last `days` days ending at `today` (today-6..today for a week).
void wq_stats_window(uint32_t today, uint32_t days, wq_stats_sum_t *out);
