// SPDX-License-Identifier: Apache-2.0
//
// Durable settings with write-then-read-back verification.
//
// Three key/value backends share one record layout:
//   * device  - NVS, with an explicit commit before verification
//   * native  - process RAM, so the simulator behaves like one persistent device
//   * test    - WQ_KV_TEST delegates to the harness, which injects failures
//
// The verification pass is the point of this module: a write that only reports
// success is not proof of durability, and a lost wordbook choice is exactly the
// kind of silent failure users notice.

#include "wq_settings.h"

#include <stddef.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "nvs.h"
static const char *TAG = "wq_cfg";
#define WQ_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#include <stdio.h>
#define WQ_LOGW(...) do { fprintf(stderr, "W wq_cfg: "); fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } while (0)
#endif

#define WQ_CFG_NAMESPACE "wordquest"

// One row per persisted field: NVS key plus its offset in the record.
static const struct {
    const char *key;
    size_t offset;
} FIELDS[] = {
    {"book", offsetof(wq_settings_t, book)},
    {"music", offsetof(wq_settings_t, music_on)},
    {"daily", offsetof(wq_settings_t, daily)},
    {"level", offsetof(wq_settings_t, level)},
    {"streak", offsetof(wq_settings_t, streak)},
    {"lastday", offsetof(wq_settings_t, last_day)},
    {"days", offsetof(wq_settings_t, days_total)},
};
#define FIELD_COUNT (sizeof(FIELDS) / sizeof(FIELDS[0]))

static uint32_t field_get(const wq_settings_t *s, size_t i)
{
    uint32_t value;
    memcpy(&value, (const char *)s + FIELDS[i].offset, sizeof(value));
    return value;
}

static void field_set(wq_settings_t *s, size_t i, uint32_t value)
{
    memcpy((char *)s + FIELDS[i].offset, &value, sizeof(value));
}

// --- key/value backend -----------------------------------------------------

#if defined(ESP_PLATFORM)

static nvs_handle_t s_handle;
static bool s_open;

static int kv_begin(bool write)
{
    // A missing namespace is normal on a blank device: reads then fall back to
    // defaults. Only a real error aborts.
    s_open = nvs_open(WQ_CFG_NAMESPACE, write ? NVS_READWRITE : NVS_READONLY,
                      &s_handle) == ESP_OK;
    return s_open ? 0 : -1;
}

static int kv_put(const char *key, uint32_t value)
{
    return s_open ? (nvs_set_u32(s_handle, key, value) == ESP_OK ? 0 : -1) : -1;
}

static int kv_get(const char *key, uint32_t *out)
{
    return s_open ? (nvs_get_u32(s_handle, key, out) == ESP_OK ? 0 : -1) : -1;
}

static int kv_put_blob(const char *key, const void *data, size_t len)
{
    return s_open ? (nvs_set_blob(s_handle, key, data, len) == ESP_OK ? 0 : -1)
                  : -1;
}

static int kv_get_blob(const char *key, void *data, size_t cap, size_t *out_len)
{
    size_t len = cap;
    if (!s_open || nvs_get_blob(s_handle, key, data, &len) != ESP_OK) return -1;
    *out_len = len;
    return 0;
}

static int kv_commit(void)
{
    return s_open ? (nvs_commit(s_handle) == ESP_OK ? 0 : -1) : -1;
}

static void kv_end(void)
{
    if (s_open) {
        nvs_close(s_handle);
        s_open = false;
    }
}

#elif defined(WQ_KV_TEST)

// Provided by the regression harness.
int wq_test_kv_begin(int write);
int wq_test_kv_put(const char *key, uint32_t value);
int wq_test_kv_get(const char *key, uint32_t *out);
int wq_test_kv_put_blob(const char *key, const void *data, size_t len);
int wq_test_kv_get_blob(const char *key, void *data, size_t cap, size_t *out_len);
int wq_test_kv_commit(void);
void wq_test_kv_end(void);

static int kv_begin(bool write) { return wq_test_kv_begin(write ? 1 : 0); }
static int kv_put(const char *key, uint32_t value) { return wq_test_kv_put(key, value); }
static int kv_get(const char *key, uint32_t *out) { return wq_test_kv_get(key, out); }
static int kv_put_blob(const char *key, const void *data, size_t len)
{
    return wq_test_kv_put_blob(key, data, len);
}
static int kv_get_blob(const char *key, void *data, size_t cap, size_t *out_len)
{
    return wq_test_kv_get_blob(key, data, cap, out_len);
}
static int kv_commit(void) { return wq_test_kv_commit(); }
static void kv_end(void) { wq_test_kv_end(); }

#else

// Native simulator: keep the settings for the process lifetime so switching
// books or toggling music behaves like one persistent device.
static wq_settings_t s_ram;
static bool s_ram_valid;

static int kv_begin(bool write)
{
    if (!s_ram_valid) {
        wq_settings_defaults(&s_ram);
        s_ram_valid = true;
    }
    (void)write;
    return 0;
}

static int kv_put(const char *key, uint32_t value)
{
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        if (strcmp(key, FIELDS[i].key) == 0) {
            field_set(&s_ram, i, value);
            return 0;
        }
    }
    return -1;
}

static int kv_get(const char *key, uint32_t *out)
{
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        if (strcmp(key, FIELDS[i].key) == 0) {
            *out = field_get(&s_ram, i);
            return 0;
        }
    }
    return -1;
}

// Per-book bitmaps (favourites, mistakes) are small blobs; keep a handful in
// RAM so the simulator behaves like one persistent device.
#define WQ_RAM_BLOBS 4
#define WQ_RAM_BLOB_MAX 1088
static struct {
    char key[16];
    uint8_t data[WQ_RAM_BLOB_MAX];
    size_t len;
    int used;
} s_ram_blobs[WQ_RAM_BLOBS];

static int ram_blob_find(const char *key)
{
    for (int i = 0; i < WQ_RAM_BLOBS; ++i) {
        if (s_ram_blobs[i].used && strcmp(s_ram_blobs[i].key, key) == 0) return i;
    }
    return -1;
}

static int kv_put_blob(const char *key, const void *data, size_t len)
{
    if (len > WQ_RAM_BLOB_MAX) return -1;
    int slot = ram_blob_find(key);
    if (slot < 0) {
        for (int i = 0; i < WQ_RAM_BLOBS; ++i) {
            if (!s_ram_blobs[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return -1;
    size_t n = strlen(key);
    if (n >= sizeof(s_ram_blobs[0].key)) return -1;
    memset(s_ram_blobs[slot].key, 0, sizeof(s_ram_blobs[slot].key));
    memcpy(s_ram_blobs[slot].key, key, n);
    memcpy(s_ram_blobs[slot].data, data, len);
    s_ram_blobs[slot].len = len;
    s_ram_blobs[slot].used = 1;
    return 0;
}

static int kv_get_blob(const char *key, void *data, size_t cap, size_t *out_len)
{
    const int slot = ram_blob_find(key);
    if (slot < 0 || s_ram_blobs[slot].len > cap) return -1;
    memcpy(data, s_ram_blobs[slot].data, s_ram_blobs[slot].len);
    *out_len = s_ram_blobs[slot].len;
    return 0;
}

static int kv_commit(void) { return 0; }
static void kv_end(void) {}

#endif

// --- public API ------------------------------------------------------------

void wq_settings_defaults(wq_settings_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->music_on = 1;
    out->daily = WQ_DAILY_DEFAULT;
    out->level = 1;
}

void wq_settings_sanitize(wq_settings_t *s, uint32_t book_count)
{
    if (!s) return;
    if (s->music_on > 1) s->music_on = 1;
    if (s->daily < WQ_DAILY_MIN || s->daily > WQ_DAILY_MAX) {
        // An out-of-range quota means the stored value is unusable, not a
        // preference: fall back to the default instead of the nearest bound.
        s->daily = WQ_DAILY_DEFAULT;
    }
    if (book_count && s->book >= book_count) {
        s->book = 0; // dictionary changed under a stale index
    }
    if (s->level == 0) s->level = 1;
}

int wq_settings_load(wq_settings_t *out)
{
    if (!out) return -1;
    wq_settings_defaults(out);
    if (kv_begin(false) != 0) {
        return 0; // no store yet: defaults are a valid state
    }
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        uint32_t value;
        if (kv_get(FIELDS[i].key, &value) == 0) {
            field_set(out, i, value);
        }
    }
    kv_end();
    wq_settings_sanitize(out, 0);
    return 0;
}

static int save_once(const wq_settings_t *wanted)
{
    if (kv_begin(true) != 0) {
        WQ_LOGW("settings store unavailable; changes will not persist");
        return -1;
    }
    int failed = 0;
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        if (kv_put(FIELDS[i].key, field_get(wanted, i)) != 0) {
            WQ_LOGW("write of '%s' was rejected", FIELDS[i].key);
            failed = 1;
        }
    }
    if (kv_commit() != 0) {
        WQ_LOGW("commit failed; settings are not durable");
        failed = 1;
    }
    kv_end();

    // Verification pass: reopen read-only and compare every field.
    if (kv_begin(false) != 0) {
        WQ_LOGW("read-back unavailable; durability unverified");
        kv_end();
        return -1;
    }
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        uint32_t stored = 0;
        const uint32_t expected = field_get(wanted, i);
        const int readable = kv_get(FIELDS[i].key, &stored);
        if (readable != 0 || stored != expected) {
            WQ_LOGW("read-back %s for '%s' (expected %u, got %u)",
                    readable != 0 ? "missing" : "mismatch", FIELDS[i].key,
                    (unsigned)expected, (unsigned)stored);
            failed = 1;
        }
    }
    kv_end();
    return failed ? -1 : 0;
}

int wq_settings_save(const wq_settings_t *s)
{
    if (!s) return -1;
    wq_settings_t wanted = *s;
    wq_settings_sanitize(&wanted, 0);

    if (save_once(&wanted) == 0) {
        return 0;
    }
    // NVS writes do occasionally fail transiently right after a flash erase or
    // during a concurrent commit; one retry keeps a chosen wordbook from being
    // silently dropped.
    WQ_LOGW("retrying settings save once");
    return save_once(&wanted);
}

// --- verified blobs --------------------------------------------------------
//
// Same contract as the scalar record: write, commit, then reopen and compare
// the stored bytes. Used for per-book bitmaps such as the favourites list,
// where a lost write would silently empty the user's word list.

static int save_blob_once(const char *key, const void *data, size_t len,
                          uint8_t *scratch)
{
    if (kv_begin(true) != 0) {
        WQ_LOGW("store unavailable; '%s' will not persist", key);
        return -1;
    }
    int failed = 0;
    if (kv_put_blob(key, data, len) != 0) {
        WQ_LOGW("write of '%s' was rejected", key);
        failed = 1;
    }
    if (kv_commit() != 0) {
        WQ_LOGW("commit of '%s' failed", key);
        failed = 1;
    }
    kv_end();

    size_t stored_len = 0;
    if (kv_begin(false) != 0 ||
        kv_get_blob(key, scratch, len, &stored_len) != 0) {
        WQ_LOGW("read-back missing for '%s'", key);
        kv_end();
        return -1;
    }
    if (stored_len != len || memcmp(scratch, data, len) != 0) {
        WQ_LOGW("read-back mismatch for '%s' (%u vs %u bytes)", key,
                (unsigned)stored_len, (unsigned)len);
        failed = 1;
    }
    kv_end();
    return failed ? -1 : 0;
}

int wq_settings_save_blob(const char *key, const void *data, size_t len)
{
    if (!key || !data || !len || len > WQ_BLOB_MAX) return -1;
    static uint8_t scratch[WQ_BLOB_MAX];
    if (save_blob_once(key, data, len, scratch) == 0) {
        return 0;
    }
    WQ_LOGW("retrying '%s' save once", key);
    return save_blob_once(key, data, len, scratch);
}

int wq_settings_load_blob(const char *key, void *data, size_t cap, size_t *out_len)
{
    if (!key || !data || !cap || !out_len) return -1;
    if (kv_begin(false) != 0) return -1;
    const int rc = kv_get_blob(key, data, cap, out_len);
    kv_end();
    return rc == 0 && *out_len == cap ? 0 : -1;
}
