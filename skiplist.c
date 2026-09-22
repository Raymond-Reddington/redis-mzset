#include "mzset.h"

#include <string.h>
#include <stdlib.h>

/* ---------- comparison ---------- */

static int ckey_cmp(const uint8_t *a, uint16_t alen,
                    const uint8_t *b, uint16_t blen) {
    uint16_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0) return c;
    return (int)alen - (int)blen;
}

/* ---------- random level ---------- */

/* Local xorshift64 so we don't touch the process-wide rand() state. */
static uint64_t rng_state = 0x1234567890abcdefULL;

static uint32_t xrand(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)x;
}

static uint8_t random_level(void) {
    uint8_t lvl = 1;
    /* MZ_SL_P == 0.25 -> compare low 16 bits < 0.25 * 65536 = 16384 */
    while ((xrand() & 0xFFFF) < 16384 && lvl < MZ_SL_MAX_LEVEL) lvl++;
    return lvl;
}

/* ---------- node alloc ---------- */

static mz_sl_node_t *sl_new_node(uint8_t level, const uint8_t *ckey, uint16_t ckey_len) {
    mz_sl_node_t *n = (mz_sl_node_t *)malloc(sizeof(*n) + level * sizeof(n->lvls[0]));
    if (!n) return NULL;
    n->backward = NULL;
    n->level = level;
    if (ckey && ckey_len > 0) {
        n->ckey = (uint8_t *)malloc(ckey_len);
        if (!n->ckey) { free(n); return NULL; }
        memcpy(n->ckey, ckey, ckey_len);
        n->ckey_len = ckey_len;
    } else {
        n->ckey = NULL;
        n->ckey_len = 0;
    }
    for (uint8_t i = 0; i < level; i++) {
        n->lvls[i].forward = NULL;
        n->lvls[i].span = 0;
    }
    return n;
}

/* ---------- lifecycle ---------- */

int mz_sl_init(mz_sl_t *sl) {
    sl->header = sl_new_node(MZ_SL_MAX_LEVEL, NULL, 0);
    if (!sl->header) return -1;
    sl->tail = NULL;
    sl->length = 0;
    sl->level = 1;
    return 0;
}

void mz_sl_free(mz_sl_t *sl) {
    if (!sl || !sl->header) return;
    mz_sl_node_t *n = sl->header->lvls[0].forward;
    free(sl->header);
    while (n) {
        mz_sl_node_t *next = n->lvls[0].forward;
        free(n->ckey);
        free(n);
        n = next;
    }
    sl->header = sl->tail = NULL;
    sl->length = 0;
    sl->level = 0;
}

/* ---------- insert ---------- */

void mz_sl_insert(mz_sl_t *sl, const uint8_t *ckey, uint16_t ckey_len) {
    mz_sl_node_t *update[MZ_SL_MAX_LEVEL];
    uint64_t rank[MZ_SL_MAX_LEVEL];
    mz_sl_node_t *x = sl->header;

    for (int i = sl->level - 1; i >= 0; i--) {
        rank[i] = (i == sl->level - 1) ? 0 : rank[i + 1];
        while (x->lvls[i].forward &&
               ckey_cmp(x->lvls[i].forward->ckey, x->lvls[i].forward->ckey_len,
                        ckey, ckey_len) < 0) {
            rank[i] += x->lvls[i].span;
            x = x->lvls[i].forward;
        }
        update[i] = x;
    }

    uint8_t lvl = random_level();
    if (lvl > sl->level) {
        for (uint8_t i = sl->level; i < lvl; i++) {
            rank[i] = 0;
            update[i] = sl->header;
            update[i]->lvls[i].span = sl->length;
        }
        sl->level = lvl;
    }

    x = sl_new_node(lvl, ckey, ckey_len);
    for (uint8_t i = 0; i < lvl; i++) {
        x->lvls[i].forward = update[i]->lvls[i].forward;
        update[i]->lvls[i].forward = x;
        x->lvls[i].span = update[i]->lvls[i].span - (uint32_t)(rank[0] - rank[i]);
        update[i]->lvls[i].span = (uint32_t)(rank[0] - rank[i]) + 1;
    }
    for (uint8_t i = lvl; i < sl->level; i++) {
        update[i]->lvls[i].span++;
    }

    x->backward = (update[0] == sl->header) ? NULL : update[0];
    if (x->lvls[0].forward) x->lvls[0].forward->backward = x;
    else                    sl->tail = x;
    sl->length++;
}

/* ---------- delete ---------- */

int mz_sl_delete(mz_sl_t *sl, const uint8_t *ckey, uint16_t ckey_len) {
    mz_sl_node_t *update[MZ_SL_MAX_LEVEL];
    mz_sl_node_t *x = sl->header;

    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->lvls[i].forward &&
               ckey_cmp(x->lvls[i].forward->ckey, x->lvls[i].forward->ckey_len,
                        ckey, ckey_len) < 0) {
            x = x->lvls[i].forward;
        }
        update[i] = x;
    }
    x = x->lvls[0].forward;
    if (!x || ckey_cmp(x->ckey, x->ckey_len, ckey, ckey_len) != 0) return 0;

    for (uint8_t i = 0; i < sl->level; i++) {
        if (update[i]->lvls[i].forward == x) {
            update[i]->lvls[i].span += x->lvls[i].span - 1;
            update[i]->lvls[i].forward = x->lvls[i].forward;
        } else {
            update[i]->lvls[i].span--;
        }
    }
    if (x->lvls[0].forward) x->lvls[0].forward->backward = x->backward;
    else                    sl->tail = x->backward;

    while (sl->level > 1 && sl->header->lvls[sl->level - 1].forward == NULL) sl->level--;
    sl->length--;
    free(x->ckey);
    free(x);
    return 1;
}

/* ---------- rank / lookup ---------- */

uint64_t mz_sl_get_rank(const mz_sl_t *sl, const uint8_t *ckey, uint16_t ckey_len) {
    uint64_t rank = 0;
    mz_sl_node_t *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->lvls[i].forward &&
               ckey_cmp(x->lvls[i].forward->ckey, x->lvls[i].forward->ckey_len,
                        ckey, ckey_len) <= 0) {
            rank += x->lvls[i].span;
            x = x->lvls[i].forward;
        }
        if (x != sl->header && x->ckey_len == ckey_len &&
            memcmp(x->ckey, ckey, ckey_len) == 0) {
            return rank;
        }
    }
    return 0;
}

mz_sl_node_t *mz_sl_get_by_rank(const mz_sl_t *sl, uint64_t rank) {
    if (rank == 0 || rank > sl->length) return NULL;
    uint64_t traversed = 0;
    mz_sl_node_t *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->lvls[i].forward && (traversed + x->lvls[i].span) <= rank) {
            traversed += x->lvls[i].span;
            x = x->lvls[i].forward;
        }
        if (traversed == rank) return x == sl->header ? NULL : x;
    }
    return NULL;
}
