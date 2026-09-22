/* Standalone unit test: exercises encoding + hashtable + skiplist + mzset ops.
 * Does not depend on Redis. Build with:
 *   cc -O2 -Wall -std=gnu11 -I.. tests/test_core.c ../encoding.c ../hashtable.c \
 *      ../skiplist.c ../mzset.c -o tests/test_core
 */

#include "../mzset.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL  " __VA_ARGS__); printf("\n"); \
        failures++; \
    } else { \
        printf("  ok    " __VA_ARGS__); printf("\n"); \
    } \
} while (0)

static int cmp_bytes(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c) return c;
    return (int)alen - (int)blen;
}

/* ---------- encoding ---------- */

static void test_encoding_i64(void) {
    printf("[encoding: i64 asc]\n");
    uint8_t types[] = { MZ_T_I64 };
    mz_schema_t s; mz_schema_init(&s, types, 0, 1);

    int64_t xs[] = { INT64_MIN, -1000, -1, 0, 1, 1000, INT64_MAX };
    int n = sizeof(xs)/sizeof(xs[0]);

    uint8_t bufs[8][8];
    for (int i = 0; i < n; i++) {
        mz_val_t v; v.i64 = xs[i];
        mz_encode_field(&s, 0, v, bufs[i]);
    }
    for (int i = 1; i < n; i++)
        CHECK(cmp_bytes(bufs[i-1], 8, bufs[i], 8) < 0,
              "%lld < %lld", (long long)xs[i-1], (long long)xs[i]);

    /* roundtrip */
    for (int i = 0; i < n; i++) {
        mz_val_t v = mz_decode_field(&s, 0, bufs[i]);
        CHECK(v.i64 == xs[i], "roundtrip %lld", (long long)xs[i]);
    }
}

static void test_encoding_f64(void) {
    printf("[encoding: f64 asc]\n");
    uint8_t types[] = { MZ_T_F64 };
    mz_schema_t s; mz_schema_init(&s, types, 0, 1);
    double xs[] = { -1e300, -1.5, -0.5, -0.0, 0.0, 0.5, 1.5, 1e300 };
    int n = sizeof(xs)/sizeof(xs[0]);
    uint8_t bufs[8][8];
    for (int i = 0; i < n; i++) {
        mz_val_t v; v.f64 = xs[i];
        mz_encode_field(&s, 0, v, bufs[i]);
    }
    for (int i = 1; i < n; i++) {
        int c = cmp_bytes(bufs[i-1], 8, bufs[i], 8);
        /* -0.0 and 0.0 have same bit encoding after transform? actually different */
        CHECK(c <= 0, "%g <= %g", xs[i-1], xs[i]);
    }
    for (int i = 0; i < n; i++) {
        mz_val_t v = mz_decode_field(&s, 0, bufs[i]);
        CHECK(v.f64 == xs[i] || (xs[i] == 0.0 && v.f64 == 0.0),
              "roundtrip %g", xs[i]);
    }
}

static void test_encoding_desc(void) {
    printf("[encoding: i64 desc]\n");
    uint8_t types[] = { MZ_T_I64 };
    mz_schema_t s; mz_schema_init(&s, types, 1, 1);  /* dirs bit 0 = 1 -> desc */
    uint8_t a[8], b[8];
    mz_val_t va = { .i64 = 100 }, vb = { .i64 = 200 };
    mz_encode_field(&s, 0, va, a);
    mz_encode_field(&s, 0, vb, b);
    CHECK(cmp_bytes(a, 8, b, 8) > 0, "desc: 100's encoding > 200's");
    CHECK(mz_decode_field(&s, 0, a).i64 == 100, "desc decode 100");
    CHECK(mz_decode_field(&s, 0, b).i64 == 200, "desc decode 200");
}

/* ---------- hashtable ---------- */

static void test_hashtable(void) {
    printf("[hashtable]\n");
    mz_dict_t d; mz_dict_init(&d);
    mz_dict_put(&d, "alice", 5, "v1", 2);
    mz_dict_put(&d, "bob",   3, "value2", 6);
    uint32_t vl;
    const uint8_t *v = mz_dict_get(&d, "alice", 5, &vl);
    CHECK(v && vl == 2 && memcmp(v, "v1", 2) == 0, "get alice");
    v = mz_dict_get(&d, "bob", 3, &vl);
    CHECK(v && vl == 6 && memcmp(v, "value2", 6) == 0, "get bob");
    CHECK(mz_dict_get(&d, "carol", 5, &vl) == NULL, "get missing");

    /* replace same-size */
    mz_dict_put(&d, "alice", 5, "vX", 2);
    v = mz_dict_get(&d, "alice", 5, &vl);
    CHECK(vl == 2 && memcmp(v, "vX", 2) == 0, "replace same-size");
    /* replace different size */
    mz_dict_put(&d, "alice", 5, "loooooong", 9);
    v = mz_dict_get(&d, "alice", 5, &vl);
    CHECK(vl == 9 && memcmp(v, "loooooong", 9) == 0, "replace diff-size");

    /* many inserts to trigger resize */
    for (int i = 0; i < 500; i++) {
        char k[16]; int l = snprintf(k, sizeof(k), "k%d", i);
        mz_dict_put(&d, k, l, &i, sizeof i);
    }
    CHECK(d.used == 502, "used = 502 (got %llu)", (unsigned long long)d.used);
    for (int i = 0; i < 500; i++) {
        char k[16]; int l = snprintf(k, sizeof(k), "k%d", i);
        int stored;
        v = mz_dict_get(&d, k, l, &vl);
        CHECK(v && vl == sizeof i, "get k%d present", i);
        memcpy(&stored, v, sizeof stored);
        CHECK(stored == i, "get k%d value correct", i);
    }
    CHECK(mz_dict_del(&d, "alice", 5) == 1, "del alice");
    CHECK(mz_dict_del(&d, "alice", 5) == 0, "del alice again -> 0");
    mz_dict_free(&d);
}

/* ---------- skiplist ---------- */

static void test_skiplist(void) {
    printf("[skiplist]\n");
    mz_sl_t sl; mz_sl_init(&sl);
    const char *ks[] = { "aaa", "bbb", "ccc", "ddd", "eee" };
    for (int i = 0; i < 5; i++)
        mz_sl_insert(&sl, (const uint8_t *)ks[i], 3);
    CHECK(sl.length == 5, "length=5");
    for (int i = 0; i < 5; i++)
        CHECK(mz_sl_get_rank(&sl, (const uint8_t *)ks[i], 3) == (uint64_t)(i + 1),
              "rank(%s) == %d", ks[i], i + 1);
    for (int i = 0; i < 5; i++) {
        mz_sl_node_t *n = mz_sl_get_by_rank(&sl, i + 1);
        CHECK(n && n->ckey_len == 3 && memcmp(n->ckey, ks[i], 3) == 0,
              "get_by_rank(%d) == %s", i + 1, ks[i]);
    }
    CHECK(mz_sl_delete(&sl, (const uint8_t *)"ccc", 3) == 1, "delete ccc");
    CHECK(sl.length == 4, "length=4");
    CHECK(mz_sl_get_rank(&sl, (const uint8_t *)"ddd", 3) == 3, "rank(ddd) now 3");

    /* insert lots, verify sorted */
    mz_sl_free(&sl); mz_sl_init(&sl);
    int N = 1000;
    for (int i = 0; i < N; i++) {
        uint8_t k[8];
        int v = (i * 7919) % N;   /* pseudo-random */
        k[0] = (v >> 24) & 0xFF; k[1] = (v >> 16) & 0xFF;
        k[2] = (v >> 8) & 0xFF;  k[3] = v & 0xFF;
        mz_sl_insert(&sl, k, 4);
    }
    CHECK(sl.length == (uint64_t)N, "N inserted");
    /* verify order */
    int ordered = 1;
    mz_sl_node_t *x = sl.header->lvls[0].forward;
    mz_sl_node_t *p = NULL;
    while (x) {
        if (p && memcmp(p->ckey, x->ckey, 4) >= 0) { ordered = 0; break; }
        p = x; x = x->lvls[0].forward;
    }
    CHECK(ordered, "1000 elements sorted correctly");
    mz_sl_free(&sl);
}

/* ---------- mzset end-to-end ---------- */

static void test_mzset(void) {
    printf("[mzset end-to-end]\n");
    uint8_t types[] = { MZ_T_F64, MZ_T_I64 };
    mz_schema_t s; mz_schema_init(&s, types, 0b01, 2);  /* f64 DESC, i64 ASC */
    mzset_t *m = mzset_create(&s);

    mz_val_t v1[2] = {{ .f64 = 87.5 }, { .i64 = 1000 }};
    mz_val_t v2[2] = {{ .f64 = 92.0 }, { .i64 = 900  }};
    mz_val_t v3[2] = {{ .f64 = 87.5 }, { .i64 = 500  }};
    mzset_add(m, "alice", 5, v1, 0);
    mzset_add(m, "bob",   3, v2, 0);
    mzset_add(m, "carol", 5, v3, 0);
    CHECK(mzset_card(m) == 3, "card=3");

    /* Expected order (f64 DESC, i64 ASC):
     * bob (92.0)  ->  carol (87.5, i64=500)  ->  alice (87.5, i64=1000) */
    CHECK(mzset_rank(m, "bob",   3, 0) == 1, "rank(bob)=1");
    CHECK(mzset_rank(m, "carol", 5, 0) == 2, "rank(carol)=2");
    CHECK(mzset_rank(m, "alice", 5, 0) == 3, "rank(alice)=3");
    CHECK(mzset_rank(m, "bob", 3, 1) == 3, "rev-rank(bob)=3");

    /* update alice's f64 to 100.0 -> becomes top */
    uint8_t idx[] = { 0 };
    mz_val_t nv[] = {{ .f64 = 100.0 }};
    CHECK(mzset_update(m, "alice", 5, idx, nv, 1) == MZ_OK, "update alice");
    CHECK(mzset_rank(m, "alice", 5, 0) == 1, "alice now #1");

    /* incrby i64 */
    mz_val_t d; d.i64 = 50;
    mz_val_t out;
    CHECK(mzset_incrby(m, "carol", 5, 1, d, &out) == MZ_OK && out.i64 == 550,
          "carol incrby[1] += 50 -> 550");

    /* rem */
    CHECK(mzset_rem(m, "bob", 3) == MZ_OK, "rem bob");
    CHECK(mzset_card(m) == 2, "card=2");
    CHECK(mzset_rem(m, "bob", 3) == MZ_ERR_NO_MEMBER, "rem bob again -> err");

    mzset_free(m);
}

/* ---------- rank-large scale sanity ---------- */

static void test_scale(void) {
    printf("[mzset scale: 100k members]\n");
    uint8_t types[] = { MZ_T_I64 };
    mz_schema_t s; mz_schema_init(&s, types, 0, 1);
    mzset_t *m = mzset_create(&s);
    int N = 100000;
    for (int i = 0; i < N; i++) {
        char k[16]; int l = snprintf(k, sizeof(k), "m%08d", i);
        mz_val_t v; v.i64 = (int64_t)((i * 2654435761u) % N);
        mzset_add(m, k, l, &v, 0);
    }
    CHECK(mzset_card(m) == (uint64_t)N, "N inserted");

    /* rank of member 0 should equal rank determined by its score */
    char k[16]; int l = snprintf(k, sizeof(k), "m%08d", 0);
    uint64_t r = mzset_rank(m, k, l, 0);
    CHECK(r >= 1 && r <= (uint64_t)N, "rank in range");
    mzset_free(m);
}

int main(void) {
    test_encoding_i64();
    test_encoding_f64();
    test_encoding_desc();
    test_hashtable();
    test_skiplist();
    test_mzset();
    test_scale();
    printf("\n%s (%d failures)\n", failures ? "FAIL" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
