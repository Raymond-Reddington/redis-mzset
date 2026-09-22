#include "mzset.h"

#include <string.h>
#include <stdlib.h>

/* ---------- schema ---------- */

static uint16_t field_width(uint8_t t) {
    switch (t) {
        case MZ_T_I64:
        case MZ_T_F64:
        case MZ_T_TS:   return 8;
        case MZ_T_BOOL: return 1;
        default:        return 0;
    }
}

int mz_schema_init(mz_schema_t *s, const uint8_t *types, uint16_t dirs, uint8_t n_fields) {
    if (n_fields == 0 || n_fields > MZ_MAX_FIELDS) return -1;
    s->n_fields = n_fields;
    s->dirs = dirs;
    uint16_t off = 0;
    for (uint8_t i = 0; i < n_fields; i++) {
        uint16_t w = field_width(types[i]);
        if (w == 0) return -1;
        s->types[i]   = types[i];
        s->offsets[i] = off;
        off += w;
    }
    s->offsets[n_fields] = off;
    /* zero the rest to keep the struct deterministic (for digest / RDB) */
    for (uint8_t i = n_fields; i < MZ_MAX_FIELDS; i++) {
        s->types[i] = 0;
        s->offsets[i + 1] = 0;
    }
    return 0;
}

/* ---------- byte-order helpers ---------- */

static void put_be64(uint8_t *out, uint64_t x) {
    out[0] = (uint8_t)(x >> 56);
    out[1] = (uint8_t)(x >> 48);
    out[2] = (uint8_t)(x >> 40);
    out[3] = (uint8_t)(x >> 32);
    out[4] = (uint8_t)(x >> 24);
    out[5] = (uint8_t)(x >> 16);
    out[6] = (uint8_t)(x >>  8);
    out[7] = (uint8_t)(x);
}

static uint64_t get_be64(const uint8_t *in) {
    return ((uint64_t)in[0] << 56) | ((uint64_t)in[1] << 48) |
           ((uint64_t)in[2] << 40) | ((uint64_t)in[3] << 32) |
           ((uint64_t)in[4] << 24) | ((uint64_t)in[5] << 16) |
           ((uint64_t)in[6] <<  8) | ((uint64_t)in[7]);
}

/* IEEE 754 double order-preserving transform. */
static uint64_t f64_to_orderable(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    /* If sign bit set (negative), flip all bits.
     * Otherwise flip only sign bit. */
    if (u & 0x8000000000000000ULL) return ~u;
    return u ^ 0x8000000000000000ULL;
}

static double orderable_to_f64(uint64_t u) {
    /* Inverse: if leading bit is 1 -> was positive; flip sign only.
     *          if leading bit is 0 -> was negative; flip all. */
    if (u & 0x8000000000000000ULL) u ^= 0x8000000000000000ULL;
    else                           u  = ~u;
    double d;
    memcpy(&d, &u, 8);
    return d;
}

/* ---------- encode/decode a single field ---------- */

void mz_encode_field(const mz_schema_t *s, uint8_t i, mz_val_t v, uint8_t *out) {
    uint8_t *p = out + s->offsets[i];
    uint16_t w = s->offsets[i + 1] - s->offsets[i];
    switch (s->types[i]) {
        case MZ_T_I64:
            put_be64(p, (uint64_t)v.i64 ^ 0x8000000000000000ULL);
            break;
        case MZ_T_F64:
            put_be64(p, f64_to_orderable(v.f64));
            break;
        case MZ_T_TS:
            put_be64(p, v.ts);
            break;
        case MZ_T_BOOL:
            p[0] = v.b ? 1 : 0;
            break;
        default: break;
    }
    if (MZ_FIELD_DESC(s, i)) {
        for (uint16_t k = 0; k < w; k++) p[k] ^= 0xFF;
    }
}

mz_val_t mz_decode_field(const mz_schema_t *s, uint8_t i, const uint8_t *in) {
    mz_val_t v;
    uint16_t w = s->offsets[i + 1] - s->offsets[i];
    uint8_t buf[8];
    const uint8_t *p = in + s->offsets[i];
    if (MZ_FIELD_DESC(s, i)) {
        for (uint16_t k = 0; k < w; k++) buf[k] = p[k] ^ 0xFF;
        p = buf;
    }
    switch (s->types[i]) {
        case MZ_T_I64:
            v.i64 = (int64_t)(get_be64(p) ^ 0x8000000000000000ULL);
            break;
        case MZ_T_F64:
            v.f64 = orderable_to_f64(get_be64(p));
            break;
        case MZ_T_TS:
            v.ts = get_be64(p);
            break;
        case MZ_T_BOOL:
            v.b = p[0] ? 1 : 0;
            break;
        default:
            memset(&v, 0, sizeof v);
    }
    return v;
}

/* ---------- pack/unpack raw fields ---------- */

/* raw layout: no encoding, values are stored in native byte order for cheap
 * pack/unpack. sizes match encoded widths so blob_size is the same. */

void mz_pack_raw(const mz_schema_t *s, const mz_val_t *vals, uint8_t *out) {
    for (uint8_t i = 0; i < s->n_fields; i++) {
        uint8_t *p = out + s->offsets[i];
        switch (s->types[i]) {
            case MZ_T_I64: memcpy(p, &vals[i].i64, 8); break;
            case MZ_T_F64: memcpy(p, &vals[i].f64, 8); break;
            case MZ_T_TS:  memcpy(p, &vals[i].ts,  8); break;
            case MZ_T_BOOL: p[0] = vals[i].b ? 1 : 0; break;
        }
    }
}

void mz_unpack_raw(const mz_schema_t *s, const uint8_t *in, mz_val_t *vals) {
    for (uint8_t i = 0; i < s->n_fields; i++) {
        const uint8_t *p = in + s->offsets[i];
        switch (s->types[i]) {
            case MZ_T_I64: memcpy(&vals[i].i64, p, 8); break;
            case MZ_T_F64: memcpy(&vals[i].f64, p, 8); break;
            case MZ_T_TS:  memcpy(&vals[i].ts,  p, 8); break;
            case MZ_T_BOOL: vals[i].b = p[0] ? 1 : 0; break;
        }
    }
}

/* ---------- composite key builders ---------- */

size_t mz_build_ckey(const mz_schema_t *s, const uint8_t *raw_blob,
                     const void *member_id, size_t member_id_len,
                     uint8_t *out) {
    /* encode each field from raw blob into out */
    mz_val_t v;
    for (uint8_t i = 0; i < s->n_fields; i++) {
        const uint8_t *p = raw_blob + s->offsets[i];
        switch (s->types[i]) {
            case MZ_T_I64: memcpy(&v.i64, p, 8); break;
            case MZ_T_F64: memcpy(&v.f64, p, 8); break;
            case MZ_T_TS:  memcpy(&v.ts,  p, 8); break;
            case MZ_T_BOOL: v.b = p[0]; break;
        }
        mz_encode_field(s, i, v, out);
    }
    uint16_t prefix = MZ_SCHEMA_BLOB(s);
    if (member_id_len > 0) memcpy(out + prefix, member_id, member_id_len);
    return (size_t)prefix + member_id_len;
}

size_t mz_build_ckey_from_vals(const mz_schema_t *s, const mz_val_t *vals,
                               const void *member_id, size_t member_id_len,
                               uint8_t *out) {
    for (uint8_t i = 0; i < s->n_fields; i++) {
        mz_encode_field(s, i, vals[i], out);
    }
    uint16_t prefix = MZ_SCHEMA_BLOB(s);
    if (member_id_len > 0) memcpy(out + prefix, member_id, member_id_len);
    return (size_t)prefix + member_id_len;
}
