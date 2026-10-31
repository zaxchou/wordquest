// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "word_pack.h" // wq_word_entry_t layout (field-compatible view)

// Compressed wordbook store: two embedded dictionaries (CET-4/CET-6) shipped
// as a zlib-compressed blob, decompressed once into PSRAM at init and parsed
// into fixed-layout views that satisfy wq_word_entry_t.

int wq_dict_init(void);

size_t wq_dict_book_count(void);
const char *wq_dict_book_name(size_t book);
size_t wq_dict_word_count(size_t book);
// Random-access entry view into the PSRAM pool (gloss == meaning).
const wq_word_entry_t *wq_dict_entry(size_t book, size_t idx);
