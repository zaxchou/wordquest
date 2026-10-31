// SPDX-License-Identifier: Apache-2.0

#include "wq_model.h"

#include <string.h>

static uint32_t next_random(wq_session_t *s)
{
    // xorshift32: portable, deterministic given the seed.
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x ? x : 0x9e3779b9U;
    return s->rng;
}

static void shuffle_indices(size_t *values, size_t count, wq_session_t *s)
{
    if (count < 2) return;
    for (size_t i = count - 1; i > 0; --i) {
        const size_t j = next_random(s) % (i + 1);
        const size_t tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

static bool contains(const size_t *values, size_t count, size_t word)
{
    for (size_t i = 0; i < count; ++i) {
        if (values[i] == word) return true;
    }
    return false;
}

void wq_session_start(wq_session_t *s, const wq_word_entry_t *pack,
                      size_t pack_count, const size_t *new_words,
                      size_t new_count, const size_t *review_words,
                      size_t review_count, wq_task_t *tasks,
                      size_t task_capacity, uint32_t seed)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->pack = pack;
    s->pack_count = pack_count;
    s->tasks = tasks;
    s->task_capacity = task_capacity;
    s->rng = seed ? seed : 0x19451945U;
    s->correct_slot = WQ_OPTIONS;
    if (!pack || !pack_count || !tasks || !task_capacity) return;

    if (!new_words) new_count = 0;
    if (!review_words) review_count = 0;
    if (new_count > WQ_NEW_WORDS_PER_DAY) new_count = WQ_NEW_WORDS_PER_DAY;
    if (review_count > WQ_NEW_WORDS_PER_DAY * 2U) {
        review_count = WQ_NEW_WORDS_PER_DAY * 2U;
    }
    size_t picked[WQ_NEW_WORDS_PER_DAY];
    size_t picked_count = 0;
    for (size_t i = 0; i < new_count && picked_count < task_capacity / 3; ++i) {
        if (new_words[i] < pack_count && !contains(picked, picked_count, new_words[i])) {
            picked[picked_count++] = new_words[i];
        }
    }
    shuffle_indices(picked, picked_count, s);
    for (size_t i = 0; i < picked_count; ++i) {
        tasks[s->task_count++] = (wq_task_t){WQ_TASK_LEARN, picked[i], false};
        tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, picked[i], false};
    }
    shuffle_indices(picked, picked_count, s);
    for (size_t i = 0; i < picked_count; ++i) {
        tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, picked[i], false};
    }
    // Append valid distinct older words, then shuffle just that review pass.
    const size_t review_start = s->task_count;
    for (size_t i = 0; i < review_count && s->task_count < task_capacity; ++i) {
        const size_t word = review_words[i];
        if (word >= pack_count || contains(picked, picked_count, word)) continue;
        bool duplicate = false;
        for (size_t j = review_start; j < s->task_count; ++j) {
            if (tasks[j].word == word) duplicate = true;
        }
        if (!duplicate) tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, word, false};
    }
    for (size_t i = s->task_count - review_start; i > 1; --i) {
        const size_t j = review_start + next_random(s) % i;
        const size_t last = review_start + i - 1;
        const wq_task_t tmp = tasks[last];
        tasks[last] = tasks[j];
        tasks[j] = tmp;
    }
    s->hp_max = (int)(picked_count * 2 + s->task_count - review_start);
    s->hp = s->hp_max;
}

const wq_task_t *wq_session_current(const wq_session_t *s)
{
    if (!s || !s->tasks || s->cursor >= s->task_count) return NULL;
    return &s->tasks[s->cursor];
}

size_t wq_session_build_options(wq_session_t *s, size_t *slots)
{
    const wq_task_t *task = wq_session_current(s);
    if (!task || task->kind != WQ_TASK_QUIZ || !slots || !s->pack_count ||
        task->word >= s->pack_count) return WQ_OPTIONS;

    // Repainting a quiz must neither reshuffle it nor allow it to be scored again.
    if (s->correct_slot < WQ_OPTIONS) {
        if (slots != s->options) memcpy(slots, s->options, sizeof(s->options));
        return s->correct_slot;
    }
    s->options[0] = task->word;
    const size_t distinct = s->pack_count < WQ_OPTIONS ? s->pack_count : WQ_OPTIONS;
    for (size_t filled = 1; filled < distinct; ++filled) {
        size_t candidate = next_random(s) % s->pack_count;
        while (contains(s->options, filled, candidate)) {
            candidate = (candidate + 1) % s->pack_count;
        }
        s->options[filled] = candidate;
    }
    // The fixed four-button UI needs valid indices even for tiny dictionaries.
    // Repeat distractors where possible; a one-word pack has four correct buttons.
    for (size_t i = distinct; i < WQ_OPTIONS; ++i) {
        s->options[i] = distinct > 1 ? s->options[1 + (i - distinct) % (distinct - 1)]
                                     : task->word;
    }
    shuffle_indices(s->options, WQ_OPTIONS, s);
    for (size_t i = 0; i < WQ_OPTIONS; ++i) {
        if (s->options[i] == task->word) {
            s->correct_slot = i;
            break;
        }
    }
    if (slots != s->options) memcpy(slots, s->options, sizeof(s->options));
    return s->correct_slot;
}

bool wq_session_answer(wq_session_t *s, size_t slot)
{
    const wq_task_t *current = wq_session_current(s);
    if (!current || current->kind != WQ_TASK_QUIZ || current->done || s->answered ||
        s->correct_slot >= WQ_OPTIONS || slot > WQ_OPTIONS) return false;

    wq_task_t *task = &s->tasks[s->cursor];
    s->answered = true;
    if (slot < WQ_OPTIONS && s->options[slot] == task->word) {
        if (s->hp > 0) --s->hp;
        ++s->correct_count;
        task->done = true;
        return true;
    }
    ++s->wrong_count;
    if (s->task_count < s->task_capacity) {
        const size_t word = task->word;
        task->done = true;
        s->tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, word, false};
    }
    // At capacity, retain this pending quiz. advance() opens its next attempt.
    return false;
}

bool wq_session_advance(wq_session_t *s)
{
    if (!s) return false;
    const size_t previous = s->cursor;
    while (s->cursor < s->task_count && s->tasks[s->cursor].done) ++s->cursor;
    if (s->cursor != previous || s->answered) {
        s->answered = false;
        s->correct_slot = WQ_OPTIONS;
    }
    return s->cursor < s->task_count;
}

void wq_session_complete_current(wq_session_t *s)
{
    const wq_task_t *task = wq_session_current(s);
    if (task && task->kind == WQ_TASK_LEARN) s->tasks[s->cursor].done = true;
}

size_t wq_session_done_count(const wq_session_t *s)
{
    size_t done = 0;
    for (size_t i = 0; i < s->task_count; ++i) done += s->tasks[i].done ? 1U : 0U;
    return done;
}

int wq_session_hp(const wq_session_t *s) { return s->hp; }
int wq_session_hp_max(const wq_session_t *s) { return s->hp_max; }
size_t wq_session_correct_count(const wq_session_t *s) { return s->correct_count; }
size_t wq_session_wrong_count(const wq_session_t *s) { return s->wrong_count; }
