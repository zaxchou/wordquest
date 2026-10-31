// SPDX-License-Identifier: Apache-2.0
// Standalone native regression test: compile with main/wq_model.c and
// main/wq_progress.c, adding main/ to the include path. No ESP/GSP dependency.
#include "wq_model.h"
#include "wq_progress.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const wq_word_entry_t pack[64] = {{0}};

static void assert_options(wq_session_t *session, size_t pack_count)
{
    size_t slots[WQ_OPTIONS];
    const size_t correct = wq_session_build_options(session, slots);
    assert(correct < WQ_OPTIONS);
    assert(!memcmp(slots, session->options, sizeof(slots)));
    assert(slots[correct] == wq_session_current(session)->word);
    for (size_t i = 0; i < WQ_OPTIONS; ++i) {
        assert(slots[i] < pack_count);
        if (pack_count >= WQ_OPTIONS) {
            for (size_t j = 0; j < i; ++j) assert(slots[i] != slots[j]);
        }
    }
    const uint32_t rng = session->rng;
    size_t repaint[WQ_OPTIONS];
    assert(wq_session_build_options(session, repaint) == correct);
    assert(!memcmp(slots, repaint, sizeof(slots)));
    assert(session->rng == rng);
}

static void finish_session(wq_session_t *session, bool make_mistakes)
{
    size_t steps = 0;
    const int hp = wq_session_hp(session);
    bool wrong = false;
    while (wq_session_current(session)) {
        assert(++steps < 512);
        if (wq_session_current(session)->kind == WQ_TASK_LEARN) {
            wq_session_complete_current(session);
        } else {
            assert_options(session, session->pack_count);
            const size_t correct = session->correct_slot;
            if (make_mistakes && !wrong) {
                assert(!wq_session_answer(session, WQ_OPTIONS));
                assert(session->answered);
                assert(wq_session_wrong_count(session) == 1);
                assert(wq_session_hp(session) == hp);
                wrong = true;
            } else {
                // complete_current may only bypass a learn card, never a quiz.
                wq_session_complete_current(session);
                assert(!wq_session_current(session)->done);
                assert(wq_session_answer(session, correct));
            }
            const int remaining = session->hp;
            const size_t correct_count = session->correct_count;
            const size_t wrong_count = session->wrong_count;
            size_t repaint[WQ_OPTIONS];
            assert(wq_session_build_options(session, repaint) == correct);
            assert(session->answered);
            assert(!wq_session_answer(session, correct));
            assert(session->hp == remaining);
            assert(session->correct_count == correct_count);
            assert(session->wrong_count == wrong_count);
        }
        wq_session_advance(session);
    }
    assert(wq_session_hp(session) == 0);
    assert(wq_session_correct_count(session) == (size_t)hp);
    assert(wq_session_done_count(session) == session->task_count);
    assert(!wq_session_answer(session, 0));
    assert(!wq_session_advance(session));
}

static void test_session_flow(void)
{
    const size_t new_words[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const size_t review_words[] = {20, 21, 22};
    wq_task_t tasks[64];
    wq_session_t session;
    wq_session_start(&session, pack, 64, new_words, 10, review_words, 3,
                     tasks, 64, 123);
    assert(session.task_count == 33);
    assert(session.hp_max == 23);
    assert(wq_session_build_options(&session, session.options) == WQ_OPTIONS);
    assert(!wq_session_answer(&session, 0));
    finish_session(&session, true);
    assert(session.wrong_count == 1);
    assert(session.task_count == 34);
}

static void test_capacity_and_indices(void)
{
    const size_t new_words[] = {99, 2, 2, 4, 6, 64, 8};
    const size_t review_words[] = {99, 2, 12, 12, 14, 64, 16};
    struct {
        wq_task_t before;
        wq_task_t tasks[24];
        wq_task_t after;
    } storage;
    wq_session_t session;
    for (size_t capacity = 0; capacity <= 24; ++capacity) {
        memset(&storage, 0xa5, sizeof(storage));
        unsigned char before[sizeof(storage.before)];
        unsigned char boundary[sizeof(storage.before)];
        memcpy(before, &storage.before, sizeof(before));
        memcpy(boundary, &storage.tasks[capacity], sizeof(boundary));
        wq_session_start(&session, pack, 64, new_words, 7, review_words, 7,
                         storage.tasks, capacity, 1);
        assert(session.task_count <= capacity);
        assert(!memcmp(before, &storage.before, sizeof(before)));
        assert(!memcmp(boundary, &storage.tasks[capacity], sizeof(boundary)));
        for (size_t i = 0; i < session.task_count; ++i) assert(storage.tasks[i].word < 64);
        finish_session(&session, true);
    }
    wq_session_start(&session, pack, 64, new_words, 7, review_words, 7,
                     storage.tasks, 24, 1);
    // Four distinct valid new words and three distinct older words.
    assert(session.task_count == 15);
    assert(session.hp == 11);
    wq_session_start(&session, NULL, 64, new_words, 7, NULL, 7, storage.tasks, 24, 1);
    assert(!session.task_count);
    wq_session_start(&session, pack, 64, NULL, 7, NULL, 7, NULL, 24, 1);
    assert(!session.task_count);
    wq_session_start(&session, pack, 0, new_words, 7, NULL, 0, storage.tasks, 24, 1);
    assert(!session.task_count);
}

static void test_small_packs_and_retry(void)
{
    wq_session_t session;
    wq_task_t tasks[1];
    for (size_t count = 1; count <= 8; ++count) {
        for (uint32_t seed = 0; seed < 100; ++seed) {
            const size_t word = seed % count;
            wq_session_start(&session, pack, count, NULL, 0, &word, 1, tasks, 1, seed);
            assert_options(&session, count);
            assert(!wq_session_answer(&session, WQ_OPTIONS + 1));
            assert(!session.answered);
            // Exercise both an incorrect button and the explicit forgot action.
            size_t wrong_slot = WQ_OPTIONS;
            if (count > 1 && seed % 2) {
                wrong_slot = 0;
                while (session.options[wrong_slot] == word) ++wrong_slot;
            }
            // Full storage retries the same task, retaining one point of HP.
            assert(!wq_session_answer(&session, wrong_slot));
            assert(session.task_count == 1 && session.hp == 1);
            assert(wq_session_advance(&session));
            assert(!session.answered);
            assert_options(&session, count);
            assert(wq_session_answer(&session, session.correct_slot));
            assert(!wq_session_advance(&session));
            assert(session.hp == 0 && session.wrong_count == 1);
        }
    }
    // Every displayed choice in a singleton dictionary denotes the right word.
    const size_t word = 0;
    for (size_t slot = 0; slot < WQ_OPTIONS; ++slot) {
        wq_session_start(&session, pack, 1, NULL, 0, &word, 1, tasks, 1, 1);
        wq_session_build_options(&session, session.options);
        assert(wq_session_answer(&session, slot));
    }
}

static void test_progress_books_and_sparse_review(void)
{
    size_t out[16];
    const size_t sparse[] = {41001, 41599, 41001, 41600};
    assert(!wq_progress_load(11, 41600));
    wq_progress_mark_learned(sparse, 4);
    wq_progress_commit_start(41000);
    assert(wq_progress_learned() == 2);
    assert(wq_progress_pick_review(16, out) == 2);
    assert(out[0] != out[1]);
    assert((out[0] == 41001 || out[0] == 41599));
    assert((out[1] == 41001 || out[1] == 41599));
    assert(wq_progress_pick_review(1, out) == 1);
    assert(wq_progress_review_due(1) == 1);
    assert(wq_progress_review_due(10) == 2);
    assert(!wq_progress_load(12, 8));
    assert(!wq_progress_learned());
    const size_t other[] = {0, 2, 4, 6};
    wq_progress_mark_learned(other, 4);
    wq_progress_commit_start(5);
    assert(wq_progress_pick_new(16, out) == 4);
    assert(out[0] == 5 && out[1] == 7 && out[2] == 1 && out[3] == 3);
    assert(!wq_progress_load(11, 41600));
    assert(wq_progress_book() == 11 && wq_progress_total() == 41600);
    assert(wq_progress_start() == 41000 && wq_progress_learned() == 2);
    assert(wq_progress_pick_new(3, out) == 3);
    assert(out[0] == 41000 && out[1] == 41002 && out[2] == 41003);
    assert(!wq_progress_load(12, 8));
    assert(wq_progress_start() == 5 && wq_progress_learned() == 4);
    assert(wq_progress_pick_new(1, out) == 1 && out[0] == 5);
    assert(wq_progress_load(13, 0) == -1);
    assert(wq_progress_load(13, 41601) == -1);
    assert(wq_progress_book() == 12 && wq_progress_learned() == 4);
    assert(!wq_progress_pick_new(3, NULL));
    assert(!wq_progress_pick_review(3, NULL));
    wq_progress_mark_learned(NULL, 1);
    // A changed dictionary size invalidates this book's old bitmap.
    assert(!wq_progress_load(12, 9));
    assert(!wq_progress_learned() && !wq_progress_start());
}

static void test_progress_session_integration(void)
{
    size_t new_words[WQ_NEW_WORDS_PER_DAY];
    size_t review_words[WQ_NEW_WORDS_PER_DAY];
    size_t review[16];
    wq_task_t tasks[64];
    wq_session_t session;
    assert(!wq_progress_load(20, 12));
    size_t learned = 0;
    for (size_t day = 0; day < 3; ++day) {
        const size_t new_count = wq_progress_pick_new(10, new_words);
        const size_t review_count = wq_progress_pick_review(10, review_words);
        assert(new_count == (day == 0 ? 10U : day == 1 ? 2U : 0U));
        assert(review_count == (day == 0 ? 0U : 10U));
        wq_session_start(&session, pack, 12, new_words, new_count,
                         review_words, review_count, tasks, 64, (uint32_t)day);
        finish_session(&session, true);
        wq_progress_mark_learned(new_words, new_count);
        learned += new_count;
        if (new_count) wq_progress_commit_start(new_words[new_count - 1] + 1);
        assert(wq_progress_learned() == learned);
        assert(!wq_progress_load(21, 12));
        assert(!wq_progress_learned());
        assert(!wq_progress_load(20, 12));
        assert(wq_progress_learned() == learned);
    }
    assert(wq_progress_learned() == 12);
    assert(!wq_progress_pick_new(10, new_words));
    assert(wq_progress_pick_review(16, review) == 12);
    for (size_t i = 0; i < 12; ++i) {
        assert(review[i] < 12);
        for (size_t j = 0; j < i; ++j) assert(review[i] != review[j]);
    }
}

int main(void)
{
    test_session_flow();
    test_capacity_and_indices();
    test_small_packs_and_retry();
    test_progress_books_and_sparse_review();
    test_progress_session_integration();
    puts("PASS: model/progress native regressions (5 suites, 800 seeded small-pack sessions)");
    return 0;
}
