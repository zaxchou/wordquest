// SPDX-License-Identifier: Apache-2.0

#include "wq_dict.h"

#include <string.h>
#include <stdio.h>

#include "miniz_tinfl.h"
#include "wq_books_blob.h"

#if !defined(ESP_PLATFORM)
#include <stdio.h>
#include <stdlib.h>
static const char *TAG = "wq_dict";
#define WQ_LOGE(...) do { fprintf(stderr, "E wq_dict: "); fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } while (0)
static void *dict_malloc(size_t n)
{
    return malloc(n);
}

static void dict_free(void *p)
{
    free(p);
}
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "wq_dict";
#define WQ_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
static void *dict_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static void dict_free(void *p)
{
    heap_caps_free(p);
}
#endif


#define WQ_DICT_MAGIC 0x4B425157 // "WQBK"
#define WQ_DICT_MAX_BOOKS 4

typedef struct
{
    const char *name;
    size_t count;
    wq_word_entry_t *entries; // PSRAM views into the string pool
} wq_dict_book_t;

static wq_dict_book_t s_books[WQ_DICT_MAX_BOOKS];
static size_t s_book_count;
static uint8_t *s_raw; // decompressed blob, lives for the app's lifetime

// Parse a fixed-stride record block (14 B per word) into entry views that
// point straight into the string pool.
static void parse_records(const uint8_t *recs, size_t count,
                          const uint8_t *pool, wq_word_entry_t *out)
{
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *r = recs + i * 14;
        uint32_t off;
        uint16_t len[5];
        memcpy(&off, r, 4);
        memcpy(len, r + 4, 10);
        out[i].word = (const char *)pool + off;
        out[i].ipa = out[i].word + len[0];
        out[i].gloss = out[i].ipa + len[1]; // gloss == meaning
        out[i].meaning = out[i].gloss;
        out[i].example_en = out[i].meaning + len[2];
        out[i].example_zh = out[i].example_en + len[3];
        (void)len[4];
    }
}

int wq_dict_init(void)
{
    if (s_book_count) {
        return 0; // already loaded
    }

    const uint8_t *blob = WQ_BOOKS_BLOB;
    if (memcmp(blob, "WQBK", 4) != 0) {
        WQ_LOGE( "bad wordbook blob magic");
        return -1;
    }
    uint32_t raw_size;
    memcpy(&raw_size, blob + 4, 4);

    // Decompress into PSRAM; the compressed blob stays mapped in flash rodata.
    s_raw = dict_malloc(raw_size);
    if (!s_raw) {
        WQ_LOGE( "PSRAM alloc of %u bytes failed", (unsigned)raw_size);
        return -1;
    }
    const size_t done = tinfl_decompress_mem_to_mem(
        s_raw, raw_size, WQ_BOOKS_BLOB + 8, WQ_BOOKS_BLOB_SIZE - 8,
        TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (done != raw_size) {
        WQ_LOGE( "wordbook decompress failed (%u/%u)", (unsigned)done,
                 (unsigned)raw_size);
        dict_free(s_raw);
        s_raw = NULL;
        return -1;
    }

    // Layout: ver(2) book_count(2) | per book: name_len(2) name count(4)
    //         | per book: records (14 B per word) | pool_size(4) | pool
    const uint8_t *p = s_raw; // decompressed body starts at version
    uint16_t version, book_count;
    memcpy(&version, p, 2);
    memcpy(&book_count, p + 2, 2);
    p += 4;
    if (version != 1 || book_count == 0 || book_count > WQ_DICT_MAX_BOOKS) {
        WQ_LOGE( "bad wordbook header (v%u, %u books)", version,
                 book_count);
        return -1;
    }

    // Pass 1: book names/counts, then locate the record blocks and pool.
    static char names[WQ_DICT_MAX_BOOKS][24];
    size_t counts[WQ_DICT_MAX_BOOKS] = {0};
    const uint8_t *recs[WQ_DICT_MAX_BOOKS] = {0};
    const uint8_t *pool = NULL;
    size_t pool_size = 0;

    const uint8_t *q = p;
    for (size_t b = 0; b < book_count; ++b) {
        uint16_t name_len;
        memcpy(&name_len, q, 2);
        q += 2;
        memcpy(names[b], q, name_len < 24 ? name_len : 23);
        q += name_len;
        memcpy(&counts[b], q, 4);
        q += 4;
    }
    for (size_t b = 0; b < book_count; ++b) {
        recs[b] = q;
        q += counts[b] * 14;
    }
    memcpy(&pool_size, q, 4);
    q += 4;
    pool = q;
    if (q + pool_size != s_raw + raw_size) {
        WQ_LOGE( "pool size mismatch (%u)", (unsigned)pool_size);
        return -1;
    }

    // Pass 2: build entry views.
    for (size_t b = 0; b < book_count; ++b) {
        wq_word_entry_t *entries =
            dict_malloc(counts[b] * sizeof(wq_word_entry_t));
        if (!entries) {
            WQ_LOGE( "entry alloc failed for %s", names[b]);
            return -1;
        }
        parse_records(recs[b], counts[b], pool, entries);
        s_books[b].name = names[b]; // points into s_raw, stable for app life
        s_books[b].count = counts[b];
        s_books[b].entries = entries;
        ++s_book_count;
        WQ_LOGE( "book %u: %s (%u words)", (unsigned)b, names[b],
                 (unsigned)counts[b]);
    }
    return 0;
}

size_t wq_dict_book_count(void)
{
    return s_book_count;
}

const char *wq_dict_book_name(size_t book)
{
    return book < s_book_count ? s_books[book].name : "";
}

size_t wq_dict_word_count(size_t book)
{
    return book < s_book_count ? s_books[book].count : 0;
}

const wq_word_entry_t *wq_dict_entry(size_t book, size_t idx)
{
    if (book >= s_book_count || idx >= s_books[book].count) {
        return NULL;
    }
    return &s_books[book].entries[idx];
}
