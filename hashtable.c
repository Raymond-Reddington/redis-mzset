#include "mzset.h"

#include <string.h>
#include <stdlib.h>

#define MZ_DICT_INIT_SIZE  16

/* FNV-1a 64 */
static uint64_t hash_bytes(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int keq(const mz_dict_entry_t *e, const void *key, size_t klen) {
    return e->key_len == klen && memcmp(e->payload, key, klen) == 0;
}

int mz_dict_init(mz_dict_t *d) {
    d->size = MZ_DICT_INIT_SIZE;
    d->used = 0;
    d->buckets = (mz_dict_entry_t **)calloc(d->size, sizeof(mz_dict_entry_t *));
    return d->buckets ? 0 : -1;
}

static void free_chain(mz_dict_entry_t *e) {
    while (e) {
        mz_dict_entry_t *n = e->next;
        free(e);
        e = n;
    }
}

void mz_dict_free(mz_dict_t *d) {
    if (!d || !d->buckets) return;
    for (uint64_t i = 0; i < d->size; i++) free_chain(d->buckets[i]);
    free(d->buckets);
    d->buckets = NULL;
    d->size = d->used = 0;
}

static void dict_resize(mz_dict_t *d, uint64_t new_size) {
    mz_dict_entry_t **nb = (mz_dict_entry_t **)calloc(new_size, sizeof(mz_dict_entry_t *));
    if (!nb) return; /* keep old table on OOM */
    for (uint64_t i = 0; i < d->size; i++) {
        mz_dict_entry_t *e = d->buckets[i];
        while (e) {
            mz_dict_entry_t *next = e->next;
            uint64_t idx = hash_bytes(e->payload, e->key_len) & (new_size - 1);
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    free(d->buckets);
    d->buckets = nb;
    d->size = new_size;
}

const uint8_t *mz_dict_get(const mz_dict_t *d, const void *key, size_t klen, uint32_t *out_vlen) {
    uint64_t idx = hash_bytes(key, klen) & (d->size - 1);
    for (mz_dict_entry_t *e = d->buckets[idx]; e; e = e->next) {
        if (keq(e, key, klen)) {
            if (out_vlen) *out_vlen = e->val_len;
            return e->payload + e->key_len;
        }
    }
    return NULL;
}

int mz_dict_put(mz_dict_t *d, const void *key, size_t klen,
                const void *val, size_t vlen) {
    uint64_t idx = hash_bytes(key, klen) & (d->size - 1);
    mz_dict_entry_t *e = d->buckets[idx];
    while (e) {
        if (keq(e, key, klen)) {
            if (e->val_len == vlen) {
                memcpy(e->payload + e->key_len, val, vlen);
                return 0;
            }
            /* size changed, realloc entry */
            mz_dict_entry_t *ne = (mz_dict_entry_t *)malloc(sizeof(*ne) + klen + vlen);
            if (!ne) return -1;
            ne->next    = e->next;
            ne->key_len = (uint32_t)klen;
            ne->val_len = (uint32_t)vlen;
            memcpy(ne->payload, key, klen);
            memcpy(ne->payload + klen, val, vlen);
            /* unlink old */
            mz_dict_entry_t **pp = &d->buckets[idx];
            while (*pp != e) pp = &(*pp)->next;
            *pp = ne;
            free(e);
            return 0;
        }
        e = e->next;
    }
    mz_dict_entry_t *ne = (mz_dict_entry_t *)malloc(sizeof(*ne) + klen + vlen);
    if (!ne) return -1;
    ne->next    = d->buckets[idx];
    ne->key_len = (uint32_t)klen;
    ne->val_len = (uint32_t)vlen;
    memcpy(ne->payload, key, klen);
    memcpy(ne->payload + klen, val, vlen);
    d->buckets[idx] = ne;
    d->used++;
    if (d->used > d->size) dict_resize(d, d->size * 2);
    return 1;
}

int mz_dict_del(mz_dict_t *d, const void *key, size_t klen) {
    uint64_t idx = hash_bytes(key, klen) & (d->size - 1);
    mz_dict_entry_t **pp = &d->buckets[idx];
    while (*pp) {
        if (keq(*pp, key, klen)) {
            mz_dict_entry_t *e = *pp;
            *pp = e->next;
            free(e);
            d->used--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/* ---------- iterator ---------- */

void mz_dict_iter_init(mz_dict_iter_t *it, const mz_dict_t *d) {
    it->d = d;
    it->bucket = 0;
    it->entry = NULL;
}

int mz_dict_iter_next(mz_dict_iter_t *it,
                      const uint8_t **key, uint32_t *klen,
                      const uint8_t **val, uint32_t *vlen) {
    if (it->entry) it->entry = it->entry->next;
    while (!it->entry) {
        if (it->bucket >= it->d->size) return 0;
        it->entry = it->d->buckets[it->bucket++];
    }
    *key  = it->entry->payload;
    *klen = it->entry->key_len;
    *val  = it->entry->payload + it->entry->key_len;
    *vlen = it->entry->val_len;
    return 1;
}
