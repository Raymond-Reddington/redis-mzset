#ifndef MZSET_H
#define MZSET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MZ_MAX_FIELDS       16
#define MZ_MAX_MEMBER_ID    512
#define MZ_SL_MAX_LEVEL     24        /* good for ~2^24 elements */
#define MZ_SL_P             0.25

/* ---------- field types ---------- */

typedef enum {
    MZ_T_I64  = 1,
    MZ_T_F64  = 2,
    MZ_T_TS   = 3,   /* timestamp, stored as uint64 */
    MZ_T_BOOL = 4,
} mz_field_type_t;

typedef union {
    int64_t  i64;
    double   f64;
    uint64_t ts;
    uint8_t  b;
} mz_val_t;

/* ---------- schema ---------- */

typedef struct {
    uint8_t  n_fields;
    uint8_t  types[MZ_MAX_FIELDS];        /* mz_field_type_t values */
    uint16_t dirs;                        /* bit i = 1  -> DESC */
    uint16_t offsets[MZ_MAX_FIELDS + 1];  /* offsets[i] = byte offset of field i;
                                             offsets[n_fields] = blob_size */
} mz_schema_t;

#define MZ_SCHEMA_BLOB(s) ((s)->offsets[(s)->n_fields])
#define MZ_FIELD_DESC(s, i) (((s)->dirs >> (i)) & 1u)

/* return -1 on invalid schema (bad type / n_fields out of range) */
int mz_schema_init(mz_schema_t *s, const uint8_t *types, uint16_t dirs, uint8_t n_fields);

/* encode / decode field i (in place: writes / reads at out+offsets[i]) */
void mz_encode_field(const mz_schema_t *s, uint8_t i, mz_val_t v, uint8_t *out);
mz_val_t mz_decode_field(const mz_schema_t *s, uint8_t i, const uint8_t *in);

/* raw pack / unpack: field values <-> raw fields blob (no encoding) */
void mz_pack_raw(const mz_schema_t *s, const mz_val_t *vals, uint8_t *out);
void mz_unpack_raw(const mz_schema_t *s, const uint8_t *in, mz_val_t *vals);

/* build composite key = encoded_prefix ++ member_id
 * out must have capacity of blob_size + member_id_len bytes.
 * returns total length. */
size_t mz_build_ckey(const mz_schema_t *s, const uint8_t *raw_blob,
                     const void *member_id, size_t member_id_len,
                     uint8_t *out);

/* Same but using raw values instead of packed blob. */
size_t mz_build_ckey_from_vals(const mz_schema_t *s, const mz_val_t *vals,
                               const void *member_id, size_t member_id_len,
                               uint8_t *out);

/* ---------- hashtable: member_id (bytes) -> fields_blob (bytes) ---------- */

typedef struct mz_dict_entry {
    struct mz_dict_entry *next;
    uint32_t key_len;
    uint32_t val_len;
    /* payload: [key_len bytes key][val_len bytes value] */
    uint8_t  payload[];
} mz_dict_entry_t;

typedef struct {
    mz_dict_entry_t **buckets;
    uint64_t          size;      /* number of buckets */
    uint64_t          used;      /* number of entries */
} mz_dict_t;

int   mz_dict_init(mz_dict_t *d);
void  mz_dict_free(mz_dict_t *d);
/* returns pointer to stored value (owned by dict); NULL if not found. */
const uint8_t *mz_dict_get(const mz_dict_t *d, const void *key, size_t klen, uint32_t *out_vlen);
/* insert or replace. returns 1 if inserted new, 0 if replaced. */
int mz_dict_put(mz_dict_t *d, const void *key, size_t klen,
                const void *val, size_t vlen);
int mz_dict_del(mz_dict_t *d, const void *key, size_t klen);

typedef struct {
    const mz_dict_t *d;
    uint64_t bucket;
    mz_dict_entry_t *entry;
} mz_dict_iter_t;

void mz_dict_iter_init(mz_dict_iter_t *it, const mz_dict_t *d);
/* Returns 0 when done, 1 with pointers set to current entry payload. */
int  mz_dict_iter_next(mz_dict_iter_t *it,
                       const uint8_t **key, uint32_t *klen,
                       const uint8_t **val, uint32_t *vlen);

/* ---------- skiplist (ordered by memcmp of ckey) ---------- */

typedef struct mz_sl_node {
    struct mz_sl_node *backward;
    uint8_t           *ckey;      /* owned; NULL for header */
    uint16_t           ckey_len;
    uint8_t            level;     /* number of forward pointers */
    struct mz_sl_lvl {
        struct mz_sl_node *forward;
        uint32_t           span;
    } lvls[];
} mz_sl_node_t;

typedef struct {
    mz_sl_node_t *header;
    mz_sl_node_t *tail;
    uint64_t      length;
    uint8_t       level;          /* current max level in use */
} mz_sl_t;

int   mz_sl_init(mz_sl_t *sl);
void  mz_sl_free(mz_sl_t *sl);
/* insert (ckey copied). ckey must not already exist. */
void  mz_sl_insert(mz_sl_t *sl, const uint8_t *ckey, uint16_t ckey_len);
/* delete by ckey. returns 1 if removed, 0 if not found. */
int   mz_sl_delete(mz_sl_t *sl, const uint8_t *ckey, uint16_t ckey_len);
/* 1-based rank of ckey; 0 if not found. */
uint64_t mz_sl_get_rank(const mz_sl_t *sl, const uint8_t *ckey, uint16_t ckey_len);
/* node at rank (1-based). NULL if out of range. */
mz_sl_node_t *mz_sl_get_by_rank(const mz_sl_t *sl, uint64_t rank);

/* ---------- mzset (the top-level object stored under a key) ---------- */

typedef struct {
    mz_schema_t schema;
    mz_dict_t   dict;      /* member_id -> raw fields blob */
    mz_sl_t     sl;        /* ordered by composite key */
} mzset_t;

mzset_t *mzset_create(const mz_schema_t *s);
void     mzset_free(mzset_t *m);

/* Return codes for mutation ops. */
#define MZ_OK             0
#define MZ_ERR_NO_MEMBER  1
#define MZ_ERR_EXISTS     2
#define MZ_ERR_BAD_FIELD  3
#define MZ_ERR_OVERFLOW   4

/* add or replace. if create_only=1, fails with MZ_ERR_EXISTS when member present. */
int mzset_add(mzset_t *m, const void *member_id, size_t mlen,
              const mz_val_t *vals, int create_only);
/* partial update: for each idx in indices[0..n), set vals[i]. */
int mzset_update(mzset_t *m, const void *member_id, size_t mlen,
                 const uint8_t *indices, const mz_val_t *vals, size_t n);
/* increment single field (must be i64/f64/ts). result written to *out_new if not NULL. */
int mzset_incrby(mzset_t *m, const void *member_id, size_t mlen,
                 uint8_t idx, mz_val_t delta, mz_val_t *out_new);
int mzset_rem(mzset_t *m, const void *member_id, size_t mlen);
/* Append one field to the schema. All existing members get default_val for the new field.
 * Relative ordering is preserved (default is identical across members) so no re-sort. */
int mzset_alter_add(mzset_t *m, uint8_t new_type, uint8_t new_desc, mz_val_t default_val);
/* fills *out_vals with n_fields values. returns MZ_ERR_NO_MEMBER if absent. */
int mzset_score(const mzset_t *m, const void *member_id, size_t mlen, mz_val_t *out_vals);
/* returns 1-based rank; 0 if not found. if rev=1, rank from the top. */
uint64_t mzset_rank(const mzset_t *m, const void *member_id, size_t mlen, int rev);
uint64_t mzset_card(const mzset_t *m);

/* Range by 1-based inclusive rank. If rev, iterate from tail.
 * Callback receives member_id bytes, its length, and pointer to raw fields blob.
 * Returning non-zero from callback aborts iteration. */
typedef int (*mzset_range_cb)(void *ud, const uint8_t *member_id, uint16_t mlen,
                              const uint8_t *raw_fields);
void mzset_range(const mzset_t *m, int64_t start, int64_t stop, int rev,
                 mzset_range_cb cb, void *ud);

#endif /* MZSET_H */
