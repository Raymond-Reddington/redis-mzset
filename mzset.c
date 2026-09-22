#include "mzset.h"

#include <string.h>
#include <stdlib.h>

/* buffer size for a full composite key (fields blob + member_id) */
#define MZ_CKEY_BUFSZ (MZ_MAX_FIELDS * 8 + MZ_MAX_MEMBER_ID)

/* ---------- lifecycle ---------- */

mzset_t *mzset_create(const mz_schema_t *s) {
    mzset_t *m = (mzset_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->schema = *s;
    if (mz_dict_init(&m->dict) < 0) { free(m); return NULL; }
    if (mz_sl_init(&m->sl) < 0) { mz_dict_free(&m->dict); free(m); return NULL; }
    return m;
}

void mzset_free(mzset_t *m) {
    if (!m) return;
    mz_dict_free(&m->dict);
    mz_sl_free(&m->sl);
    free(m);
}

uint64_t mzset_card(const mzset_t *m) { return m->sl.length; }

/* ---------- add / replace ---------- */

int mzset_add(mzset_t *m, const void *member_id, size_t mlen,
              const mz_val_t *vals, int create_only) {
    if (mlen == 0 || mlen > MZ_MAX_MEMBER_ID) return MZ_ERR_BAD_FIELD;
    uint16_t blob = MZ_SCHEMA_BLOB(&m->schema);

    uint8_t ckey[MZ_CKEY_BUFSZ];
    uint32_t old_len = 0;
    const uint8_t *old_raw = mz_dict_get(&m->dict, member_id, mlen, &old_len);

    if (old_raw) {
        if (create_only) return MZ_ERR_EXISTS;
        /* remove old node from skiplist */
        size_t ck_len = mz_build_ckey(&m->schema, old_raw, member_id, mlen, ckey);
        mz_sl_delete(&m->sl, ckey, (uint16_t)ck_len);
    }

    /* pack new raw fields into dict */
    uint8_t raw[MZ_MAX_FIELDS * 8];
    mz_pack_raw(&m->schema, vals, raw);
    mz_dict_put(&m->dict, member_id, mlen, raw, blob);

    /* insert new node */
    size_t ck_len = mz_build_ckey_from_vals(&m->schema, vals, member_id, mlen, ckey);
    mz_sl_insert(&m->sl, ckey, (uint16_t)ck_len);
    return MZ_OK;
}

/* ---------- update ---------- */

int mzset_update(mzset_t *m, const void *member_id, size_t mlen,
                 const uint8_t *indices, const mz_val_t *vals, size_t n) {
    uint32_t old_len = 0;
    const uint8_t *old_raw = mz_dict_get(&m->dict, member_id, mlen, &old_len);
    if (!old_raw) return MZ_ERR_NO_MEMBER;

    /* unpack, patch, re-encode */
    mz_val_t values[MZ_MAX_FIELDS];
    mz_unpack_raw(&m->schema, old_raw, values);

    for (size_t i = 0; i < n; i++) {
        uint8_t idx = indices[i];
        if (idx >= m->schema.n_fields) return MZ_ERR_BAD_FIELD;
        values[idx] = vals[i];
    }

    /* delete old from sl */
    uint8_t ckey[MZ_CKEY_BUFSZ];
    size_t ck_len = mz_build_ckey(&m->schema, old_raw, member_id, mlen, ckey);
    mz_sl_delete(&m->sl, ckey, (uint16_t)ck_len);

    /* repack + reinsert */
    uint8_t raw[MZ_MAX_FIELDS * 8];
    mz_pack_raw(&m->schema, values, raw);
    mz_dict_put(&m->dict, member_id, mlen, raw, MZ_SCHEMA_BLOB(&m->schema));

    ck_len = mz_build_ckey_from_vals(&m->schema, values, member_id, mlen, ckey);
    mz_sl_insert(&m->sl, ckey, (uint16_t)ck_len);
    return MZ_OK;
}

/* ---------- incrby ---------- */

static int add_i64_check(int64_t a, int64_t b, int64_t *out) {
    /* overflow check via __builtin if available, else manual */
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, out);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return 1;
    *out = a + b;
    return 0;
#endif
}

int mzset_incrby(mzset_t *m, const void *member_id, size_t mlen,
                 uint8_t idx, mz_val_t delta, mz_val_t *out_new) {
    if (idx >= m->schema.n_fields) return MZ_ERR_BAD_FIELD;
    uint8_t t = m->schema.types[idx];
    if (t == MZ_T_BOOL) return MZ_ERR_BAD_FIELD;

    uint32_t old_len = 0;
    const uint8_t *old_raw = mz_dict_get(&m->dict, member_id, mlen, &old_len);
    if (!old_raw) return MZ_ERR_NO_MEMBER;

    mz_val_t values[MZ_MAX_FIELDS];
    mz_unpack_raw(&m->schema, old_raw, values);

    mz_val_t nv;
    switch (t) {
        case MZ_T_I64:
            if (add_i64_check(values[idx].i64, delta.i64, &nv.i64)) return MZ_ERR_OVERFLOW;
            values[idx] = nv;
            break;
        case MZ_T_F64:
            nv.f64 = values[idx].f64 + delta.f64;
            values[idx] = nv;
            break;
        case MZ_T_TS:
            nv.ts = values[idx].ts + delta.ts;   /* wrap semantics; fine for timestamps */
            values[idx] = nv;
            break;
    }
    if (out_new) *out_new = values[idx];

    /* rebuild sl entry */
    uint8_t ckey[MZ_CKEY_BUFSZ];
    size_t ck_len = mz_build_ckey(&m->schema, old_raw, member_id, mlen, ckey);
    mz_sl_delete(&m->sl, ckey, (uint16_t)ck_len);

    uint8_t raw[MZ_MAX_FIELDS * 8];
    mz_pack_raw(&m->schema, values, raw);
    mz_dict_put(&m->dict, member_id, mlen, raw, MZ_SCHEMA_BLOB(&m->schema));

    ck_len = mz_build_ckey_from_vals(&m->schema, values, member_id, mlen, ckey);
    mz_sl_insert(&m->sl, ckey, (uint16_t)ck_len);
    return MZ_OK;
}

/* ---------- rem ---------- */

int mzset_rem(mzset_t *m, const void *member_id, size_t mlen) {
    uint32_t old_len = 0;
    const uint8_t *old_raw = mz_dict_get(&m->dict, member_id, mlen, &old_len);
    if (!old_raw) return MZ_ERR_NO_MEMBER;

    uint8_t ckey[MZ_CKEY_BUFSZ];
    size_t ck_len = mz_build_ckey(&m->schema, old_raw, member_id, mlen, ckey);
    mz_sl_delete(&m->sl, ckey, (uint16_t)ck_len);
    mz_dict_del(&m->dict, member_id, mlen);
    return MZ_OK;
}

/* ---------- alter: append a field ---------- */

int mzset_alter_add(mzset_t *m, uint8_t new_type, uint8_t new_desc, mz_val_t defv) {
    if (m->schema.n_fields >= MZ_MAX_FIELDS) return MZ_ERR_BAD_FIELD;

    /* build new schema */
    uint8_t types[MZ_MAX_FIELDS];
    memcpy(types, m->schema.types, m->schema.n_fields);
    types[m->schema.n_fields] = new_type;
    uint16_t dirs = m->schema.dirs;
    if (new_desc) dirs |= (uint16_t)(1u << m->schema.n_fields);
    uint8_t new_n = m->schema.n_fields + 1;

    mz_schema_t new_sch;
    if (mz_schema_init(&new_sch, types, dirs, new_n) < 0) return MZ_ERR_BAD_FIELD;

    uint16_t old_blob  = MZ_SCHEMA_BLOB(&m->schema);
    uint16_t add_width = MZ_SCHEMA_BLOB(&new_sch) - old_blob;

    /* encode default value via NEW schema into scratch; extract [old_blob, +add_width). */
    uint8_t enc_scratch[MZ_MAX_FIELDS * 8];
    memset(enc_scratch, 0, sizeof enc_scratch);
    mz_encode_field(&new_sch, new_n - 1, defv, enc_scratch);

    /* raw default bytes */
    uint8_t raw_default[8];
    memset(raw_default, 0, sizeof raw_default);
    switch (new_type) {
        case MZ_T_I64:  memcpy(raw_default, &defv.i64, 8); break;
        case MZ_T_F64:  memcpy(raw_default, &defv.f64, 8); break;
        case MZ_T_TS:   memcpy(raw_default, &defv.ts,  8); break;
        case MZ_T_BOOL: raw_default[0] = defv.b ? 1 : 0;  break;
    }

    /* 1. Rebuild dict with extended values. Entries are variable-sized so easier
     *    to migrate into a fresh dict and swap. */
    mz_dict_t nd;
    if (mz_dict_init(&nd) < 0) return MZ_ERR_OVERFLOW;
    mz_dict_iter_t it; mz_dict_iter_init(&it, &m->dict);
    const uint8_t *k, *v; uint32_t klen, vlen;
    uint8_t new_val[MZ_MAX_FIELDS * 8];
    while (mz_dict_iter_next(&it, &k, &klen, &v, &vlen)) {
        memcpy(new_val, v, vlen);
        memcpy(new_val + vlen, raw_default, add_width);
        mz_dict_put(&nd, k, klen, new_val, vlen + add_width);
    }
    mz_dict_free(&m->dict);
    m->dict = nd;

    /* 2. Rewrite every skiplist node's ckey in place:
     *    [old_fields][member_id]  ->  [old_fields][enc_default][member_id]
     *    Since the encoded default is identical across members, relative ordering
     *    is preserved: no re-sort needed. */
    mz_sl_node_t *x = m->sl.header->lvls[0].forward;
    while (x) {
        uint16_t new_len = (uint16_t)(x->ckey_len + add_width);
        uint8_t *nk = (uint8_t *)malloc(new_len);
        if (!nk) return MZ_ERR_OVERFLOW;
        memcpy(nk, x->ckey, old_blob);
        memcpy(nk + old_blob, enc_scratch + old_blob, add_width);
        memcpy(nk + old_blob + add_width, x->ckey + old_blob, x->ckey_len - old_blob);
        free(x->ckey);
        x->ckey = nk;
        x->ckey_len = new_len;
        x = x->lvls[0].forward;
    }

    m->schema = new_sch;
    return MZ_OK;
}

/* ---------- score ---------- */

int mzset_score(const mzset_t *m, const void *member_id, size_t mlen, mz_val_t *out_vals) {
    uint32_t old_len = 0;
    const uint8_t *raw = mz_dict_get(&m->dict, member_id, mlen, &old_len);
    if (!raw) return MZ_ERR_NO_MEMBER;
    mz_unpack_raw(&m->schema, raw, out_vals);
    return MZ_OK;
}

/* ---------- rank ---------- */

uint64_t mzset_rank(const mzset_t *m, const void *member_id, size_t mlen, int rev) {
    uint32_t old_len = 0;
    const uint8_t *raw = mz_dict_get(&m->dict, member_id, mlen, &old_len);
    if (!raw) return 0;
    uint8_t ckey[MZ_CKEY_BUFSZ];
    size_t ck_len = mz_build_ckey(&m->schema, raw, member_id, mlen, ckey);
    uint64_t r = mz_sl_get_rank(&m->sl, ckey, (uint16_t)ck_len);
    if (r == 0) return 0;
    if (rev) return m->sl.length - r + 1;
    return r;
}

/* ---------- range (0-based inclusive, Redis-style negatives) ---------- */

void mzset_range(const mzset_t *m, int64_t start, int64_t stop, int rev,
                 mzset_range_cb cb, void *ud) {
    int64_t len = (int64_t)m->sl.length;
    if (len == 0) return;
    if (start < 0) start += len;
    if (stop  < 0) stop  += len;
    if (start < 0) start = 0;
    if (start >= len || start > stop) return;
    if (stop >= len) stop = len - 1;

    uint64_t r_first, r_last;
    if (!rev) {
        r_first = (uint64_t)start + 1;
        r_last  = (uint64_t)stop  + 1;
    } else {
        r_first = (uint64_t)(len - start);
        r_last  = (uint64_t)(len - stop);
    }

    mz_sl_node_t *x = mz_sl_get_by_rank(&m->sl, r_first);
    if (!x) return;

    uint64_t count = (r_first > r_last) ? (r_first - r_last + 1)
                                        : (r_last - r_first + 1);
    uint16_t blob = MZ_SCHEMA_BLOB(&m->schema);
    while (x && count > 0) {
        const uint8_t *mid = x->ckey + blob;
        uint16_t mid_len = x->ckey_len - blob;
        uint32_t vlen = 0;
        const uint8_t *raw = mz_dict_get(&m->dict, mid, mid_len, &vlen);
        if (cb(ud, mid, mid_len, raw)) break;
        x = rev ? x->backward : x->lvls[0].forward;
        count--;
    }
}
