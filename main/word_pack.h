// SPDX-License-Identifier: Apache-2.0
//
// Word entry type shared by the dictionary loader (wq_dict.c) and the UI.
// The old 36-word JunEnglish pack has been retired; word data now comes
// from the CET-4/CET-6 dictionaries loaded via wq_dict.c.
#pragma once

typedef struct {
    const char *word;
    const char *gloss; // short meaning (same as meaning for dict entries)
    const char *ipa;
    const char *meaning;
    const char *example_en;
    const char *example_zh;
} wq_word_entry_t;
