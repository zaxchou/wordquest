// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Per-wordbook learning progress persisted in NVS as a learned-bitmap plus a
// cursor. The bitmap has one bit per word; "斩掉" words get their bit set.
// Review candidates are randomly sampled from the learned set. This singleton
// is owned by the UI task; native simulation retains each book in process RAM.

// Load (or initialize) progress for the selected book. total_words comes
// from the dictionary. Must be called after switching books.
int wq_progress_load(uint8_t book, size_t total_words);

uint8_t wq_progress_book(void);
size_t wq_progress_total(void);
size_t wq_progress_learned(void);
bool wq_progress_is_learned(size_t idx);
size_t wq_progress_start(void);

// Select the next `n` unlearned word indexes (cyclic scan from the cursor).
// Returns how many were found (may be < n when the book is nearly done).
size_t wq_progress_pick_new(size_t n, size_t *out);

// Randomly sample min(n, learned) distinct learned word indexes.
size_t wq_progress_pick_review(size_t n, size_t *out);

// Mark word indexes as learned and persist the bitmap.
void wq_progress_mark_learned(const size_t *idx, size_t n);

// Flag a word answered wrong (stubborn). Stubborn words take the first review
// slots, so mistakes come back tomorrow instead of disappearing.
void wq_progress_mark_wrong(size_t idx);
void wq_progress_clear_wrong(size_t idx); // recalled correctly: graduate
size_t wq_progress_wrong_count(void);

// Favourites (生词本): user-saved words, stored as a verified blob per book.
// Toggling returns the new state so the UI can echo it immediately.
bool wq_progress_toggle_fav(size_t idx);
bool wq_progress_is_fav(size_t idx);
size_t wq_progress_fav_count(void);
// Copy up to `n` favourite indexes (ascending order).
size_t wq_progress_pick_favs(size_t n, size_t *out);

// Skipped words (生僻词): kept out of the new-word pool but still listed in the
// word browser. Toggling returns the new state.
bool wq_progress_toggle_skip(size_t idx);
bool wq_progress_is_skipped(size_t idx);
size_t wq_progress_skip_count(void);

// Persist the cursor after a completed session.
void wq_progress_commit_start(size_t start);

// Number of review candidates available today (capped).
size_t wq_progress_review_due(size_t cap);

// Spaced repetition (1/3/7-day ladder). The clock is supplied by the UI: day 0
// means "unknown", and selection then behaves like a plain random review.
void wq_progress_set_today(uint32_t day);
uint8_t wq_progress_stage(size_t idx);
// Feed one graded answer back into the ladder (correct promotes, wrong resets).
void wq_progress_record_answer(size_t idx, bool correct);
// Words whose interval has elapsed (bounded by learned).
size_t wq_progress_due_count(void);
// Call once after a review batch was served so stages rest until their interval.
void wq_progress_advance_schedule(void);
