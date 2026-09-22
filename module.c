#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"
#include "mzset.h"

#include <string.h>
#include <strings.h>   /* strncasecmp on POSIX */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static RedisModuleType *MZSetType;

/* ============================================================
 * helpers: parse / format
 * ============================================================ */

static const char *type_name(uint8_t t) {
    switch (t) {
        case MZ_T_I64:  return "i64";
        case MZ_T_F64:  return "f64";
        case MZ_T_TS:   return "ts";
        case MZ_T_BOOL: return "bool";
        default:        return "?";
    }
}

static int parse_field_type(const char *s, size_t len, uint8_t *out) {
    if (len == 3 && strncasecmp(s, "i64", 3) == 0) { *out = MZ_T_I64;  return 0; }
    if (len == 3 && strncasecmp(s, "f64", 3) == 0) { *out = MZ_T_F64;  return 0; }
    if (len == 2 && strncasecmp(s, "ts",  2) == 0) { *out = MZ_T_TS;   return 0; }
    if (len == 4 && strncasecmp(s, "bool",4) == 0) { *out = MZ_T_BOOL; return 0; }
    return -1;
}

/* returns 0 for ASC, 1 for DESC, -1 on error */
static int parse_dir(const char *s, size_t len) {
    if (len == 3 && strncasecmp(s, "asc",  3) == 0) return 0;
    if (len == 4 && strncasecmp(s, "desc", 4) == 0) return 1;
    return -1;
}

static int parse_schema_field(RedisModuleString *arg, uint8_t *type, uint8_t *desc) {
    size_t len;
    const char *s = RedisModule_StringPtrLen(arg, &len);
    const char *colon = memchr(s, ':', len);
    if (!colon) return -1;
    size_t tlen = colon - s;
    size_t dlen = len - tlen - 1;
    if (parse_field_type(s, tlen, type) < 0) return -1;
    int d = parse_dir(colon + 1, dlen);
    if (d < 0) return -1;
    *desc = (uint8_t)d;
    return 0;
}

static int parse_value(RedisModuleString *arg, uint8_t type, mz_val_t *out) {
    long long ll;
    double d;
    size_t len;
    const char *s = RedisModule_StringPtrLen(arg, &len);
    switch (type) {
        case MZ_T_I64:
            if (RedisModule_StringToLongLong(arg, &ll) != REDISMODULE_OK) return -1;
            out->i64 = (int64_t)ll;
            return 0;
        case MZ_T_F64:
            if (RedisModule_StringToDouble(arg, &d) != REDISMODULE_OK) return -1;
            if (d != d) return -1;              /* reject NaN */
            out->f64 = d;
            return 0;
        case MZ_T_TS:
            if (RedisModule_StringToLongLong(arg, &ll) != REDISMODULE_OK) return -1;
            if (ll < 0) return -1;
            out->ts = (uint64_t)ll;
            return 0;
        case MZ_T_BOOL:
            if (len == 1 && (s[0] == '0' || s[0] == '1')) {
                out->b = (uint8_t)(s[0] - '0');
                return 0;
            }
            return -1;
    }
    return -1;
}

static void reply_value(RedisModuleCtx *ctx, uint8_t type, mz_val_t v) {
    switch (type) {
        case MZ_T_I64:  RedisModule_ReplyWithLongLong(ctx, v.i64); break;
        case MZ_T_F64:  RedisModule_ReplyWithDouble(ctx, v.f64); break;
        case MZ_T_TS:   RedisModule_ReplyWithLongLong(ctx, (long long)v.ts); break;
        case MZ_T_BOOL: RedisModule_ReplyWithLongLong(ctx, v.b); break;
        default:        RedisModule_ReplyWithNull(ctx);
    }
}

/* Open key, return existing mzset* or NULL if empty.
 * On wrong type: replies with error and returns (mzset_t*)-1. */
#define WRONG_TYPE ((mzset_t *)(intptr_t)-1)

static mzset_t *open_mzset(RedisModuleCtx *ctx, RedisModuleString *keyname,
                           int write, RedisModuleKey **out_key) {
    int mode = write ? (REDISMODULE_READ | REDISMODULE_WRITE) : REDISMODULE_READ;
    RedisModuleKey *k = RedisModule_OpenKey(ctx, keyname, mode);
    int t = RedisModule_KeyType(k);
    if (t == REDISMODULE_KEYTYPE_EMPTY) {
        *out_key = k;
        return NULL;
    }
    if (t != REDISMODULE_KEYTYPE_MODULE ||
        RedisModule_ModuleTypeGetType(k) != MZSetType) {
        RedisModule_CloseKey(k);
        RedisModule_ReplyWithError(ctx, "WRONGTYPE Operation against a key holding the wrong kind of value");
        *out_key = NULL;
        return WRONG_TYPE;
    }
    *out_key = k;
    return (mzset_t *)RedisModule_ModuleTypeGetValue(k);
}

/* ============================================================
 * commands
 * ============================================================ */

/* MZ.CREATE key SCHEMA <type:dir> [<type:dir> ...] */
static int cmd_create(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) return RedisModule_WrongArity(ctx);

    size_t l;
    const char *w = RedisModule_StringPtrLen(argv[2], &l);
    if (l != 6 || strncasecmp(w, "SCHEMA", 6) != 0)
        return RedisModule_ReplyWithError(ctx, "ERR expected SCHEMA keyword");

    int nf = argc - 3;
    if (nf < 1 || nf > MZ_MAX_FIELDS)
        return RedisModule_ReplyWithError(ctx, "ERR n_fields out of range [1, 16]");

    uint8_t types[MZ_MAX_FIELDS];
    uint16_t dirs = 0;
    for (int i = 0; i < nf; i++) {
        uint8_t t, d;
        if (parse_schema_field(argv[3 + i], &t, &d) < 0)
            return RedisModule_ReplyWithError(ctx, "ERR bad schema field (expect <type>:<asc|desc>)");
        types[i] = t;
        if (d) dirs |= (uint16_t)(1u << i);
    }

    mz_schema_t s;
    if (mz_schema_init(&s, types, dirs, (uint8_t)nf) < 0)
        return RedisModule_ReplyWithError(ctx, "ERR invalid schema");

    RedisModuleKey *k = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR key already exists");
    }
    mzset_t *m = mzset_create(&s);
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithError(ctx, "ERR OOM"); }
    RedisModule_ModuleTypeSetValue(k, MZSetType, m);
    RedisModule_CloseKey(k);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* MZ.ADD key member v1 v2 ... vN [NX] */
static int cmd_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) return RedisModule_WrongArity(ctx);

    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 1, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithError(ctx, "ERR no such key"); }

    int nx = 0;
    int n_val_args = argc - 3;
    /* peek last arg for NX */
    size_t ll;
    const char *last = RedisModule_StringPtrLen(argv[argc - 1], &ll);
    if (ll == 2 && strncasecmp(last, "NX", 2) == 0) {
        nx = 1;
        n_val_args--;
    }
    if (n_val_args != m->schema.n_fields) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR field count mismatch");
    }

    mz_val_t vals[MZ_MAX_FIELDS];
    for (uint8_t i = 0; i < m->schema.n_fields; i++) {
        if (parse_value(argv[3 + i], m->schema.types[i], &vals[i]) < 0) {
            RedisModule_CloseKey(k);
            return RedisModule_ReplyWithError(ctx, "ERR bad field value");
        }
    }

    size_t mlen;
    const char *mid = RedisModule_StringPtrLen(argv[2], &mlen);
    if (mlen == 0 || mlen > MZ_MAX_MEMBER_ID) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR member length out of range");
    }

    int rc = mzset_add(m, mid, mlen, vals, nx);
    RedisModule_CloseKey(k);
    if (rc == MZ_ERR_EXISTS) return RedisModule_ReplyWithLongLong(ctx, 0);
    if (rc != MZ_OK) return RedisModule_ReplyWithError(ctx, "ERR add failed");
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, 1);
}

/* MZ.UPDATE key member idx v [idx v ...] */
static int cmd_update(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 5 || ((argc - 3) & 1)) return RedisModule_WrongArity(ctx);

    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 1, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithError(ctx, "ERR no such key"); }

    int n_pairs = (argc - 3) / 2;
    if (n_pairs > MZ_MAX_FIELDS) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR too many updates");
    }

    uint8_t indices[MZ_MAX_FIELDS];
    mz_val_t vals[MZ_MAX_FIELDS];
    for (int i = 0; i < n_pairs; i++) {
        long long idx;
        if (RedisModule_StringToLongLong(argv[3 + i*2], &idx) != REDISMODULE_OK ||
            idx < 0 || idx >= m->schema.n_fields) {
            RedisModule_CloseKey(k);
            return RedisModule_ReplyWithError(ctx, "ERR bad field index");
        }
        indices[i] = (uint8_t)idx;
        if (parse_value(argv[3 + i*2 + 1], m->schema.types[idx], &vals[i]) < 0) {
            RedisModule_CloseKey(k);
            return RedisModule_ReplyWithError(ctx, "ERR bad field value");
        }
    }

    size_t mlen;
    const char *mid = RedisModule_StringPtrLen(argv[2], &mlen);
    int rc = mzset_update(m, mid, mlen, indices, vals, n_pairs);
    RedisModule_CloseKey(k);
    if (rc == MZ_ERR_NO_MEMBER) return RedisModule_ReplyWithLongLong(ctx, 0);
    if (rc != MZ_OK) return RedisModule_ReplyWithError(ctx, "ERR update failed");
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, 1);
}

/* MZ.INCRBY key member idx delta */
static int cmd_incrby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 5) return RedisModule_WrongArity(ctx);

    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 1, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithError(ctx, "ERR no such key"); }

    long long idx;
    if (RedisModule_StringToLongLong(argv[3], &idx) != REDISMODULE_OK ||
        idx < 0 || idx >= m->schema.n_fields) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR bad field index");
    }
    uint8_t t = m->schema.types[idx];
    mz_val_t delta;
    if (parse_value(argv[4], t, &delta) < 0) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR bad delta");
    }

    size_t mlen;
    const char *mid = RedisModule_StringPtrLen(argv[2], &mlen);
    mz_val_t nv;
    int rc = mzset_incrby(m, mid, mlen, (uint8_t)idx, delta, &nv);
    RedisModule_CloseKey(k);
    if (rc == MZ_ERR_NO_MEMBER) return RedisModule_ReplyWithError(ctx, "ERR no such member");
    if (rc == MZ_ERR_OVERFLOW) return RedisModule_ReplyWithError(ctx, "ERR i64 overflow");
    if (rc == MZ_ERR_BAD_FIELD) return RedisModule_ReplyWithError(ctx, "ERR field not incrementable");
    if (rc != MZ_OK) return RedisModule_ReplyWithError(ctx, "ERR incrby failed");
    RedisModule_ReplicateVerbatim(ctx);
    reply_value(ctx, t, nv);
    return REDISMODULE_OK;
}

/* MZ.REM key member [member ...] */
static int cmd_rem(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 1, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithLongLong(ctx, 0); }

    long long removed = 0;
    for (int i = 2; i < argc; i++) {
        size_t mlen;
        const char *mid = RedisModule_StringPtrLen(argv[i], &mlen);
        if (mzset_rem(m, mid, mlen) == MZ_OK) removed++;
    }
    if (m->sl.length == 0) RedisModule_DeleteKey(k);
    RedisModule_CloseKey(k);
    if (removed) RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, removed);
}

/* MZ.SCORE key member */
static int cmd_score(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 0, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithNull(ctx); }

    size_t mlen;
    const char *mid = RedisModule_StringPtrLen(argv[2], &mlen);
    mz_val_t vals[MZ_MAX_FIELDS];
    int rc = mzset_score(m, mid, mlen, vals);
    if (rc != MZ_OK) { RedisModule_CloseKey(k); return RedisModule_ReplyWithNull(ctx); }

    RedisModule_ReplyWithArray(ctx, m->schema.n_fields);
    for (uint8_t i = 0; i < m->schema.n_fields; i++)
        reply_value(ctx, m->schema.types[i], vals[i]);
    RedisModule_CloseKey(k);
    return REDISMODULE_OK;
}

/* MZ.RANK key member [REV] */
static int cmd_rank(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3 || argc > 4) return RedisModule_WrongArity(ctx);
    int rev = 0;
    if (argc == 4) {
        size_t l; const char *s = RedisModule_StringPtrLen(argv[3], &l);
        if (l == 3 && strncasecmp(s, "REV", 3) == 0) rev = 1;
        else return RedisModule_ReplyWithError(ctx, "ERR expected REV");
    }
    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 0, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithNull(ctx); }

    size_t mlen;
    const char *mid = RedisModule_StringPtrLen(argv[2], &mlen);
    uint64_t r = mzset_rank(m, mid, mlen, rev);
    RedisModule_CloseKey(k);
    if (r == 0) return RedisModule_ReplyWithNull(ctx);
    return RedisModule_ReplyWithLongLong(ctx, (long long)(r - 1));  /* 0-based */
}

/* MZ.ALTER key ADD <type:dir> DEFAULT <value> */
static int cmd_alter(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 6) return RedisModule_WrongArity(ctx);
    size_t l;
    const char *s = RedisModule_StringPtrLen(argv[2], &l);
    if (l != 3 || strncasecmp(s, "ADD", 3) != 0)
        return RedisModule_ReplyWithError(ctx, "ERR expected ADD");

    uint8_t t, d;
    if (parse_schema_field(argv[3], &t, &d) < 0)
        return RedisModule_ReplyWithError(ctx, "ERR bad type:dir");

    s = RedisModule_StringPtrLen(argv[4], &l);
    if (l != 7 || strncasecmp(s, "DEFAULT", 7) != 0)
        return RedisModule_ReplyWithError(ctx, "ERR expected DEFAULT");

    mz_val_t defv;
    if (parse_value(argv[5], t, &defv) < 0)
        return RedisModule_ReplyWithError(ctx, "ERR bad default value");

    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 1, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithError(ctx, "ERR no such key"); }

    int rc = mzset_alter_add(m, t, d, defv);
    RedisModule_CloseKey(k);
    if (rc == MZ_ERR_BAD_FIELD)
        return RedisModule_ReplyWithError(ctx, "ERR too many fields (max 16)");
    if (rc != MZ_OK)
        return RedisModule_ReplyWithError(ctx, "ERR alter failed");
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* MZ.CARD key */
static int cmd_card(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 0, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    long long n = m ? (long long)mzset_card(m) : 0;
    RedisModule_CloseKey(k);
    return RedisModule_ReplyWithLongLong(ctx, n);
}

/* MZ.RANGE key start stop [REV] [WITHFIELDS] */
typedef struct {
    RedisModuleCtx *ctx;
    const mz_schema_t *schema;
    int withfields;
    long long emitted;
} range_ud_t;

static int range_cb(void *ud_, const uint8_t *mid, uint16_t mlen, const uint8_t *raw) {
    range_ud_t *ud = (range_ud_t *)ud_;
    if (ud->withfields) {
        RedisModule_ReplyWithArray(ud->ctx, 2);
        RedisModule_ReplyWithStringBuffer(ud->ctx, (const char *)mid, mlen);
        RedisModule_ReplyWithArray(ud->ctx, ud->schema->n_fields);
        mz_val_t vals[MZ_MAX_FIELDS];
        mz_unpack_raw(ud->schema, raw, vals);
        for (uint8_t i = 0; i < ud->schema->n_fields; i++)
            reply_value(ud->ctx, ud->schema->types[i], vals[i]);
    } else {
        RedisModule_ReplyWithStringBuffer(ud->ctx, (const char *)mid, mlen);
    }
    ud->emitted++;
    return 0;
}

static int cmd_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4 || argc > 6) return RedisModule_WrongArity(ctx);

    long long start, stop;
    if (RedisModule_StringToLongLong(argv[2], &start) != REDISMODULE_OK ||
        RedisModule_StringToLongLong(argv[3], &stop) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR bad start/stop");

    int rev = 0, withfields = 0;
    for (int i = 4; i < argc; i++) {
        size_t l; const char *s = RedisModule_StringPtrLen(argv[i], &l);
        if      (l == 3  && strncasecmp(s, "REV",        3)  == 0) rev = 1;
        else if (l == 10 && strncasecmp(s, "WITHFIELDS", 10) == 0) withfields = 1;
        else return RedisModule_ReplyWithError(ctx, "ERR expected REV or WITHFIELDS");
    }

    RedisModuleKey *k;
    mzset_t *m = open_mzset(ctx, argv[1], 0, &k);
    if (m == WRONG_TYPE) return REDISMODULE_OK;
    if (!m) { RedisModule_CloseKey(k); return RedisModule_ReplyWithArray(ctx, 0); }

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    range_ud_t ud = { ctx, &m->schema, withfields, 0 };
    mzset_range(m, (int64_t)start, (int64_t)stop, rev, range_cb, &ud);
    RedisModule_ReplySetArrayLength(ctx, ud.emitted);

    RedisModule_CloseKey(k);
    return REDISMODULE_OK;
}

/* ============================================================
 * data type callbacks
 * ============================================================ */

static void mzset_free_cb(void *value) {
    mzset_free((mzset_t *)value);
}

/* RDB layout:
 *   u8  version                 (== 1)
 *   u8  n_fields
 *   u8  types[n_fields]
 *   u16 dirs
 *   u64 count
 *   for each entry:
 *      str member_id
 *      str raw_fields_blob (fixed length == blob_size)
 */
static void mzset_rdb_save(RedisModuleIO *rdb, void *value) {
    mzset_t *m = (mzset_t *)value;
    RedisModule_SaveUnsigned(rdb, 1);                       /* format version */
    RedisModule_SaveUnsigned(rdb, m->schema.n_fields);
    for (uint8_t i = 0; i < m->schema.n_fields; i++)
        RedisModule_SaveUnsigned(rdb, m->schema.types[i]);
    RedisModule_SaveUnsigned(rdb, m->schema.dirs);
    RedisModule_SaveUnsigned(rdb, m->dict.used);

    mz_dict_iter_t it; mz_dict_iter_init(&it, &m->dict);
    const uint8_t *key, *val; uint32_t klen, vlen;
    while (mz_dict_iter_next(&it, &key, &klen, &val, &vlen)) {
        RedisModule_SaveStringBuffer(rdb, (const char *)key, klen);
        RedisModule_SaveStringBuffer(rdb, (const char *)val, vlen);
    }
}

static void *mzset_rdb_load(RedisModuleIO *rdb, int encver) {
    if (encver != 1) return NULL;
    (void)RedisModule_LoadUnsigned(rdb);   /* format version */
    uint8_t nf   = (uint8_t)RedisModule_LoadUnsigned(rdb);
    uint8_t types[MZ_MAX_FIELDS];
    for (uint8_t i = 0; i < nf; i++) types[i] = (uint8_t)RedisModule_LoadUnsigned(rdb);
    uint16_t dirs = (uint16_t)RedisModule_LoadUnsigned(rdb);
    uint64_t cnt  = RedisModule_LoadUnsigned(rdb);

    mz_schema_t s;
    if (mz_schema_init(&s, types, dirs, nf) < 0) return NULL;
    mzset_t *m = mzset_create(&s);
    if (!m) return NULL;

    uint8_t ckey[MZ_MAX_FIELDS * 8 + MZ_MAX_MEMBER_ID];
    for (uint64_t i = 0; i < cnt; i++) {
        size_t klen, vlen;
        char *key = RedisModule_LoadStringBuffer(rdb, &klen);
        char *val = RedisModule_LoadStringBuffer(rdb, &vlen);
        if (!key || !val || vlen != MZ_SCHEMA_BLOB(&m->schema)) {
            if (key) RedisModule_Free(key);
            if (val) RedisModule_Free(val);
            mzset_free(m);
            return NULL;
        }
        mz_dict_put(&m->dict, key, klen, val, vlen);
        size_t ck = mz_build_ckey(&m->schema, (const uint8_t *)val, key, klen, ckey);
        mz_sl_insert(&m->sl, ckey, (uint16_t)ck);
        RedisModule_Free(key);
        RedisModule_Free(val);
    }
    return m;
}

/* AOF rewrite: emit MZ.CREATE + one MZ.ADD per member. */
static void mzset_aof_rewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    mzset_t *m = (mzset_t *)value;
    RedisModuleCtx *ctx = RedisModule_GetContextFromIO(aof);

    /* CREATE */
    {
        RedisModuleString *args[2 + MZ_MAX_FIELDS];
        int n = 0;
        args[n++] = key;
        args[n++] = RedisModule_CreateString(ctx, "SCHEMA", 6);
        for (uint8_t i = 0; i < m->schema.n_fields; i++) {
            char tmp[16];
            int len = snprintf(tmp, sizeof(tmp), "%s:%s",
                               type_name(m->schema.types[i]),
                               MZ_FIELD_DESC(&m->schema, i) ? "desc" : "asc");
            args[n++] = RedisModule_CreateString(ctx, tmp, (size_t)len);
        }
        RedisModule_EmitAOF(aof, "mz.create", "v", args, n);
        for (int i = 1; i < n; i++) RedisModule_FreeString(ctx, args[i]);
    }

    /* one ADD per member */
    mz_dict_iter_t it; mz_dict_iter_init(&it, &m->dict);
    const uint8_t *kb, *vb; uint32_t klen, vlen;
    while (mz_dict_iter_next(&it, &kb, &klen, &vb, &vlen)) {
        mz_val_t vals[MZ_MAX_FIELDS];
        mz_unpack_raw(&m->schema, vb, vals);

        RedisModuleString *args[2 + MZ_MAX_FIELDS + 1];
        int n = 0;
        args[n++] = key;
        args[n++] = RedisModule_CreateString(ctx, (const char *)kb, klen);
        for (uint8_t i = 0; i < m->schema.n_fields; i++) {
            char tmp[32];
            int len;
            switch (m->schema.types[i]) {
                case MZ_T_I64:  len = snprintf(tmp, sizeof(tmp), "%lld", (long long)vals[i].i64); break;
                case MZ_T_F64:  len = snprintf(tmp, sizeof(tmp), "%.17g", vals[i].f64); break;
                case MZ_T_TS:   len = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)vals[i].ts); break;
                case MZ_T_BOOL: len = snprintf(tmp, sizeof(tmp), "%d", vals[i].b ? 1 : 0); break;
                default: len = 1; tmp[0] = '0'; tmp[1] = 0;
            }
            args[n++] = RedisModule_CreateString(ctx, tmp, (size_t)len);
        }
        RedisModule_EmitAOF(aof, "mz.add", "v", args, n);
        for (int i = 1; i < n; i++) RedisModule_FreeString(ctx, args[i]);
    }
}

static size_t mzset_mem_usage(const void *value) {
    const mzset_t *m = (const mzset_t *)value;
    size_t total = sizeof(*m);
    total += m->dict.size * sizeof(void *);
    /* dict entries: header + key + val (fields blob) */
    mz_dict_iter_t it; mz_dict_iter_init(&it, &m->dict);
    const uint8_t *k, *v; uint32_t kl, vl;
    while (mz_dict_iter_next(&it, &k, &kl, &v, &vl))
        total += sizeof(mz_dict_entry_t) + kl + vl;
    /* skiplist: header + per-node cost (approx) */
    total += sizeof(mz_sl_node_t) + MZ_SL_MAX_LEVEL * 12;
    /* avg 1/(1-p) ≈ 1.33 levels per node; conservative estimate */
    total += m->sl.length * (sizeof(mz_sl_node_t) + 2 * 12);
    total += m->sl.length * (MZ_SCHEMA_BLOB(&m->schema) + 16);
    return total;
}

static void mzset_digest(RedisModuleDigest *md, void *value) {
    mzset_t *m = (mzset_t *)value;
    mz_dict_iter_t it; mz_dict_iter_init(&it, &m->dict);
    const uint8_t *k, *v; uint32_t kl, vl;
    while (mz_dict_iter_next(&it, &k, &kl, &v, &vl)) {
        RedisModule_DigestAddStringBuffer(md, (const char *)k, kl);
        RedisModule_DigestAddStringBuffer(md, (const char *)v, vl);
        RedisModule_DigestEndSequence(md);
    }
}

/* ============================================================
 * OnLoad
 * ============================================================ */

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv; (void)argc;
    if (RedisModule_Init(ctx, "mzset", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleTypeMethods tm = {
        .version     = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load    = mzset_rdb_load,
        .rdb_save    = mzset_rdb_save,
        .aof_rewrite = mzset_aof_rewrite,
        .mem_usage   = mzset_mem_usage,
        .free        = mzset_free_cb,
        .digest      = mzset_digest,
    };
    MZSetType = RedisModule_CreateDataType(ctx, "MZSetType", 1, &tm);
    if (MZSetType == NULL) return REDISMODULE_ERR;

    #define CMD(name, fn, flags) \
        if (RedisModule_CreateCommand(ctx, name, fn, flags, 1, 1, 1) == REDISMODULE_ERR) \
            return REDISMODULE_ERR

    CMD("mz.create", cmd_create, "write deny-oom");
    CMD("mz.add",    cmd_add,    "write deny-oom");
    CMD("mz.update", cmd_update, "write deny-oom");
    CMD("mz.incrby", cmd_incrby, "write deny-oom");
    CMD("mz.rem",    cmd_rem,    "write");
    CMD("mz.alter",  cmd_alter,  "write deny-oom");
    CMD("mz.score",  cmd_score,  "readonly fast");
    CMD("mz.rank",   cmd_rank,   "readonly fast");
    CMD("mz.range",  cmd_range,  "readonly");
    CMD("mz.card",   cmd_card,   "readonly fast");

    return REDISMODULE_OK;
}
