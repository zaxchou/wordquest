// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

// Per-wordbook learning progress persisted in NVS as a learned-bitmap plus a
// cursor. The bitmap has one bit per word; "斩掉" words get their bit set.
// Review candidates are randomly sampled from the learned set.

// Load (or initialize) progress for the selected book. total_words comes
// from the dictionary. Must be called after switching books.
int wq_progress_load(uint8_t book, size_t total_words);

uint8_t wq_progress_book(void);
size_t wq_progress_total(void);
size_t wq_progress_learned(void);
size_t wq_progress_start(void);

// Select the next `n` unlearned word indexes (cyclic scan from the cursor).
// Returns how many were found (may be < n when the book is nearly done).
size_t wq_progress_pick_new(size_t n, size_t *out);

// Randomly sample `n` learned word indexes (may return fewer).
size_t wq_progress_pick_review(size_t n, size_t *out);

// Mark word indexes as learned and persist the bitmap.
void wq_progress_mark_learned(const size_t *idx, size_t n);

// Persist the cursor after a completed session.
void wq_progress_commit_start(size_t start);

// Number of review candidates available today (capped).
size_t wq_progress_review_due(size_t cap);
