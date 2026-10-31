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
    for (size_t i = count - 1; i > 0; --i) {
        const size_t j = next_random(s) % (i + 1);
        const size_t tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

void wq_session_start(wq_session_t *s, const wq_word_entry_t *pack,
                      size_t pack_count, const size_t *new_words,
                      size_t new_count, const size_t *review_words,
                      size_t review_count, wq_task_t *tasks,
                      size_t task_capacity, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->pack = pack;
    s->pack_count = pack_count;
    s->tasks = tasks;
    s->task_capacity = task_capacity;
    s->rng = seed ? seed : 0x19451945U;

    if (pack_count == 0) {
        return;
    }
    if (new_count > WQ_NEW_WORDS_PER_DAY) {
        new_count = WQ_NEW_WORDS_PER_DAY;
    }
    if (new_count > pack_count) {
        new_count = pack_count;
    }
    if (review_count > WQ_NEW_WORDS_PER_DAY * 2U) {
        review_count = WQ_NEW_WORDS_PER_DAY * 2U;
    }
    for (size_t i = 0; i < review_count; ++i) {
        if (review_words[i] >= pack_count) {
            review_count = (size_t)i;
            break;
        }
    }

    size_t picked[WQ_NEW_WORDS_PER_DAY];
    for (size_t i = 0; i < new_count; ++i) {
        picked[i] = new_words[i];
    }
    shuffle_indices(picked, new_count, s);

    size_t quiz = 0;
    for (size_t i = 0; i < new_count; ++i) {
        s->tasks[s->task_count++] =
            (wq_task_t){WQ_TASK_LEARN, picked[i], false};
        s->tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, picked[i], false};
        quiz += 1;
    }
    // Shuffled review pass over the new words.
    size_t review[WQ_NEW_WORDS_PER_DAY];
    for (size_t i = 0; i < new_count; ++i) {
        review[i] = picked[i];
    }
    shuffle_indices(review, new_count, s);
    for (size_t i = 0; i < new_count; ++i) {
        s->tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, review[i], false};
        quiz += 1;
    }
    // Review of previously learned words (one shuffled quiz each).
    size_t rev2[WQ_NEW_WORDS_PER_DAY * 2U];
    for (size_t i = 0; i < review_count; ++i) {
        rev2[i] = review_words[i];
    }
    shuffle_indices(rev2, review_count, s);
    for (size_t i = 0; i < review_count; ++i) {
        s->tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, rev2[i], false};
        quiz += 1;
    }

    s->hp_max = (int)quiz;
    s->hp = s->hp_max;
    s->cursor = 0;
}

const wq_task_t *wq_session_current(const wq_session_t *s)
{
    if (s->cursor >= s->task_count) {
        return NULL;
    }
    return &s->tasks[s->cursor];
}

size_t wq_session_build_options(wq_session_t *s, size_t *slots)
{
    const wq_task_t *task = wq_session_current(s);
    if (!task || task->kind != WQ_TASK_QUIZ) {
        return 0;
    }

    slots[0] = task->word;
    size_t filled = 1;
    while (filled < WQ_OPTIONS) {
        const size_t candidate = next_random(s) % s->pack_count;
        bool duplicate = false;
        for (size_t i = 0; i < filled; ++i) {
            if (slots[i] == candidate) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            slots[filled++] = candidate;
        }
    }
    shuffle_indices(slots, WQ_OPTIONS, s);

    for (size_t i = 0; i < WQ_OPTIONS; ++i) {
        if (slots[i] == task->word) {
            s->correct_slot = i;
            break;
        }
    }
    s->answered = false;
    return s->correct_slot;
}

bool wq_session_answer(wq_session_t *s, size_t slot)
{
    wq_task_t *task = &s->tasks[s->cursor];
    if (s->cursor >= s->task_count || task->kind != WQ_TASK_QUIZ ||
        s->answered) {
        return false;
    }
    s->answered = true;
    if (slot == s->correct_slot) {
        if (s->hp > 0) {
            --s->hp;
        }
        ++s->correct_count;
        task->done = true;
        return true;
    }
    // Wrong: the task stays pending; try it again later in this session.
    // Boss max HP is unchanged — forgetting extends the fight, nothing more.
    ++s->wrong_count;
    task->done = true;
    if (s->task_count < s->task_capacity) {
        s->tasks[s->task_count++] = (wq_task_t){WQ_TASK_QUIZ, task->word, false};
    }
    return false;
}

bool wq_session_advance(wq_session_t *s)
{
    while (s->cursor < s->task_count && s->tasks[s->cursor].done) {
        ++s->cursor;
    }
    return s->cursor < s->task_count;
}

void wq_session_complete_current(wq_session_t *s)
{
    if (s->cursor < s->task_count) {
        s->tasks[s->cursor].done = true;
    }
}

size_t wq_session_done_count(const wq_session_t *s)
{
    size_t done = 0;
    for (size_t i = 0; i < s->task_count; ++i) {
        done += s->tasks[i].done ? 1U : 0U;
    }
    return done;
}

int wq_session_hp(const wq_session_t *s)
{
    return s->hp;
}

int wq_session_hp_max(const wq_session_t *s)
{
    return s->hp_max;
}

size_t wq_session_correct_count(const wq_session_t *s)
{
    return s->correct_count;
}

size_t wq_session_wrong_count(const wq_session_t *s)
{
    return s->wrong_count;
}
