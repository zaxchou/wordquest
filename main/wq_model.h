// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "word_pack.h"

// Daily boss-battle session: learn new words, then prove recall.
// One correct quiz answer deals one point of boss damage. Wrong answers
// re-queue the word without changing boss HP, so forgetting is never
// punished twice.

// Hard cap on daily new words: task arrays are sized from this. The user's
// configured quota (5..20, UI layer) must never exceed it.
#define WQ_NEW_WORDS_PER_DAY 20U
#define WQ_OPTIONS 4U

typedef enum {
    WQ_TASK_LEARN = 0,
    WQ_TASK_QUIZ,
} wq_task_kind_t;

typedef struct {
    wq_task_kind_t kind;
    size_t word; // index into the word pack
    bool done;
} wq_task_t;

typedef struct {
    const wq_word_entry_t *pack;
    size_t pack_count;
    wq_task_t *tasks;
    size_t task_capacity;
    size_t task_count;
    size_t cursor;
    int hp;
    int hp_max;
    size_t options[WQ_OPTIONS];   // word indices for the current quiz
    size_t correct_slot;
    bool answered;                // current quiz already scored
    size_t correct_count;         // quiz answers scored correct this session
    size_t wrong_count;           // quiz answers scored wrong this session
    uint32_t rng;
} wq_session_t;

// Build a session: `new_count` words get one learn task each plus quiz
// tasks (immediate recall + shuffled review pass); `review_count` learned
// words get one shuffled quiz each. Boss HP equals the number of quiz
// tasks. Task storage is caller-owned. Invalid/duplicate indices are skipped;
// only complete three-task new-word groups that fit the capacity are included.
// Mutable session state belongs to the calling UI task.
void wq_session_start(wq_session_t *s, const wq_word_entry_t *pack,
                      size_t pack_count, const size_t *new_words,
                      size_t new_count, const size_t *review_words,
                      size_t review_count, wq_task_t *tasks,
                      size_t task_capacity, uint32_t seed);

// Current task, or NULL when every task is done (boss defeated).
const wq_task_t *wq_session_current(const wq_session_t *s);

// Fill `slots` and s->options with the current quiz's cached word indices.
// Return the correct slot, or WQ_OPTIONS when no quiz can be built. Distractors
// are distinct for packs of >= WQ_OPTIONS words; smaller packs repeat valid
// indices. Repainting preserves the options and the already-answered state.
size_t wq_session_build_options(wq_session_t *s, size_t *slots);

// Score the current quiz task. Returns true on a correct answer (boss
// takes damage); a wrong answer reveals nothing here and re-queues the
// task. WQ_OPTIONS means "forgot"; larger slots are ignored. Repeated calls on
// the same attempt are ignored. At capacity the quiz stays pending until retry.
bool wq_session_answer(wq_session_t *s, size_t slot);

// Move past completed tasks, or reopen an answered pending quiz for retry.
// Returns false when the session is complete.
bool wq_session_advance(wq_session_t *s);

// Mark the current task done without scoring (learn cards).
void wq_session_complete_current(wq_session_t *s);

// Number of finished tasks in this session (for the daily progress readout).
size_t wq_session_done_count(const wq_session_t *s);

int wq_session_hp(const wq_session_t *s);
int wq_session_hp_max(const wq_session_t *s);

// Quiz answer tallies for the day summary (wrong includes "forgot").
size_t wq_session_correct_count(const wq_session_t *s);
size_t wq_session_wrong_count(const wq_session_t *s);
