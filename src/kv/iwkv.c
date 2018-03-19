#include "iwkv.h"
#include "iwlog.h"
#include "iwarr.h"
#include "iwutils.h"
#include "iwfsmfile.h"
#include "iwcfg.h"
#include "khash.h"
#include "ksort.h"
#include <pthread.h>
#include <stdatomic.h>

// Hardcoded requirements (fixme):
static_assert(sizeof(off_t) == 8, "sizeof(off_t) == 8 bytes");
static_assert(sizeof(size_t) == 8, "sizeof(size_t) == 8 bytes");

// IWKV magic number
#define IWKV_MAGIC 0x69776b76

// IWDB magic number
#define IWDB_MAGIC 0x69776462

// Max key + value size: 255Mb
#define IWKV_MAX_KVSZ 0xfffffff

// Max database file size: ~255Gb
#define IWKV_MAX_DBSZ 0x3fffffffc0

// Size of KV fsm block as power of 2
#define IWKV_FSM_BPOW 6

// Length of KV fsm header in bytes
#define KVHDRSZ 255

// Number of skip list levels
#define SLEVELS 30

#define AANUM (2 * SLEVELS + 3 /* (new block created) + (db block may be updated) + (first db block) */)

// Lower key length in SBLK
#define SBLK_LKLEN 64 

// Size of database start block in bytes
#define DB_SZ (5 * (1 << IWKV_FSM_BPOW))

// Size of `SBLK` in bytes
#define SBLK_SZ (4 * (1 << IWKV_FSM_BPOW))

// Number of `KV` blocks in KVBLK
#define KVBLK_IDXNUM 32

// Initial `KVBLK` size power of 2
#define KVBLK_INISZPOW 9

// KVBLK header size: blen:u1,idxsz:u2
#define KVBLK_HDRSZ 3

// Max kvp offset bytes
#define KVP_MAX_OFF_VLEN 8

// Max kvp len 0xfffffffULL bytes
#define KVP_MAX_LEN_VLEN 5

#define KVBLK_MAX_IDX_SZ ((KVP_MAX_OFF_VLEN + KVP_MAX_LEN_VLEN) * KVBLK_IDXNUM)

// Max non KV size [blen:u1,idxsz:u2,[ps1:vn,pl1:vn,...,ps63,pl63]
#define KVBLK_MAX_NKV_SZ (KVBLK_HDRSZ + KVBLK_MAX_IDX_SZ)

#define ADDR2BLK(addr_) ((addr_) >> IWKV_FSM_BPOW)

#define BLK2ADDR(blk_) (((off_t) (blk_)) << IWKV_FSM_BPOW)

#ifdef IW_TESTS
volatile int8_t iwkv_next_level = -1;
#endif

struct IWKV;
struct IWDB;

typedef uint32_t blkn_t;
typedef uint32_t dbid_t;

/* Key/Value pair stored in `KVBLK` */
typedef struct KV {
  uint8_t *key;
  uint8_t *val;
  size_t keysz;
  size_t valsz;
} KV;

/* Ket/Value (KV) index: Offset and length. */
typedef struct KVP {
  off_t off;      /**< KV block offset relative to `end` of KVBLK */
  uint32_t len;   /**< Length of kv pair block */
  uint8_t  ridx;  /**< Position of the auctually persisted slot in `KVBLK` */
} KVP;

typedef enum {
  KVBLK_DURTY = 1 /**< KVBLK data is durty and should be flushed to mm */
} kvblk_flags_t;

typedef enum {
  RMKV_SYNC = 1,
  RMKV_NO_RESIZE = 1 << 1
} kvblk_rmkv_opts_t;

/* KVBLK: [szpow:u1,idxsz:u2,[ps0:vn,pl0:vn,..., ps32,pl32]____[[KV],...]] */
typedef struct KVBLK {
  IWDB db;
  off_t addr;                 /**< Block address */
  off_t maxoff;               /**< Max pair offset */
  uint16_t idxsz;             /**< Size of KV pairs index in bytes */
  int8_t zidx;                /**< Index of first empty pair slot (zero index), or -1 */
  uint8_t szpow;              /**< Block size as power of 2 */
  kvblk_flags_t flags;        /**< Flags */
  KVP pidx[KVBLK_IDXNUM];     /**< KV pairs index */
} KVBLK;

typedef enum {
  SBLK_FULL_LKEY  = 1UL,       /**< The lowest `SBLK` key is fully contained in `SBLK`. Persistent flag. */
  SBLK_DB         = 1UL << 3,  /**< This block is the start database block. */
  SBLK_DURTY      = 1UL << 4,  /**< Block data changed, block marked as durty and needs to be persisted */
  SBLK_LKEY_ONLY   = 1UL << 5,
} sblk_flags_t;

#define SBLK_PERSISTENT_FLAGS (SBLK_FULL_LKEY)

/* Database: [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n[30]:u4,c[30]:u4]:u257 */
struct IWDB {
  // SBH
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sblk_flags_t flags;         /**< Flags */
  // !SBH
  IWKV iwkv;
  iwdb_flags_t dbflg;         /**< Database specific flags */
  pthread_rwlock_t rwl;       /**< Database API RW lock */
  dbid_t id;                  /**< Database ID */
  uint64_t next_db_addr;      /**< Next IWDB addr */
  struct IWDB *next;          /**< Next IWDB meta */
  struct IWDB *prev;          /**< Prev IWDB meta */
  volatile int32_t wk_count;  /**< Number of active database workers */
  volatile atomic_bool open;  /**< True if DB is in OPEN state */
  uint32_t lcnt[SLEVELS];     /**< SBLK count per level */
};

#define IWDB_DUP_FLAGS (IWDB_DUP_UINT32_VALS | IWDB_DUP_UINT64_VALS )

/* Skiplist block: [u1:flags,lvl:u1,lkl:u1,pnum:u1,p0:u4,kblk:u4,[pi0:u1,... pi32],n0-n29:u4,pad:u28,lk:u64]:u256 */
typedef struct SBLK {
  // SBH
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sblk_flags_t flags;         /**< Flags */
  uint8_t lvl;                /**< Skip list node level */
  blkn_t p0;                  /**< Prev node, if IWDB it is the last node */
  blkn_t n[SLEVELS];          /**< Next nodes */
  // !SBH
  KVBLK *kvblk;               /**< Associated KVBLK */
  blkn_t kvblkn;              /**< Associated KVBLK block number */
  int8_t lkl;                 /**< Lower key length within a buffer */
  int8_t pnum;                /**< Number of active kv indexes in `SBLK::pi` */
  int8_t pi[KVBLK_IDXNUM];    /**< Sorted KV slots, value is an index of kv slot in `KVBLK` */
  uint8_t lk[SBLK_LKLEN];     /**< Lower key buffer */
} SBLK;

KHASH_MAP_INIT_INT(DBS, IWDB)

/** IWKV instance */
struct IWKV {
  IWFS_FSM fsm;               /**< FSM pool */
  pthread_rwlock_t rwl;       /**< API RW lock */
  IWDB first_db;              /**< First database in chain */
  IWDB last_db;               /**< Last database in chain */
  khash_t(DBS) *dbs;          /**< Database id -> IWDB mapping */
  iwkv_openflags oflags;      /**< Open flags */
  pthread_cond_t wk_cond;     /**< Workers cond variable */
  pthread_mutex_t wk_mtx;     /**< Workers cond mutext */
  volatile int32_t wk_count;  /**< Number of active workers */
  volatile atomic_bool open;  /**< True if kvstore is in OPEN state */
};

typedef enum {
  IWLCTX_PUT = 1,             /**< Put key value operation */
  IWLCTX_DEL = 1 << 1,        /**< Delete key operation */
} iwlctx_op_t;

/** Database lookup context */
typedef struct IWLCTX {
  IWDB db;
  const IWKV_val *key;        /**< Search key */
  IWKV_val *val;              /**< Update value */
  SBLK *lower;                /**< Next to upper bound block */
  SBLK *upper;                /**< Upper bound block */
  SBLK *nb;                   /**< New block */
  SBLK *dblk;                 /**< First database block */
  off_t upper_addr;           /**< Upper block address used in `_lx_del_lr()` */
#ifndef NDEBUG
  uint32_t num_cmps;
#endif
  uint8_t saan;               /**< Position of next free `SBLK` element in the `saa` area */
  uint8_t kaan;               /**< Position of next free `KVBLK` element in the `kaa` area */
  int8_t nlvl;                /**< Level of new inserted/deleted `SBLK` node. -1 if no new node inserted/deleted */
  iwlctx_op_t op;             /**< Context operation */
  iwkv_opflags opflags;       /**< Operation flags */
  sblk_flags_t sblk_flags;    /**< `SBLK` flags applied to all new/looked blocks in this context */
  SBLK *plower[SLEVELS];      /**< Pinned lower nodes per level */
  SBLK *pupper[SLEVELS];      /**< Pinned upper nodes per level */
  SBLK saa[AANUM];            /**< `SBLK` allocation area */
  KVBLK kaa[AANUM];           /**< `KVBLK` allocation area */
} IWLCTX;

/** Cursor context */
struct IWKV_cursor {
  SBLK *cn;                   /**< Current `SBLK` node */
  off_t dbaddr;               /**< Database address used as `cn` */
  uint8_t cnpos;              /**< Position in the current `SBLK` node */
  bool closed;                /**< Cursor closed */
  IWLCTX lx;                  /**< Lookup context */
};

void iwkvd_kvblk(FILE *f, KVBLK *kb, int maxvlen);
void iwkvd_sblk(FILE *f, IWLCTX *lx, SBLK *sb, int flags);
void iwkvd_db(FILE *f, IWDB db, int flags);

#define ENSURE_OPEN(iwkv_) \
  if (!iwkv_ || !(iwkv_->open)) return IW_ERROR_INVALID_STATE

#define ENSURE_OPEN_DB(db_) \
  if (!(db_) || !(db_)->iwkv || !(db_)->open || !((db_)->iwkv->open)) return IW_ERROR_INVALID_STATE

#define API_RLOCK(iwkv_, rci_) \
  ENSURE_OPEN(iwkv_);  \
  rci_ = pthread_rwlock_rdlock(&(iwkv_)->rwl); \
  if (rci_) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_)

IW_INLINE iwrc _api_rlock(IWKV iwkv)  {
  int rci;
  API_RLOCK(iwkv, rci);
  return 0;
}

#define API_WLOCK(iwkv_, rci_) \
  ENSURE_OPEN(iwkv_);  \
  rci_ = pthread_rwlock_wrlock(&(iwkv_)->rwl); \
  if (rci_) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_)

IW_INLINE iwrc _api_wlock(IWKV iwkv)  {
  int rci;
  API_WLOCK(iwkv, rci);
  return 0;
}

#define API_UNLOCK(iwkv_, rci_, rc_)  \
  rci_ = pthread_rwlock_unlock(&(iwkv_)->rwl); \
  if (rci_) IWRC(iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_), rc_)

#define API_DB_RLOCK(db_, rci_)                               \
  do {                                                        \
    API_RLOCK((db_)->iwkv, rci_);                             \
    rci_ = pthread_rwlock_rdlock(&(db_)->rwl);                \
    if (rci_) {                                               \
      pthread_rwlock_unlock(&(db_)->iwkv->rwl);               \
      return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_);  \
    }                                                         \
  } while(0)

IW_INLINE iwrc _api_db_rlock(IWDB db)  {
  int rci;
  API_DB_RLOCK(db, rci);
  return 0;
}

#define API_DB_WLOCK(db_, rci_)                               \
  do {                                                        \
    API_RLOCK((db_)->iwkv, rci_);                             \
    rci_ = pthread_rwlock_wrlock(&(db_)->rwl);                \
    if (rci_) {                                               \
      pthread_rwlock_unlock(&(db_)->iwkv->rwl);               \
      return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_);  \
    }                                                         \
  } while(0)

IW_INLINE iwrc _api_db_wlock(IWDB db)  {
  int rci;
  API_DB_WLOCK(db, rci);
  return 0;
}

#define API_DB_UNLOCK(db_, rci_, rc_)                                     \
  do {                                                                    \
    rci_ = pthread_rwlock_unlock(&(db_)->rwl);                            \
    if (rci_) IWRC(iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_), rc_);  \
    API_UNLOCK((db_)->iwkv, rci_, rc_);                                   \
  } while(0)

#define AAPOS_INC(aan_)         \
  do {                          \
    if ((aan_) < AANUM - 1) {   \
      (aan_) = (aan_) + 1;      \
    } else {                    \
      (aan_) = 0;               \
    }                           \
  } while(0)


// SBLK
// [u1:flags,lvl:u1,lkl:u1,pnum:u1,p0:u4,kblk:u4,[pi0:u1,... pi32],n0-n29:u4,pad:u28,lk:u64]:u256

#define SOFF_FLAGS_U1     0
#define SOFF_LVL_U1       (SOFF_FLAGS_U1 + 1)
#define SOFF_LKL_U1       (SOFF_LVL_U1 + 1)
#define SOFF_PNUM_U1      (SOFF_LKL_U1 + 1)
#define SOFF_P0_U4        (SOFF_PNUM_U1 + 1)
#define SOFF_KBLK_U4      (SOFF_P0_U4 + 4)
#define SOFF_PI0_U1       (SOFF_KBLK_U4 + 4)
#define SOFF_N0_U4        (SOFF_PI0_U1 + 1 * KVBLK_IDXNUM) 
#define SOFF_LK           (SOFF_N0_U4 + 4 * SLEVELS + 28)
#define SOFF_END          (SOFF_LK + SBLK_LKLEN)
static_assert(SOFF_END == 256, "SBLK size");

// DB
// [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n[30]:u4,c[30]:u4]:u257
#define DOFF_MAGIC_U4     0
#define DOFF_DBFLG_U1     (DOFF_MAGIC_U4 + 4)
#define DOFF_DBID_U4      (DOFF_DBFLG_U1 + 1)
#define DOFF_NEXTDB_U4    (DOFF_DBID_U4 + 4)
#define DOFF_P0_U4        (DOFF_NEXTDB_U4 + 4)
#define DOFF_N0_U4        (DOFF_P0_U4 + 4)
#define DOFF_C0_U4        (DOFF_N0_U4 + 4 * SLEVELS)
#define DOFF_END          (DOFF_C0_U4 + 4 * SLEVELS)
static_assert(DOFF_END == 257, "DB size");

// KVBLK
// [szpow:u1,idxsz:u2,[ps1:vn,pl1:vn,...,ps63,pl63]____[[_KV],...]] // KVBLK
#define KBLK_SZPOW_OFF   0

//-------------------------- UTILS

IW_INLINE int _cmp_key(iwdb_flags_t dbflg, void *v1, int v1len, void *v2, int v2len) {
  int rv;
  if (!dbflg) {
    rv = strncmp(v2, v1, MIN(v1len, v2len));
    return rv == 0 ? v2len - v1len : rv;
  } else if (dbflg & IWDB_UINT64_KEYS) {
    uint64_t n1, n2;
    if (v1len != 8 || v2len != 8) { // should never happen
      assert(false);
      return 0;
    }
    memcpy(&n1, v1, v1len);
    n1 = IW_ITOHLL(n1);
    memcpy(&n2, v2, v2len);
    n2 = IW_ITOHLL(n2);
    return n1 > n2 ? -1 : n1 < n2 ? 1 : 0;
  } else if (dbflg & IWDB_UINT32_KEYS) {
    uint32_t n1, n2;
    if (v1len != 4 || v2len != 4) { // should never happen
      assert(false);
      return 0;
    }
    memcpy(&n1, v1, v1len);
    n1 = IW_ITOHL(n1);
    memcpy(&n2, v2, v2len);
    memcpy(&n2, v2, v2len);
    n2 = IW_ITOHL(n2);
    return n1 > n2 ? -1 : n1 < n2 ? 1 : 0;
  } else {
    rv = strncmp(v2, v1, MIN(v1len, v2len));
    return rv == 0 ? v2len - v1len : rv;
  }
}

IW_INLINE void _kv_val_dispose(IWKV_val *v) {
  if (v) {
    if (v->data) {
      free(v->data);
    }
    v->size = 0;
    v->data = 0;
  }
}

IW_INLINE void _kv_dispose(IWKV_val *key, IWKV_val *val) {
  _kv_val_dispose(key);
  _kv_val_dispose(val);
}

void iwkv_val_dispose(IWKV_val *v) {
  _kv_val_dispose(v);
}

void iwkv_kv_dispose(IWKV_val *key, IWKV_val *val) {
  _kv_dispose(key, val);
}

IW_INLINE void _num2lebuf(uint8_t buf[static 8], void *numdata, int sz) {
  assert(sz == 4 || sz == 8);
  if (sz > 4) {
    uint64_t llv;
    memcpy(&llv, numdata, sizeof(llv));
    llv = IW_HTOILL(llv);
    memcpy(buf, &llv, sizeof(llv));
  } else {
    uint32_t lv;
    memcpy(&lv, numdata, sizeof(lv));
    lv = IW_HTOIL(lv);
    memcpy(buf, &lv, sizeof(lv));
  }
}

static int _u4cmp(const void *o1, const void *o2) {
  uint32_t v1, v2;
  memcpy(&v1, o1, sizeof(uint32_t));
  memcpy(&v2, o2, sizeof(uint32_t));
  v1 = IW_ITOHL(v1);
  v2 = IW_ITOHL(v2);
  if (v1 > v2) {
    return 1;
  } else if (v1 < v2) {
    return -1;
  } else {
    return 0;
  }
}

static int _u8cmp(const void *o1, const void *o2) {
  uint64_t v1, v2;
  memcpy(&v1, o1, sizeof(uint64_t));
  memcpy(&v2, o2, sizeof(uint64_t));
  v1 = IW_ITOHLL(v1);
  v2 = IW_ITOHLL(v2);
  if (v1 > v2) {
    return 1;
  } else if (v1 < v2) {
    return -1;
  } else {
    return 0;
  }
}

//-------------------------- IWKV/IWDB WORKERS

static iwrc _iwkv_worker_inc_nolk(IWKV iwkv) {
  if (!iwkv || !iwkv->open) {
    return IW_ERROR_INVALID_STATE;
  }
  int rci = pthread_mutex_lock(&iwkv->wk_mtx);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  if (!iwkv->open) {
    pthread_mutex_unlock(&iwkv->wk_mtx);
    return IW_ERROR_INVALID_STATE;
  }
  ++iwkv->wk_count;
  pthread_cond_broadcast(&iwkv->wk_cond);
  pthread_mutex_unlock(&iwkv->wk_mtx);
  return 0;
}

static iwrc _iwkv_worker_dec_nolk(IWKV iwkv) {
  if (!iwkv) {
    return IW_ERROR_INVALID_STATE;
  }
  int rci = pthread_mutex_lock(&iwkv->wk_mtx);
  if (rci) {
    // Last chanсe to be safe
    --iwkv->wk_count;
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  --iwkv->wk_count;
  pthread_cond_broadcast(&iwkv->wk_cond);
  pthread_mutex_unlock(&iwkv->wk_mtx);
  return 0;
}

static iwrc _db_worker_inc_nolk(IWDB db) {
  if (!db || !db->iwkv || !db->iwkv->open || !db->open) {
    return IW_ERROR_INVALID_STATE;
  }
  IWKV iwkv = db->iwkv;
  int rci = pthread_mutex_lock(&iwkv->wk_mtx);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  if (!iwkv->open || !db->open) {
    pthread_mutex_unlock(&iwkv->wk_mtx);
    return IW_ERROR_INVALID_STATE;
  }
  ++iwkv->wk_count;
  ++db->wk_count;
  pthread_cond_broadcast(&iwkv->wk_cond);
  pthread_mutex_unlock(&iwkv->wk_mtx);
  return 0;
}

static iwrc _db_worker_dec_nolk(IWDB db) {
  if (!db || !db->iwkv) { // do not use ENSURE_OPEN_DB here
    return IW_ERROR_INVALID_STATE;
  }
  IWKV iwkv = db->iwkv;
  int rci = pthread_mutex_lock(&iwkv->wk_mtx);
  if (rci) {
    // Last chanсe to be safe
    --iwkv->wk_count;
    --db->wk_count;
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  --iwkv->wk_count;
  --db->wk_count;
  pthread_cond_broadcast(&iwkv->wk_cond);
  pthread_mutex_unlock(&iwkv->wk_mtx);
  return 0;
}

static iwrc _wnw_iwkw_wl(IWKV iwkv) {
  int rci = pthread_rwlock_wrlock(&iwkv->rwl);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  return 0;
}

static iwrc _wnw_db_wl(IWDB db) {
  int rci = pthread_rwlock_wrlock(&db->rwl);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  return 0;
}

static iwrc _wnw(IWKV iwkv, iwrc(*after)(IWKV iwkv)) {
  iwrc rc = 0;
  int rci = pthread_mutex_lock(&iwkv->wk_mtx);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  while (iwkv->wk_count > 0) {
    pthread_cond_wait(&iwkv->wk_cond, &iwkv->wk_mtx);
  }
  if (after) {
    rc = after(iwkv);
  }
  rci = pthread_mutex_unlock(&iwkv->wk_mtx);
  if (rci) {
    IWRC(iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci), rc);
  }
  return rc;
}

static iwrc _wnw_db(IWDB db, iwrc(*after)(IWDB db)) {
  iwrc rc = 0;
  IWKV iwkv = db->iwkv;
  int rci = pthread_mutex_lock(&iwkv->wk_mtx);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  while (db->wk_count > 0) {
    pthread_cond_wait(&iwkv->wk_cond, &iwkv->wk_mtx);
  }
  if (after) {
    rc = after(db);
  }
  rci = pthread_mutex_unlock(&iwkv->wk_mtx);
  if (rci) {
    IWRC(iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci), rc);
  }
  return rc;
}

//--------------------------  DB

static iwrc _db_at(IWKV iwkv, IWDB *dbp, off_t addr, uint8_t *mm) {
  iwrc rc = 0;
  uint8_t *rp;
  uint32_t lv;
  int rci;
  IWDB db = calloc(1, sizeof(struct IWDB));
  *dbp = 0;
  if (!db) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  rci = pthread_rwlock_init(&db->rwl, 0);
  if (rci) {
    free(db);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n[30]:u4,c[30]:u8]:u377
  db->flags = SBLK_DB;
  db->addr = addr;
  db->db = db;
  db->iwkv = iwkv;
  rp = mm + addr;
  IW_READLV(rp, lv, lv);
  if (lv != IWDB_MAGIC) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  IW_READBV(rp, lv, db->dbflg);
  IW_READLV(rp, lv, db->id);
  IW_READLV(rp, lv, db->next_db_addr);
  db->next_db_addr = BLK2ADDR(db->next_db_addr); // blknum -> addr
  rp = mm + addr + DOFF_C0_U4;
  for (int i = 0; i < SLEVELS; ++i) {
    IW_READLV(rp, lv, db->lcnt[i]);
  }
  db->open = true;
  *dbp = db;
finish:
  if (rc)  {
    pthread_rwlock_destroy(&db->rwl);
    free(db);
  }
  return rc;
}

static void _db_save(IWDB db, uint8_t *mm) {
  uint32_t lv;
  uint8_t *wp = mm + db->addr;
  db->next_db_addr = db->next ? db->next->addr : 0;
  // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n0-n29:u4]
  IW_WRITELV(wp, lv, IWDB_MAGIC);
  IW_WRITEBV(wp, lv, db->dbflg);
  IW_WRITELV(wp, lv, db->id);
  IW_WRITELV(wp, lv, ADDR2BLK(db->next_db_addr));
}

static iwrc _db_load_chain(IWKV iwkv, off_t addr, uint8_t *mm) {
  iwrc rc;
  int rci;
  IWDB db = 0, ndb;
  if (!addr) return 0;
  do {
    rc = _db_at(iwkv, &ndb, addr, mm);
    RCRET(rc);
    if (db) {
      db->next = ndb;
      ndb->prev = db;
    } else {
      iwkv->first_db = ndb;
    }
    db = ndb;
    addr = db->next_db_addr;
    iwkv->last_db = db;
    khiter_t k = kh_put(DBS, iwkv->dbs, db->id, &rci);
    if (rci != -1) {
      kh_value(iwkv->dbs, k) = db;
    } else {
      rc = IW_ERROR_FAIL;
      return rc;
    }
  } while (db->next_db_addr);
  return rc;
}

static void _db_release_lw(IWDB *dbp) {
  assert(dbp && *dbp);
  pthread_rwlock_destroy(&(*dbp)->rwl);
  free(*dbp);
  *dbp = 0;
}

typedef struct DISPOSE_DB_CTX {
  IWKV iwkv;
  IWDB *dbp;
  blkn_t sbn; // First `SBLK` block in DB
  pthread_t thr;
} DISPOSE_DB_CTX;

static void *_db_dispose_chain_thr(void *op) {
  assert(op);
  iwrc rc = 0;
  DISPOSE_DB_CTX *dctx = op;
  pthread_detach(dctx->thr);
  uint8_t *mm, kvszpow;
  IWFS_FSM *fsm = &dctx->iwkv->fsm;
  blkn_t sbn = dctx->sbn, kvblkn;
  while (sbn) {
    off_t sba = BLK2ADDR(sbn);
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCBREAK(rc);
    memcpy(&kvblkn, mm + sba + SOFF_KBLK_U4, 4);
    kvblkn = IW_ITOHL(kvblkn);
    memcpy(&sbn, mm + sba + SOFF_N0_U4, 4);
    sbn = IW_ITOHL(sbn);
    if (kvblkn) {
      memcpy(&kvszpow, mm +  BLK2ADDR(kvblkn) + KBLK_SZPOW_OFF, 1);
    }
    rc = fsm->release_mmap(fsm);
    RCBREAK(rc);
    // Deallocate `SBLK`
    rc = fsm->deallocate(fsm, sba, SBLK_SZ);
    if (rc) {
      iwlog_ecode_error3(rc);
      rc = 0;
    }
    // Deallocate `KVBLK`
    if (kvblkn) {
      rc = fsm->deallocate(fsm, BLK2ADDR(kvblkn), 1 << kvszpow);
      if (rc) {
        iwlog_ecode_error3(rc);
        rc = 0;
      }
    }
  }
  _db_release_lw(dctx->dbp);
  rc = _iwkv_worker_dec_nolk(dctx->iwkv);
  if (rc) {
    iwlog_ecode_error3(rc);
  }
  free(dctx);
  return 0;
}

static iwrc _db_destroy_lw(IWDB *dbp) {
  int rci;
  iwrc rc;
  uint8_t *mm;
  IWDB db = *dbp;
  IWKV iwkv = db->iwkv;
  IWDB prev = db->prev;
  IWDB next = db->next;
  IWFS_FSM *fsm = &db->iwkv->fsm;
  uint32_t first_sblkn;
  bool dec_worker = true;
  
  kh_del(DBS, db->iwkv->dbs, db->id);
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  if (prev) {
    prev->next = next;
    _db_save(prev, mm);
  }
  if (next) {
    next->prev = prev;
    _db_save(next, mm);
  }
  // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n[30]:u4,c[30]:u4]:u257
  memcpy(&first_sblkn, mm + db->addr + DOFF_N0_U4, 4);
  first_sblkn = IW_ITOHL(first_sblkn);
  fsm->release_mmap(fsm);
  if (db->iwkv->first_db && db->iwkv->first_db->addr == db->addr) {
    uint64_t llv;
    db->iwkv->first_db = next;
    llv = next ? next->addr : 0;
    llv = IW_HTOILL(llv);
    rc = fsm->writehdr(fsm, sizeof(uint32_t) /*skip magic*/, &llv, sizeof(llv));
  }
  if (db->iwkv->last_db && db->iwkv->last_db->addr == db->addr) {
    db->iwkv->last_db = prev;
  }
  // Cleanup DB
  off_t db_addr = db->addr;
  if (first_sblkn) {
    DISPOSE_DB_CTX *dctx = malloc(sizeof(*dctx));
    if (dctx) {
      db->open = false;
      dctx->sbn = first_sblkn;
      dctx->iwkv = db->iwkv;
      dctx->dbp = dbp;
      rci = pthread_create(&dctx->thr, 0, _db_dispose_chain_thr, dctx);
      if (rci) {
        free(dctx);
        rc = iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
      } else {
        dec_worker = false;
      }
    } else {
      rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }
  }
  IWRC(fsm->deallocate(fsm, db_addr, DB_SZ), rc);
  if (dec_worker) {
    _db_release_lw(dbp);
    _iwkv_worker_dec_nolk(iwkv);
  }
  return rc;
}

static iwrc _db_create_lw(IWKV iwkv, dbid_t dbid, iwdb_flags_t dbflg, IWDB *odb) {
  iwrc rc;
  int rci;
  uint8_t *mm = 0;
  off_t baddr = 0, blen;
  IWFS_FSM *fsm = &iwkv->fsm;
  *odb = 0;
  IWDB db = calloc(1, sizeof(struct IWDB));
  if (!db) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  rci = pthread_rwlock_init(&db->rwl, 0);
  if (rci) {
    free(db);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  rc = fsm->allocate(fsm, DB_SZ, &baddr, &blen,
                     IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE | IWFSM_ALLOC_NO_STATS);
  if (rc) {
    _db_release_lw(&db);
    return rc;
  }
  db->iwkv = iwkv;
  db->dbflg = dbflg;
  db->addr = baddr;
  db->id = dbid;
  db->prev = iwkv->last_db;
  if (!iwkv->first_db) {
    uint64_t llv;
    iwkv->first_db = db;
    llv = db->addr;
    llv = IW_HTOILL(llv);
    rc = fsm->writehdr(fsm, sizeof(uint32_t) /*skip magic*/, &llv, sizeof(llv));
  } else if (iwkv->last_db) {
    iwkv->last_db->next = db;
  }
  iwkv->last_db = db;
  khiter_t k = kh_put(DBS, iwkv->dbs, db->id, &rci);
  if (rci != -1) {
    kh_value(iwkv->dbs, k) = db;
  } else {
    RCGO(IW_ERROR_FAIL, finish);
  }
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  _db_save(db, mm);
  if (db->prev) {
    _db_save(db->prev, mm);
  }
  db->open = true;
  *odb = db;
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  if (rc) {
    fsm->deallocate(fsm, baddr, blen);
    _db_release_lw(&db);
  }
  return rc;
}

//--------------------------  KVBLK

IW_INLINE void _kvblk_create(IWLCTX *lx, off_t baddr, off_t blen, uint8_t kvbpow, KVBLK **oblk) {
  KVBLK *kblk = &lx->kaa[lx->kaan];
  assert((1ULL << kvbpow) == blen);
  kblk->db = lx->db;
  kblk->addr = baddr;
  kblk->maxoff = 0;
  kblk->idxsz = 2 * IW_VNUMSIZE(0) * KVBLK_IDXNUM;
  kblk->zidx = 0;
  kblk->szpow = kvbpow;
  kblk->flags = KVBLK_DURTY;
  memset(kblk->pidx, 0, sizeof(kblk->pidx));
  *oblk = kblk;
  AAPOS_INC(lx->kaan);
}

IW_INLINE iwrc _kvblk_destroy(KVBLK **kbp) {
  assert(kbp && *kbp && (*kbp)->db && (*kbp)->szpow && (*kbp)->addr);
  KVBLK *blk = *kbp;
  IWFS_FSM *fsm = &blk->db->iwkv->fsm;
  return fsm->deallocate(fsm, blk->addr, 1ULL << blk->szpow);
}

IW_INLINE iwrc _kvblk_peek_key(const KVBLK *kb, uint8_t idx, const uint8_t *mm, uint8_t **obuf, uint32_t *olen) {
  if (kb->pidx[idx].len) {
    uint32_t klen, step;
    const uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kb->pidx[idx].off;
    IW_READVNUMBUF(rp, klen, step);
    if (!klen) {
      *obuf = 0;
      *olen = 0;
      return IWKV_ERROR_CORRUPTED;
    }
    rp += step;
    *obuf = (uint8_t *) rp;
    *olen = klen;
  } else {
    *obuf = 0;
    *olen = 0;
  }
  return 0;
}

IW_INLINE void _kvblk_peek_val(const KVBLK *kb, uint8_t idx, const uint8_t *mm, uint8_t **obuf, uint32_t *olen) {
  assert(idx < KVBLK_IDXNUM);
  if (kb->pidx[idx].len) {
    uint32_t klen, step;
    const uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kb->pidx[idx].off;
    IW_READVNUMBUF(rp, klen, step);
    rp += step;
    rp += klen;
    *obuf = (uint8_t *) rp;
    *olen = kb->pidx[idx].len - klen - step;
  } else {
    *obuf = 0;
    *olen = 0;
  }
}

static iwrc _kvblk_getkey(KVBLK *kb, uint8_t *mm, uint8_t idx, IWKV_val *key) {
  assert(mm && idx < KVBLK_IDXNUM);
  int32_t klen;
  int step;
  KVP *kvp = &kb->pidx[idx];
  if (!kvp->len) {
    key->data = 0;
    key->size = 0;
    return 0;
  }
  // [klen:vn,key,value]
  uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  IW_READVNUMBUF(rp, klen, step);
  rp += step;
  if (klen < 1 || klen > kvp->len || klen > kvp->off) {
    return IWKV_ERROR_CORRUPTED;
  }
  key->size = klen;
  key->data = malloc(key->size);
  if (!key->data) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  memcpy(key->data, rp, key->size);
  rp += key->size;
  return 0;
}

static iwrc _kvblk_getvalue(KVBLK *kb, uint8_t *mm, uint8_t idx, IWKV_val *val) {
  assert(mm && idx < KVBLK_IDXNUM);
  int32_t klen;
  int step;
  KVP *kvp = &kb->pidx[idx];
  if (!kvp->len) {
    val->data = 0;
    val->size = 0;
    return 0;
  }
  // [klen:vn,key,value]
  uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  IW_READVNUMBUF(rp, klen, step);
  rp += step;
  if (klen < 1 || klen > kvp->len || klen > kvp->off) {
    return IWKV_ERROR_CORRUPTED;
  }
  rp += klen;
  if (kvp->len > klen + step) {
    val->size = kvp->len - klen - step;
    val->data = malloc(val->size);
    if (!val->data) {
      iwrc rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      val->data = 0;
      val->size = 0;
      return rc;
    }
    memcpy(val->data, rp, val->size);
  } else {
    val->data = 0;
    val->size = 0;
  }
  return 0;
}

static iwrc _kvblk_getkv(KVBLK *kb, uint8_t *mm, uint8_t idx, IWKV_val *key, IWKV_val *val) {
  assert(mm && idx < KVBLK_IDXNUM);
  int32_t klen;
  int step;
  KVP *kvp = &kb->pidx[idx];
  if (!kvp->len) {
    key->data = 0;
    key->size = 0;
    val->data = 0;
    val->size = 0;
    return 0;
  }
  // [klen:vn,key,value]
  uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  IW_READVNUMBUF(rp, klen, step);
  rp += step;
  if (klen < 1 || klen > kvp->len || klen > kvp->off) {
    return IWKV_ERROR_CORRUPTED;
  }
  key->size = klen;
  key->data = malloc(key->size);
  if (!key->data) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  memcpy(key->data, rp, key->size);
  rp += klen;
  if (kvp->len > klen + step) {
    val->size = kvp->len - klen - step;
    val->data = malloc(val->size);
    if (!val->data) {
      iwrc rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      free(key->data);
      key->data = 0;
      key->size = 0;
      val->size = 0;
      return rc;
    }
    memcpy(val->data, rp, val->size);
  } else {
    val->data = 0;
    val->size = 0;
  }
  return 0;
}

static iwrc _kvblk_at_mm(IWLCTX *lx, off_t addr, uint8_t *mm, KVBLK *kbp, KVBLK **blkp) {
  uint8_t *rp;
  uint16_t sv;
  int step;
  iwrc rc = 0;
  KVBLK *kb = kbp ? kbp : &lx->kaa[lx->kaan];
  kb->db = lx->db;
  kb->addr = addr;
  kb->maxoff = 0;
  kb->idxsz = 0;
  kb->zidx = -1;
  kb->szpow = 0;
  kb->flags = 0;
  memset(kb->pidx, 0, sizeof(kb->pidx));
  
  *blkp = 0;
  rp = mm + addr;
  memcpy(&kb->szpow, rp, 1);
  rp += 1;
  IW_READSV(rp, sv, kb->idxsz);
  if (IW_UNLIKELY(kb->idxsz > KVBLK_MAX_IDX_SZ)) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    IW_READVNUMBUF64(rp, kb->pidx[i].off, step);
    rp += step;
    IW_READVNUMBUF(rp, kb->pidx[i].len, step);
    rp += step;
    if (kb->pidx[i].len) {
      if (IW_UNLIKELY(!kb->pidx[i].off)) {
        rc = IWKV_ERROR_CORRUPTED;
        goto finish;
      }
      if (kb->pidx[i].off > kb->maxoff) {
        kb->maxoff = kb->pidx[i].off;
      }
    } else if (kb->zidx < 0) {
      kb->zidx = i;
    }
    kb->pidx[i].ridx = i;
  }
  *blkp = kb;
  assert(rp - (mm + addr) <= (1ULL << kb->szpow));
  if (!kbp) {
    AAPOS_INC(lx->kaan);
  }
finish:
  return rc;
}

IW_INLINE iwrc _kvblk_at(IWLCTX *lx, SBLK *sblk, KVBLK **blkp) {
  if (sblk->kvblk) {
    *blkp = sblk->kvblk;
    return 0;
  }
  iwrc rc;
  uint8_t *mm;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  *blkp = 0;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _kvblk_at_mm(lx, BLK2ADDR(sblk->kvblkn), mm, 0, blkp);
  IWRC(fsm->release_mmap(fsm), rc);
  return rc;
}

IW_INLINE off_t _kvblk_compacted_offset(KVBLK *kb) {
  off_t coff = 0;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    coff += kb->pidx[i].len;
  }
  return coff;
}

IW_INLINE off_t _kvblk_compacted_dsize(KVBLK *kb) {
  off_t coff = KVBLK_HDRSZ;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    coff += kb->pidx[i].len;
    coff += IW_VNUMSIZE32(kb->pidx[i].len);
    coff += IW_VNUMSIZE(kb->pidx[i].off);
  }
  return coff;
}

static void _kvblk_sync_mm(KVBLK *kb, uint8_t *mm) {
  if (!(kb->flags & KVBLK_DURTY)) {
    return;
  }
  uint16_t sp;
  uint8_t *szp;
  uint8_t *wp = mm + kb->addr;
  memcpy(wp, &kb->szpow, 1);
  wp += 1;
  szp = wp;
  wp += sizeof(uint16_t);
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    KVP *kvp = &kb->pidx[i];
    IW_SETVNUMBUF64(sp, wp, kvp->off);
    wp += sp;
    IW_SETVNUMBUF(sp, wp, kvp->len);
    wp += sp;
  }
  sp = wp - szp - sizeof(uint16_t);
  kb->idxsz = sp;
  assert(kb->idxsz <= KVBLK_MAX_IDX_SZ);
  sp = IW_HTOIS(sp);
  memcpy(szp, &sp, sizeof(uint16_t));
  assert(wp - (mm + kb->addr) <= (1ULL << kb->szpow));
  kb->flags &= ~KVBLK_DURTY;
}

#define _kvblk_sort_kv_lt(v1, v2) \
  (((v1).off > 0 ? (v1).off : -1UL) < ((v2).off > 0 ? (v2).off : -1UL))

KSORT_INIT(kvblk, KVP, _kvblk_sort_kv_lt)

static void _kvblk_compact_mm(KVBLK *kb, bool sync, uint8_t *mm) {
  uint8_t i;
  off_t coff = _kvblk_compacted_offset(kb);
  if (coff == kb->maxoff) { // already compacted
    return;
  }
  KVP tidx[KVBLK_IDXNUM];
  KVP tidx_tmp[KVBLK_IDXNUM];
  uint16_t idxsiz = 0;
  uint8_t *wp = mm + kb->addr + (1ULL << kb->szpow);
  memcpy(tidx, kb->pidx, sizeof(tidx));
  ks_mergesort_kvblk(KVBLK_IDXNUM, tidx, tidx_tmp);
  
  coff = 0;
  for (i = 0; i < KVBLK_IDXNUM && tidx[i].off; ++i) {
#ifndef NDEBUG
    if (i > 0) {
      assert(tidx[i - 1].off < tidx[i].off);
    }
#endif
    KVP *kvp = &kb->pidx[tidx[i].ridx];
    off_t noff = coff + kvp->len;
    if (kvp->off > noff) {
      assert(noff <= (1ULL << kb->szpow) && kvp->len <= noff);
      memmove(wp - noff, wp - kvp->off, kvp->len);
      kvp->off = noff;
    }
    coff += kvp->len;
    idxsiz += IW_VNUMSIZE(kvp->off);
    idxsiz += IW_VNUMSIZE32(kvp->len);
  }
  idxsiz += (KVBLK_IDXNUM - i) * 2;
  for (i = 0; i < KVBLK_IDXNUM; ++i) {
    if (!kb->pidx[i].len)  {
      kb->zidx = i;
      break;
    }
  }
  assert(idxsiz <= kb->idxsz);
  kb->idxsz = idxsiz;
  kb->maxoff = coff;
  if (i == KVBLK_IDXNUM) {
    kb->zidx = -1;
  }
  kb->flags |= KVBLK_DURTY;
  assert(_kvblk_compacted_offset(kb) == kb->maxoff);
  if (sync) {
    _kvblk_sync_mm(kb, mm);
  }
}

IW_INLINE off_t _kvblk_maxkvoff(KVBLK *kb) {
  off_t off = 0;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    if (kb->pidx[i].off > off) {
      off = kb->pidx[i].off;
    }
  }
  return off;
}

static iwrc _kvblk_rmkv(KVBLK *kb, uint8_t idx, kvblk_rmkv_opts_t opts) {
  iwrc rc = 0;
  uint8_t *mm = 0;
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  if (kb->pidx[idx].off >= kb->maxoff) {
    kb->maxoff = 0;
    for (int i = 0; i < KVBLK_IDXNUM; ++i) {
      if (i != idx && kb->pidx[i].off > kb->maxoff) {
        kb->maxoff = kb->pidx[i].off;
      }
    }
  }
  kb->pidx[idx].len = 0;
  kb->pidx[idx].off = 0;
  kb->flags |= KVBLK_DURTY;
  if (kb->zidx < 0 || idx < kb->zidx) {
    kb->zidx = idx;
  }
  if (!(RMKV_NO_RESIZE & opts) && kb->szpow > KVBLK_INISZPOW) {
    off_t nlen = 1ULL << kb->szpow;
    uint64_t dsz = _kvblk_compacted_dsize(kb);
    if (nlen >= 2 * dsz) {
      uint8_t npow = kb->szpow - 1;
      while (npow > KVBLK_INISZPOW && (1ULL << (npow - 1)) >= dsz) {
        --npow;
      }
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
      _kvblk_compact_mm(kb, false, mm);
      off_t maxoff = _kvblk_maxkvoff(kb);
      assert((1ULL << npow) > maxoff);
      memmove(mm + kb->addr + (1ULL << npow) - maxoff,
              mm + kb->addr + nlen - maxoff,
              maxoff);
      fsm->release_mmap(fsm);
      mm = 0;
      rc = fsm->reallocate(fsm, (1ULL << npow), &kb->addr, &nlen,
                           IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE | IWFSM_ALLOC_NO_STATS);
      RCGO(rc, finish);
      kb->szpow = npow;
      assert(nlen == (1ULL << kb->szpow));
      opts |= RMKV_SYNC;
    }
  }
  if (RMKV_SYNC & opts) {
    if (!mm) {
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
    }
    _kvblk_sync_mm(kb, mm);
  }
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  return rc;
}


static iwrc _kvblk_addkv(KVBLK *kb,
                         const IWKV_val *key,
                         const IWKV_val *val,
                         int8_t *oidx,
                         iwkv_opflags opflags,
                         bool internal) {
  iwrc rc = 0;
  off_t msz;    // max available free space
  off_t rsz;    // required size to add new key/value pair
  off_t noff;   // offset of new kvpair from end of block
  uint8_t *mm, *wp;
  size_t i, sp;
  KVP *kvp;
  IWDB db = kb->db;
  IWFS_FSM *fsm = &db->iwkv->fsm;
  *oidx = -1;
  bool compacted = false;
  IWKV_val *uval = (IWKV_val *) val, sval;
  off_t psz = IW_VNUMSIZE(key->size) + key->size + uval->size; // required KVP size
  
#ifndef NDEBUG
  uint8_t *sptr;
#endif
  
  if (kb->zidx < 0) {    
    return _IWKV_ERROR_KVBLOCK_FULL;
  }
  // DUP
  if (!internal && (db->dbflg & IWDB_DUP_FLAGS)) {
    if (opflags & IWKV_DUP_REMOVE) {
      return IWKV_ERROR_NOTFOUND;
    }
    if (((db->dbflg & IWDB_DUP_UINT32_VALS) && val->size != 4) ||
        ((db->dbflg & IWDB_DUP_UINT64_VALS) && val->size != 8)) {
      return IWKV_ERROR_DUP_VALUE_SIZE;
    }
    uint32_t lv = 1;
    uint8_t vbuf[8];
    _num2lebuf(vbuf, val->data, val->size);
    uval = &sval;
    uval->data = malloc(4 + val->size);
    uval->size = 4 + val->size;
    psz += 4;
    lv = IW_HTOIL(lv);
    wp = uval->data;
    memcpy(wp, &lv, 4);
    memcpy(wp + 4, vbuf, val->size);
  }
  // !DUP
  if (psz > IWKV_MAX_KVSZ) {
    if (uval != val) {
      _kv_val_dispose(uval);
    }
    return IWKV_ERROR_MAXKVSZ;
  }
  
start:
  msz = (1ULL << kb->szpow) - KVBLK_HDRSZ - kb->idxsz - kb->maxoff;
  assert(msz > 0);
  noff = kb->maxoff + psz;
  rsz = psz + IW_VNUMSIZE(noff) + IW_VNUMSIZE(psz);
  
  if (msz < rsz) { // not enough space
    if (!compacted) {
      compacted = true;
      if (_kvblk_compacted_offset(kb) != kb->maxoff) {
        rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
        RCGO(rc, finish);
        _kvblk_compact_mm(kb, false, mm);
        fsm->release_mmap(fsm);
        goto start;
      }
    }
    // resize the whole block
    off_t nsz = (rsz - msz) + (1ULL << kb->szpow);
    uint8_t npow = kb->szpow;
    while ((1ULL << ++npow) < nsz);
    off_t naddr = kb->addr,
          nlen = (1ULL << kb->szpow);
          
    assert(!fsm->check_allocation_status(fsm, naddr, nlen, true));
    rc = fsm->reallocate(fsm, (1ULL << npow), &naddr, &nlen,
                         IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE | IWFSM_ALLOC_NO_STATS);
    RCGO(rc, finish);
    assert(nlen == (1ULL << npow));
    // Move pairs area
    // [hdr..[pairs]] =reallocate=> [hdr..[pairs]_____] =memove=> [hdr.._____[pairs]]
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCGO(rc, finish);
    kb->addr = naddr;
    memmove(mm + naddr + nlen - kb->maxoff,
            mm + naddr + (1ULL << kb->szpow) - kb->maxoff, kb->maxoff);
    fsm->release_mmap(fsm);
    kb->szpow = npow;
    goto start;
    
  }
  *oidx = kb->zidx;
  kvp = &kb->pidx[kb->zidx];
  kvp->len = psz;
  kvp->off = noff;
  kvp->ridx = kb->zidx;
  kb->maxoff = noff;
  kb->flags |= KVBLK_DURTY;
  for (i = 0; i < KVBLK_IDXNUM; ++i) {
    if (!kb->pidx[i].len && i != kb->zidx) {
      kb->zidx = i;
      break;
    }
  }
  if (i >= KVBLK_IDXNUM) {
    kb->zidx = -1;
  }
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  assert((1ULL << kb->szpow) >= KVBLK_HDRSZ + kb->idxsz + kb->maxoff);
  assert(kvp->off < (1ULL << kb->szpow) && kvp->len <= kvp->off);
  wp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
#ifndef NDEBUG
  sptr = wp;
#endif
  // [klen:vn,key,value]
  IW_SETVNUMBUF(sp, wp, key->size);
  wp += sp;
  memcpy(wp, key->data, key->size);
  wp += key->size;
  memcpy(wp, uval->data, uval->size);
  wp += uval->size;
#ifndef NDEBUG
  assert(wp - sptr == kvp->len);
#endif
  fsm->release_mmap(fsm);
finish:
  if (uval != val) {
    _kv_val_dispose(uval);
  }
  return rc;
}

static iwrc _kvblk_updatev(KVBLK *kb,
                           int8_t *idxp,
                           const IWKV_val *key, /* Nullable */
                           const IWKV_val *val,
                           iwkv_opflags opflags,
                           bool internal) {
  assert(*idxp < KVBLK_IDXNUM);
  int32_t i;
  uint32_t len, nlen, sz;
  int8_t idx = *idxp;
  uint8_t *mm = 0, *wp, *sp;
  IWKV_val *uval = (IWKV_val *) val;
  IWKV_val *ukey = (IWKV_val *) key;
  IWKV_val sval, skey; // stack allocated key/val
  KVP *kvp = &kb->pidx[idx];
  IWDB db = kb->db;
  IWFS_FSM *fsm = &db->iwkv->fsm;
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  
  // DUP
  if (!internal && (db->dbflg & IWDB_DUP_FLAGS)) {
    if (((db->dbflg & IWDB_DUP_UINT32_VALS) && val->size != 4) ||
        ((db->dbflg & IWDB_DUP_UINT64_VALS) && val->size != 8)) {
      rc = IWKV_ERROR_DUP_VALUE_SIZE;
      goto finish;
    }
    _kvblk_peek_val(kb, idx, mm, &wp, &len);
    if (len < 4) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    sp = wp;
    memcpy(&sz, wp, 4); // number of elements
    sz = IW_ITOHL(sz);
    wp += 4;
    if (len < 4 + sz * val->size) {
      // kv capacity is less than reported number of items (sz)
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    uint8_t vbuf[8];
    uint32_t avail = len - (4 /* num items */ + sz * val->size);
    _num2lebuf(vbuf, val->data, val->size);
    
    if (opflags & IWKV_DUP_REMOVE) {
      if (!sz || !iwarr_sorted_remove(wp, sz, val->size, vbuf,  val->size > 4 ? _u8cmp : _u4cmp)) {
        rc = IWKV_ERROR_NOTFOUND;
        goto finish;
      }
      sz -= 1;
      sz = IW_HTOIL(sz);
      memcpy(sp, &sz, 4);
      if (len >= (4 + sz * val->size) * 2) {
        // Reduce size of kv value buffer
        kvp->len = kvp->len - len / 2;
        kb->flags |= KVBLK_DURTY;
      }
      goto finish;
      
    } else if (avail >= val->size) { // we have enough room to store the given number
      if (iwarr_sorted_insert(wp, sz, val->size, vbuf,
                              val->size > 4 ? _u8cmp : _u4cmp, true) == -1) {
        goto finish;
      }
      // Increment number of items
      sz += 1;
      sz = IW_HTOIL(sz);
      memcpy(sp, &sz, 4);
      goto finish;
    } else {
      // reallocate value buf
      uval = &sval;
      nlen = len;
      while (avail < val->size) {
        nlen *= 2;
        avail = nlen - (4 + sz * val->size);
      }
      uval->data = malloc(nlen);
      if (!uval->data) {
        rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
        goto finish;
      }
      // zero initialize extra bytes
      memset((uint8_t *)uval->data + len, 0, nlen - len);
      uval->size = nlen;
      memcpy(uval->data, sp, len);
      if (iwarr_sorted_insert((uint8_t *)uval->data + 4, sz, val->size, vbuf,
                              val->size > 4 ? _u8cmp : _u4cmp, true) == -1) {
        goto finish;
      }
      sz += 1;
      sz = IW_HTOIL(sz);
      memcpy(uval->data, &sz, 4);
    }
  }
  // !DUP
  
  wp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  sp = wp;
  IW_READVNUMBUF(wp, len, sz);
  wp += sz;
  if (ukey && (len != ukey->size || memcmp(wp, ukey->data, ukey->size))) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  wp += len;
  size_t rsize = IW_VNUMSIZE32(len) + len + uval->size; // required size
  if (rsize <= kvp->len) {
    memcpy(wp, uval->data, uval->size);
    wp += uval->size;
    if ((wp - sp) != kvp->len) {
      kvp->len = wp - sp;
      kb->flags |= KVBLK_DURTY;
    }
  } else {
    KVP tidx[KVBLK_IDXNUM];
    KVP tidx_tmp[KVBLK_IDXNUM];
    off_t koff = kb->pidx[idx].off;
    memcpy(tidx, kb->pidx, KVBLK_IDXNUM * sizeof(kb->pidx[0]));
    ks_mergesort_kvblk(KVBLK_IDXNUM, tidx, tidx_tmp);
    kb->flags |= KVBLK_DURTY;
    if (!ukey) { // we need a key
      ukey = &skey;
      rc = _kvblk_getkey(kb, mm, idx, ukey);
      RCGO(rc, finish);
    }
    for (i = 0; i < KVBLK_IDXNUM; ++i) {
      if (tidx[i].off == koff) {
        if (koff - (i > 0 ? tidx[i - 1].off : 0) >= rsize) {
          memcpy(wp, uval->data, uval->size);
          wp += uval->size;
          kvp->len = wp - sp;
        } else {
          // todo: optimize?
          mm = 0;
          fsm->release_mmap(fsm);
          rc = _kvblk_rmkv(kb, idx, RMKV_NO_RESIZE);
          RCGO(rc, finish);
          rc = _kvblk_addkv(kb, ukey, uval, idxp, opflags, true);
        }
        break;
      }
    }
  }
  
finish:
  if (uval != val) {
    _kv_val_dispose(uval);
  }
  if (ukey != key) {
    _kv_val_dispose(ukey);
  }
  if (mm) {
    IWRC(fsm->release_mmap(fsm), rc);
  }
  return rc;
}

//--------------------------  SBLK

IW_INLINE void _sblk_release(IWLCTX *lx, SBLK **sblkp) {
  assert(sblkp && *sblkp);
  (*sblkp)->kvblk = 0;
  *sblkp = 0;
}

IW_INLINE iwrc _sblk_loadkvblk_mm(IWLCTX *lx, SBLK *sblk, uint8_t *mm) {
  if (!sblk->kvblk && sblk->kvblkn) {
    return _kvblk_at_mm(lx, BLK2ADDR(sblk->kvblkn), mm, 0, &sblk->kvblk);
  } else {
    return 0;
  }
}

IW_INLINE iwrc _sblk_destroy(IWLCTX *lx, SBLK **sblkp) {
  assert(sblkp && *sblkp && (*sblkp)->addr);
  iwrc rc = 0;
  SBLK *sblk = *sblkp;
  if (!(sblk->flags & SBLK_DB)) {
    uint8_t kvb_szpow;
    IWFS_FSM *fsm = &lx->db->iwkv->fsm;
    off_t kvb_addr = BLK2ADDR(sblk->kvblkn),
          sblk_addr = sblk->addr;
    if (!sblk->kvblk) {
      // Read KVBLK size as power of two
      uint8_t *mm;
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCRET(rc);
      memcpy(&kvb_szpow, mm + kvb_addr + KBLK_SZPOW_OFF, 1);
      fsm->release_mmap(fsm);
    } else {
      kvb_szpow = sblk->kvblk->szpow;
    }
    if (lx->db->lcnt[sblk->lvl]) {
      lx->db->lcnt[sblk->lvl]--;
      lx->db->flags |= SBLK_DURTY;
    }
    _sblk_release(lx, sblkp);
    rc = fsm->deallocate(fsm, sblk_addr, SBLK_SZ);
    IWRC(fsm->deallocate(fsm, kvb_addr, 1ULL << kvb_szpow), rc);
  } else {
    _sblk_release(lx, sblkp);
  }
  return rc;
}

IW_INLINE uint8_t _sblk_genlevel(IWDB db) {
  int8_t lvl;
#ifdef IW_TESTS
  if (iwkv_next_level >= 0) {
    lvl = iwkv_next_level;
    iwkv_next_level = -1;
    assert(lvl >= 0 && lvl < SLEVELS);
    return lvl;
  } else if (iwkv_next_level == -2) {
    uint32_t r = random(); // up to 0x7fffffff (not 0xffffffff)
    for (lvl = 0; lvl < SLEVELS && !(r & 1); ++lvl) r >>= 1;
    return IW_UNLIKELY(lvl >= SLEVELS) ? SLEVELS - 1 : lvl;
  }
#endif
  uint8_t ret;
  uint32_t r = iwu_rand_u32();
  for (lvl = 0; lvl < SLEVELS && !(r & 1); ++lvl) r >>= 1;
  ret = IW_UNLIKELY(lvl >= SLEVELS) ? SLEVELS - 1 : lvl;
  while (ret > 0 && db->lcnt[ret - 1] == 0) {
    --ret;
  }
  return ret;
}

static iwrc _sblk_create(IWLCTX *lx, int8_t nlevel, int8_t kvbpow, off_t baddr, SBLK **oblk) {
  iwrc rc;
  SBLK *sblk;
  KVBLK *kvblk;
  off_t blen;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  if (kvbpow < KVBLK_INISZPOW) {
    kvbpow = KVBLK_INISZPOW;
  }
  off_t kvblksz = 1ULL << kvbpow;
  *oblk = 0;
  rc = fsm->allocate(fsm, SBLK_SZ + kvblksz, &baddr, &blen,
                     IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE | IWFSM_ALLOC_NO_STATS);
  RCRET(rc);
  
  assert(blen - SBLK_SZ == kvblksz);
  _kvblk_create(lx, baddr + SBLK_SZ, kvblksz, kvbpow, &kvblk);
  RCRET(rc);
  
  sblk = &lx->saa[lx->saan];
  sblk->db = lx->db;
  sblk->db->lcnt[nlevel]++;
  sblk->db->flags |= SBLK_DURTY;
  sblk->addr = baddr;
  sblk->flags = SBLK_DURTY;
  sblk->lvl = nlevel;
  sblk->p0 = 0;
  memset(sblk->n, 0, sizeof(sblk->n));
  sblk->kvblk = kvblk;
  sblk->kvblkn = ADDR2BLK(kvblk->addr);
  sblk->lkl = 0;
  sblk->pnum = 0;
  memset(sblk->pi, 0, sizeof(sblk->pi));
  *oblk = sblk;
  AAPOS_INC(lx->saan);
  return 0;
}

static iwrc _sblk_at(IWLCTX *lx, off_t addr, sblk_flags_t flgs, SBLK **sblkp) {
  uint8_t *mm;
  uint32_t lv;
  *sblkp = 0;
  sblk_flags_t flags = lx->sblk_flags | flgs;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  
  SBLK *sblk = &lx->saa[lx->saan];
  sblk->db = lx->db;
  sblk->kvblk = 0;
  
  if (IW_UNLIKELY(addr == lx->db->addr)) {
    uint8_t *rp = mm + addr + DOFF_N0_U4;
    // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n[30]:u4,c[30]:u4]:u257
    sblk->addr = addr;
    sblk->flags = SBLK_DB | flags;
    sblk->lvl = 0;
    sblk->p0 = 0;
    sblk->kvblkn = 0;
    sblk->lkl = 0;
    sblk->pnum = KVBLK_IDXNUM;
    memset(sblk->pi, 0, sizeof(sblk->pi));
    for (int i = 0; i < SLEVELS; ++i) {
      IW_READLV(rp, lv, sblk->n[i]);
      if (sblk->n[i]) {
        ++sblk->lvl;
      } else {
        break;
      }
    }
    if (sblk->lvl) --sblk->lvl;
  } else if (addr) {
    uint8_t *rp = mm + addr;
    sblk->addr = addr;
    // [u1:flags,lvl:u1,lkl:u1,pnum:u1,p0:u4,kblk:u4,[pi0:u1,... pi32],n0-n29:u4,pad:u28,lk:u64]:u256
    memcpy(&sblk->flags, rp, 1);
    if (sblk->flags & ~SBLK_PERSISTENT_FLAGS) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    sblk->flags |= flags;
    rp += 1;
    memcpy(&sblk->lvl, rp++, 1);
    if (sblk->lvl >= SLEVELS) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    memcpy(&sblk->lkl, rp++, 1);
    if (sblk->lkl > SBLK_LKLEN) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    memcpy(&sblk->pnum, rp++, 1);
    memcpy(&sblk->p0, rp, 4);
    sblk->p0 = IW_ITOHL(sblk->p0);
    rp += 4;
    memcpy(&sblk->kvblkn, rp, 4);
    sblk->kvblkn = IW_ITOHL(sblk->kvblkn);
    rp += 4;
    memcpy(sblk->pi, rp, KVBLK_IDXNUM);    
    rp += KVBLK_IDXNUM;
    for (int i = 0; i <= sblk->lvl; ++i) {
      memcpy(&sblk->n[i], rp, 4);
      sblk->n[i] = IW_ITOHL(sblk->n[i]);
      rp += 4;
    }
    rp = mm + addr + SOFF_LK;
    memcpy(sblk->lk, rp, sblk->lkl);    
  } else { // Database tail
    uint8_t *rp = mm + lx->db->addr + DOFF_P0_U4;
    sblk->addr = 0;
    sblk->flags = SBLK_DB | flags;
    sblk->lvl = 0;
    sblk->kvblkn = 0;
    sblk->lkl = 0;
    sblk->pnum = KVBLK_IDXNUM;
    memset(sblk->pi, 0, sizeof(sblk->pi));
    IW_READLV(rp, lv, sblk->p0);
    if (!sblk->p0) {
      sblk->p0 = ADDR2BLK(lx->db->addr);
    }
  }
finish:
  fsm->release_mmap(fsm);
  AAPOS_INC(lx->saan);
  *sblkp = sblk;
  return rc;
}

static void _sblk_sync_mm(IWLCTX *lx, SBLK *sblk, uint8_t *mm) {
  if (sblk->flags & SBLK_DURTY) {
    sblk->flags &= ~SBLK_DURTY;
    uint32_t lv;
    if (IW_UNLIKELY(sblk->flags & SBLK_DB)) {
      uint8_t *wp = mm + sblk->db->addr;
      if (sblk->addr) {
        assert(sblk->addr == sblk->db->addr);
        wp += DOFF_N0_U4;
        // [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n[30]:u4,c[30]:u4]:u257
        for (int i = 0; i < SLEVELS; ++i) {
          IW_WRITELV(wp, lv, sblk->n[i]);
        }
        assert(wp - (mm + sblk->db->addr) <= SBLK_SZ);
        for (int i = 0; i < SLEVELS; ++i) {
          IW_WRITELV(wp, lv, lx->db->lcnt[i]);
        }
      } else { // Database tail
        wp += DOFF_P0_U4;
        IW_WRITELV(wp, lv, sblk->p0);
        assert(wp - (mm + sblk->db->addr) <= SBLK_SZ);
      }
      return;
    } else {
      uint8_t *wp = mm + sblk->addr;
      sblk_flags_t flags = (sblk->flags & SBLK_PERSISTENT_FLAGS);
      // [u1:flags,lvl:u1,lkl:u1,pnum:u1,p0:u4,kblk:u4,[pi0:u1,... pi32],n0-n29:u4,pad:u28,lk:u64]:u256
      wp += SOFF_FLAGS_U1;
      memcpy(wp, &flags, 1);
      wp += 1;
      memcpy(wp, &sblk->lvl, 1);
      wp += 1;
      assert(sblk->lkl <= SBLK_LKLEN);
      memcpy(wp, &sblk->lkl, 1);
      wp += 1;
      memcpy(wp, &sblk->pnum, 1);
      wp += 1;
      IW_WRITELV(wp, lv, sblk->p0);
      IW_WRITELV(wp, lv, sblk->kvblkn);
      memcpy(wp, sblk->pi, KVBLK_IDXNUM);
      wp = mm + sblk->addr + SOFF_N0_U4;
      for (int i = 0; i <= sblk->lvl; ++i) {
        IW_WRITELV(wp, lv, sblk->n[i]);
      }
      wp = mm + sblk->addr + SOFF_LK;
      memcpy(wp, sblk->lk, sblk->lkl);      
    }
  }
  if (sblk->kvblk && (sblk->kvblk->flags & KVBLK_DURTY)) {
    _kvblk_sync_mm(sblk->kvblk, mm);
  }
}

IW_INLINE void _sblk_sync_and_release_mm(IWLCTX *lx, SBLK **sblkp, uint8_t *mm) {
  if (mm) {
    _sblk_sync_mm(lx, *sblkp, mm);
  }
  _sblk_release(lx, sblkp);
}

IW_INLINE iwrc _sblk_sync_and_release(IWLCTX *lx, SBLK **sblkp) {
  assert(sblkp && *sblkp);
  SBLK *sblk = *sblkp;
  if ((sblk->flags & SBLK_DURTY) || (sblk->kvblk && (sblk->kvblk->flags & KVBLK_DURTY))) {
    uint8_t *mm;
    IWFS_FSM *fsm = &lx->db->iwkv->fsm;
    iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCRET(rc);
    _sblk_sync_mm(lx, *sblkp, mm);
    _sblk_release(lx, sblkp);
    return fsm->release_mmap(fsm);
  } else {
    _sblk_release(lx, sblkp);
    return 0;
  }
}

static iwrc _sblk_find_pi_mm(SBLK *sblk, const IWKV_val *key, const uint8_t *mm, bool *found, uint8_t *idxp) {
  *found = false;
  if (sblk->flags & SBLK_DB) {
    *idxp = KVBLK_IDXNUM;
    return 0;
  }
  uint8_t *k;
  uint32_t kl;
  iwdb_flags_t dbflg = sblk->db->dbflg;
  int idx = 0,
      lb = 0,
      ub = sblk->pnum - 1;
  if (sblk->pnum < 1) {
    *idxp = 0;
    return 0;
  }
  while (1) {
    idx = (ub + lb) / 2;
    iwrc rc = _kvblk_peek_key(sblk->kvblk, sblk->pi[idx], mm, &k, &kl);
    RCRET(rc);
    int cr = _cmp_key(dbflg, k, kl, key->data, key->size);
    if (!cr) {
      *found = true;
      break;
    } else if (cr < 0) {
      lb = idx + 1;
      if (lb > ub) {
        idx = lb;
        break;
      }
    } else {
      ub = idx - 1;
      if (lb > ub) {
        break;
      }
    }
  }
  *idxp = idx;
  return 0;
}

static iwrc _sblk_insert_pi_mm(SBLK *sblk, uint8_t nidx, const IWKV_val *key,
                               const uint8_t *mm, uint8_t *idxp) {
  assert(sblk->kvblk);
  uint8_t *k;
  uint32_t kl;
  iwdb_flags_t dbflg = sblk->db->dbflg;
  int idx = 0,
      lb = 0,
      ub = sblk->pnum - 1,
      nels = sblk->pnum;
  if (nels < 1) {
    sblk->pi[0] = nidx;
    ++sblk->pnum;
    *idxp = 0;
    return 0;
  }
  while (1) {
    idx = (ub + lb) / 2;
    iwrc rc = _kvblk_peek_key(sblk->kvblk, sblk->pi[idx], mm, &k, &kl);
    RCRET(rc);
    int cr = _cmp_key(dbflg, k, kl, key->data, key->size);
    if (!cr) {
      break;
    } else if (cr < 0) {
      lb = idx + 1;
      if (lb > ub) {
        idx = lb;
        ++sblk->pnum;
        break;
      }
    } else {
      ub = idx - 1;
      if (lb > ub) {
        ++sblk->pnum;
        break;
      }
    }
  }
  if (nels - idx > 0) {
    memmove(sblk->pi + idx + 1, sblk->pi + idx, nels - idx);
  }
  sblk->pi[idx] = nidx;
  *idxp = idx;
  return 0;
}

IW_INLINE iwrc _sblk_addkv2(SBLK *sblk, int8_t idx, const IWKV_val *key, const IWKV_val *val, iwkv_opflags opflags,
                            bool internal) {
  assert(sblk && key && key->size && key->data &&
         val && idx >= 0 && sblk->kvblk);
  int8_t kvidx;
  KVBLK *kvblk = sblk->kvblk;
  if (sblk->pnum >= KVBLK_IDXNUM) {    
    return _IWKV_ERROR_KVBLOCK_FULL;
  }
  if (!internal && (opflags & IWKV_DUP_REMOVE)) {
    return IWKV_ERROR_NOTFOUND;
  }
  iwrc rc = _kvblk_addkv(kvblk, key, val, &kvidx, opflags, internal);
  RCRET(rc);
  if (sblk->pnum - idx > 0) {
    memmove(sblk->pi + idx + 1, sblk->pi + idx, sblk->pnum - idx);
  }
  sblk->pi[idx] = kvidx;
  sblk->kvblkn = ADDR2BLK(kvblk->addr);
  ++sblk->pnum;
  sblk->flags |= SBLK_DURTY;
  if (idx == 0) { // the lowest key inserted
    sblk->lkl = MIN(SBLK_LKLEN, key->size);
    memcpy(sblk->lk, key->data, sblk->lkl);
    if (key->size <= SBLK_LKLEN) {
      sblk->flags |= SBLK_FULL_LKEY;
    } else {
      sblk->flags &= ~SBLK_FULL_LKEY;
    }
  }
  return 0;
}

static iwrc _sblk_addkv(SBLK *sblk,
                        const IWKV_val *key,
                        const IWKV_val *val,
                        iwkv_opflags opflags,
                        bool internal) {
  assert(sblk && key && key->size && key->data && val && sblk->kvblk);
  int8_t kvidx;
  uint8_t *mm, idx;
  KVBLK *kvblk = sblk->kvblk;
  IWFS_FSM *fsm = &sblk->db->iwkv->fsm;
  if (sblk->pnum >= KVBLK_IDXNUM) {    
    return _IWKV_ERROR_KVBLOCK_FULL;
  }
  if (!internal && (opflags & IWKV_DUP_REMOVE)) {
    return IWKV_ERROR_NOTFOUND;
  }
  iwrc rc = _kvblk_addkv(kvblk, key, val, &kvidx, opflags, internal);
  RCRET(rc);
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _sblk_insert_pi_mm(sblk, kvidx, key, mm, &idx);
  RCRET(rc);
  if (idx == 0) { // the lowest key inserted
    sblk->lkl = MIN(SBLK_LKLEN, key->size);
    memcpy(sblk->lk, key->data, sblk->lkl);
    if (key->size <= SBLK_LKLEN) {
      sblk->flags |= SBLK_FULL_LKEY;
    } else {
      sblk->flags &= ~SBLK_FULL_LKEY;
    }
  }
  fsm->release_mmap(fsm);
  sblk->kvblkn = ADDR2BLK(kvblk->addr);
  sblk->flags |= SBLK_DURTY;
  return 0;
}

IW_INLINE iwrc _sblk_updatekv(SBLK *sblk, int8_t idx, const IWKV_val *key, const IWKV_val *val, iwkv_opflags opflags) {
  assert(sblk && sblk->kvblk && idx >= 0 && idx < sblk->pnum);
  KVBLK *kvblk = sblk->kvblk;
  int8_t kvidx = sblk->pi[idx];
  iwrc rc = _kvblk_updatev(kvblk, &kvidx, key, val, opflags, false);
  RCRET(rc);
  sblk->kvblkn = ADDR2BLK(kvblk->addr);
  sblk->pi[idx] = kvidx;
  sblk->flags |= SBLK_DURTY;
  return 0;
}

IW_INLINE iwrc _sblk_rmkv(SBLK *sblk, uint8_t idx) {
  assert(sblk && sblk->kvblk);
  KVBLK *kvblk = sblk->kvblk;
  IWFS_FSM *fsm = &sblk->db->iwkv->fsm;
  assert(kvblk && idx < sblk->pnum && sblk->pi[idx] < KVBLK_IDXNUM);
  iwrc rc = _kvblk_rmkv(kvblk, sblk->pi[idx], 0);
  RCRET(rc);
  sblk->kvblkn = ADDR2BLK(kvblk->addr);
  --sblk->pnum;
  sblk->flags |= SBLK_DURTY;
  if (idx < sblk->pnum && sblk->pnum > 0) {
    memmove(sblk->pi + idx, sblk->pi + idx + 1, sblk->pnum - idx);
  }
  if (idx == 0) { // Lowest key removed
    uint8_t *mm;
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCRET(rc);
    // Replace key with the next one or reset
    if (sblk->pnum > 0) {
      uint8_t *kbuf;
      uint32_t klen;
      rc = _kvblk_peek_key(sblk->kvblk, sblk->pi[idx], mm, &kbuf, &klen);
      RCRET(rc);
      sblk->lkl = MIN(SBLK_LKLEN, klen);
      memcpy(sblk->lk, kbuf, sblk->lkl);
      if (klen <= SBLK_LKLEN) {
        sblk->flags |= SBLK_FULL_LKEY;
      } else {
        sblk->flags &= ~SBLK_FULL_LKEY;
      }
    } else {
      sblk->lkl = 0;
    }
    fsm->release_mmap(fsm);
  }
  return rc;
}

//--------------------------  IWLCTX

IW_INLINE iwrc _lx_sblk_cmp_key(IWLCTX *lx, SBLK *sblk, int *res) {
  iwdb_flags_t dbflg = sblk->db->dbflg;
  const IWKV_val *key = lx->key;
  if (IW_UNLIKELY(sblk->pnum < 1 || (sblk->flags & SBLK_DB))) { // empty block
    *res = 0;
    return IWKV_ERROR_CORRUPTED;
  } else if ((sblk->flags & SBLK_FULL_LKEY) || key->size < sblk->lkl) {
    *res = _cmp_key(dbflg, sblk->lk, sblk->lkl, key->data, key->size);
  } else {
    uint32_t kl;
    uint8_t *k, *mm;
    IWFS_FSM *fsm = &lx->db->iwkv->fsm;
    iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    if (rc) {
      *res = 0;
      fsm->release_mmap(fsm);
      return rc;
    }
    if (!sblk->kvblk) {
      rc = _sblk_loadkvblk_mm(lx, sblk, mm);
      if (rc) {
        *res = 0;
        fsm->release_mmap(fsm);
        return rc;
      }
    }
    rc = _kvblk_peek_key(sblk->kvblk, sblk->pi[0], mm, &k, &kl);
    RCRET(rc);
    *res = _cmp_key(dbflg, k, kl, key->data, key->size);
    fsm->release_mmap(fsm);
  }
  return 0;
}

static iwrc _lx_roll_forward(IWLCTX *lx, uint8_t lvl) {
  iwrc rc = 0;
  int cret;
  SBLK *sblk;
  blkn_t blkn;
  assert(lx->lower);
  
  while ((blkn = lx->lower->n[lvl])) {
    off_t blkaddr = BLK2ADDR(blkn);
    if (lx->nlvl > -1 && lvl < lx->nlvl) {
      int8_t ulvl = lvl + 1;
      if (lx->pupper[ulvl] && lx->pupper[ulvl]->addr == blkaddr) {
        sblk = lx->pupper[ulvl];
      } else if (lx->plower[ulvl] && lx->plower[ulvl]->addr == blkaddr) {
        sblk = lx->plower[ulvl];
      } else {
        rc = _sblk_at(lx, blkaddr, SBLK_LKEY_ONLY, &sblk);
      }
    } else {
      rc = _sblk_at(lx, blkaddr, SBLK_LKEY_ONLY, &sblk);
    }
    RCRET(rc);
#ifndef NDEBUG
    ++lx->num_cmps;
#endif
    rc = _lx_sblk_cmp_key(lx, sblk, &cret);
    RCRET(rc);
    if (cret > 0 || lx->upper_addr == sblk->addr) { // upper > key
      lx->upper = sblk;
      break;
    } else {
      lx->lower = sblk;
    }
  }
  
  // load full sblk
  
  return 0;
}

static iwrc _lx_find_bounds(IWLCTX *lx) {
  iwrc rc = 0;
  int lvl;
  blkn_t blkn;
  if (!lx->dblk) {
    rc = _sblk_at(lx, lx->db->addr, 0, &lx->dblk);
    RCRET(rc);
  }
  if (!lx->lower) {
    lx->lower = lx->dblk;
  }
  if (lx->nlvl > lx->dblk->lvl) { // New level in DB
    lx->dblk->lvl = lx->nlvl;
    lx->dblk->flags |= SBLK_DURTY;
  }
  lvl = lx->lower->lvl;
  while (lvl > -1) {
    rc = _lx_roll_forward(lx, lvl);
    RCRET(rc);
    if (lx->upper) {
      blkn = ADDR2BLK(lx->upper->addr);
    } else {
      blkn = 0;
    }
    do {
      if (lx->nlvl >= lvl) {
        lx->plower[lvl] = lx->lower;
        lx->pupper[lvl] = lx->upper;
      }
    } while (lvl-- && lx->lower->n[lvl] == blkn);
  }
  return 0;
}

static void _lx_release_mm(IWLCTX *lx, uint8_t *mm) {
  if (lx->nlvl > -1) {
    SBLK *lsb = 0, *usb = 0;
    if (lx->nb) {
      _sblk_sync_and_release_mm(lx, &lx->nb, mm);
    }
    if (lx->pupper[0] == lx->upper) {
      lx->upper = 0;
    }
    if (lx->plower[0] == lx->lower) {
      lx->lower = 0;
    }
    for (int i = 0; i <= lx->nlvl; ++i) {
      if (lx->pupper[i]) {
        if (lx->pupper[i] != usb) {
          usb = lx->pupper[i];
          _sblk_sync_and_release_mm(lx, &lx->pupper[i], mm);
        }
        lx->pupper[i] = 0;
      }
      if (lx->plower[i]) {
        if (lx->plower[i] != lsb) {
          lsb = lx->plower[i];
          _sblk_sync_and_release_mm(lx, &lx->plower[i], mm);
        }
        lx->plower[i] = 0;
      }
    }
  }
  if (lx->upper) {
    _sblk_sync_and_release_mm(lx, &lx->upper, mm);
  }
  if (lx->lower) {
    _sblk_sync_and_release_mm(lx, &lx->lower, mm);
  }
  if (lx->dblk && (lx->dblk->flags & SBLK_DURTY)) {
    _sblk_sync_mm(lx, lx->dblk, mm);
  }
}

iwrc _lx_release(IWLCTX *lx) {
  uint8_t *mm;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  _lx_release_mm(lx, mm);
  return fsm->release_mmap(fsm);
}

static iwrc _lx_split_addkv(IWLCTX *lx, int idx, SBLK *sblk) {
  iwrc rc;
  SBLK *nb;
  blkn_t nblk;
  uint8_t kvbpow = 0;
  register int pivot = (KVBLK_IDXNUM / 2) + 1; // 32
  
  assert(idx > 0);
  if (idx < sblk->pnum) {
    assert(sblk->kvblk);
    // Partial split required
    // Compute space required for the new sblk which stores kv pairs after pivot `idx`
    size_t sz = 0;
    for (int i = pivot; i < sblk->pnum; ++i) {
      sz += sblk->kvblk->pidx[sblk->pi[i]].len;
    }
    if (idx > pivot) {
      sz += IW_VNUMSIZE(lx->key->size) + lx->key->size + lx->val->size;
    }
    sz += KVBLK_MAX_NKV_SZ;
    kvbpow = iwlog2_64(sz);
    while ((1ULL << kvbpow) < sz) {
      kvbpow++;
    }
  }
  rc = _sblk_create(lx, lx->nlvl, kvbpow, sblk->addr, &nb);
  RCRET(rc);
  nblk = ADDR2BLK(nb->addr);
  if (idx == sblk->pnum) { // Upper side
    rc = _sblk_addkv(nb, lx->key, lx->val, lx->opflags, false);
    RCGO(rc, finish);
  } else { // New key is somewere in a middle of sblk->kvblk
    assert(sblk->kvblk);
    // We are in the middle
    // Do the partial split
    // Move kv pairs into new `nb`
    IWKV_val key, val;
    IWFS_FSM *fsm = &lx->db->iwkv->fsm;
    for (int i = pivot, end = sblk->pnum; i < end; ++i) {
      uint8_t *mm;
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCBREAK(rc);
      rc = _kvblk_getkv(sblk->kvblk, mm, sblk->pi[i], &key, &val);
      assert(key.size);
      fsm->release_mmap(fsm);
      RCBREAK(rc);
      rc = _sblk_addkv2(nb, i - pivot, &key, &val, lx->opflags, true);
      _kv_dispose(&key, &val);
      RCBREAK(rc);
      sblk->kvblk->pidx[sblk->pi[i]].len = 0;
      sblk->kvblk->pidx[sblk->pi[i]].off = 0;
      --sblk->pnum;
    }
    sblk->kvblk->flags |= KVBLK_DURTY;
    sblk->kvblk->zidx = sblk->pi[pivot];
    sblk->kvblk->maxoff = 0;
    for (int i = 0; i < KVBLK_IDXNUM; ++i) {
      if (sblk->kvblk->pidx[i].off > sblk->kvblk->maxoff) {
        sblk->kvblk->maxoff = sblk->kvblk->pidx[i].off;
      }
    }
    if (idx > pivot) {
      rc = _sblk_addkv(nb, lx->key, lx->val, lx->opflags, false);
    } else {
      rc = _sblk_addkv(sblk, lx->key, lx->val, lx->opflags, false);
    }
    RCGO(rc, finish);
  }
  // fix levels
  //  [ lb -> sblk -> ub ]
  //  [ lb -> sblk -> nb -> ub ]
  lx->pupper[0]->p0 = nblk;
  lx->pupper[0]->flags |= SBLK_DURTY;
  nb->p0 = ADDR2BLK(lx->plower[0]->addr);
  for (int i = 0; i <= nb->lvl; ++i) {
    lx->plower[i]->n[i] = nblk;
    lx->plower[i]->flags |= SBLK_DURTY;
    nb->n[i] = ADDR2BLK(lx->pupper[i]->addr);
  }
finish:
  if (rc) {
    lx->nb = 0;
    IWRC(_sblk_destroy(lx, &nb), rc);
  } else {
    lx->nb = nb;
  }
  return rc;
}

IW_INLINE iwrc _lx_init_chute(IWLCTX *lx) {
  assert(lx->nlvl >= 0);
  iwrc rc = 0;
  if (!lx->pupper[lx->nlvl]) { // fix zero upper by dbtail
    SBLK *dbtail;
    rc = _sblk_at(lx, 0, 0, &dbtail);
    RCRET(rc);
    for (int i = lx->nlvl; i >= 0 && !lx->pupper[i]; --i) {
      lx->pupper[i] = dbtail;
    }
  }
  return 0;
}

WUR iwrc _lx_addkv(IWLCTX *lx) {
  iwrc rc;
  bool found, uadd;
  uint8_t *mm = 0, idx;
  SBLK *sblk = lx->lower;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  if (lx->nlvl > -1) {
    rc = _lx_init_chute(lx);
    RCRET(rc);
  }
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _sblk_loadkvblk_mm(lx, sblk, mm);
  if (rc) {
    fsm->release_mmap(fsm);
    return rc;
  }
  
  rc = _sblk_find_pi_mm(sblk, lx->key, mm, &found, &idx);
  RCRET(rc);
  if (found && (lx->opflags & IWKV_NO_OVERWRITE)) {
    fsm->release_mmap(fsm);
    return IWKV_ERROR_KEY_EXISTS;
  }
  
  uadd = (!found &&
          sblk->pnum > KVBLK_IDXNUM - 1 && idx > KVBLK_IDXNUM - 1 &&
          lx->upper && lx->upper->pnum < KVBLK_IDXNUM);
  if (uadd) {
    rc = _sblk_loadkvblk_mm(lx, lx->upper, mm);
    if (rc) {
      fsm->release_mmap(fsm);
      return rc;
    }
  }
  fsm->release_mmap(fsm);
  if (!found && sblk->pnum > KVBLK_IDXNUM - 1) {
    if (uadd) {
      return _sblk_addkv(lx->upper, lx->key, lx->val, lx->opflags, false);
    }
    if (lx->nlvl < 0) {
      return _IWKV_ERROR_REQUIRE_NLEVEL;
    }
  }
  if (!found && sblk->pnum >= KVBLK_IDXNUM) {
    return _lx_split_addkv(lx, idx, sblk);
  } else {
    if (!found) {
      return _sblk_addkv2(sblk, idx, lx->key, lx->val, lx->opflags, false);
    } else {
      return _sblk_updatekv(sblk, idx, lx->key, lx->val, lx->opflags);
    }
  }
}

IW_INLINE iwrc _lx_put_lw(IWLCTX *lx) {
  iwrc rc;
start:
  rc = _lx_find_bounds(lx);
  if (rc) {
    _lx_release_mm(lx, 0);
    return rc;
  }
  rc = _lx_addkv(lx);
  if (rc == _IWKV_ERROR_REQUIRE_NLEVEL) {
    SBLK *lower = lx->lower;
    _lx_release_mm(lx, 0);
    lx->nlvl = _sblk_genlevel(lx->db);
    if (lower->lvl >= lx->nlvl) {
      lx->lower = lower;
    }
    goto start;
  }
  if (rc) {
    if (rc == _IWKV_ERROR_KVBLOCK_FULL) {
      rc = IWKV_ERROR_CORRUPTED;
      iwlog_ecode_error3(rc);
    }
    _lx_release_mm(lx, 0);
  } else {
    rc = _lx_release(lx);
  }
  return rc;
}

IW_INLINE iwrc _lx_get_lr(IWLCTX *lx) {
  iwrc rc = _lx_find_bounds(lx);
  RCRET(rc);
  bool found;
  uint8_t *mm, idx;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  lx->val->size = 0;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _sblk_loadkvblk_mm(lx, lx->lower, mm);
  RCGO(rc, finish);
  rc = _sblk_find_pi_mm(lx->lower, lx->key, mm, &found, &idx);
  RCGO(rc, finish);
  if (found) {
    idx = lx->lower->pi[idx];
    rc = _kvblk_getvalue(lx->lower->kvblk, mm, idx, lx->val);
  } else {
    rc = IWKV_ERROR_NOTFOUND;
  }
finish:
  IWRC(fsm->release_mmap(fsm), rc);
  _lx_release_mm(lx, 0);
  return rc;
}

IW_INLINE iwrc _lx_del_lw(IWLCTX *lx) {
  iwrc rc;
  bool found;
  uint8_t *mm = 0, idx;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  SBLK *sblk;
  rc = _lx_find_bounds(lx);
  RCRET(rc);
  sblk = lx->lower;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  rc = _sblk_loadkvblk_mm(lx, sblk, mm);
  RCGO(rc, finish);
  rc = _sblk_find_pi_mm(sblk, lx->key, mm, &found, &idx);
  RCGO(rc, finish);
  if (!found) {
    rc = IWKV_ERROR_NOTFOUND;
    goto finish;
  }
  fsm->release_mmap(fsm);
  mm = 0;
  if (sblk->pnum == 1) { // last kv in block
    KVBLK *kvblk = sblk->kvblk;
    _lx_release_mm(lx, 0);
    lx->nlvl = sblk->lvl;
    lx->upper_addr = sblk->addr;
    rc = _lx_find_bounds(lx);
    RCRET(rc);
    assert(lx->upper->pnum == 1 && lx->upper->addr == lx->upper_addr);
    lx->upper->kvblk = kvblk;
    rc = _sblk_rmkv(lx->upper, idx);
    RCGO(rc, finish);
    for (int i = 0; i <= lx->nlvl; ++i) {
      lx->plower[i]->n[i] = lx->upper->n[i];
      lx->plower[i]->flags |= SBLK_DURTY;
      if (lx->plower[i]->flags & SBLK_DB) {
        if (!lx->plower[i]->n[i]) {
          --lx->plower[i]->lvl;
        }
      }
      if (lx->pupper[i] == lx->upper) {
        lx->pupper[i] = 0;
      }
    }
    SBLK *nb;
    assert(!lx->nb);
    rc = _sblk_at(lx, BLK2ADDR(lx->upper->n[0]), 0, &nb);
    RCGO(rc, finish);
    lx->nb = nb;
    lx->nb->p0 = lx->upper->p0;
    lx->nb->flags |= SBLK_DURTY;
    rc = _sblk_destroy(lx, &lx->upper);
  } else {
    rc = _sblk_rmkv(sblk, idx);
    RCGO(rc, finish);
  }
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  if (rc) {
    _lx_release_mm(lx, 0);
  } else {
    rc = _lx_release(lx);
  }
  return rc;
}

//--------------------------  CURSOR

IW_INLINE iwrc _cursor_get_ge_idx(IWLCTX *lx, IWKV_cursor_op op, uint8_t *oidx) {
  iwrc rc = _lx_find_bounds(lx);
  RCRET(rc);
  bool found;
  uint8_t *mm, idx;
  IWFS_FSM *fsm = &lx->db->iwkv->fsm;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _sblk_loadkvblk_mm(lx, lx->lower, mm);
  RCGO(rc, finish);
  rc = _sblk_find_pi_mm(lx->lower, lx->key, mm, &found, &idx);
  RCGO(rc, finish);
  if (found) {
    *oidx = idx;
  } else {
    if (op == IWKV_CURSOR_EQ || (lx->lower->flags & SBLK_DB) || lx->lower->pnum < 1) {
      rc = IWKV_ERROR_NOTFOUND;
    } else {
      *oidx = idx ? idx - 1 : idx;
    }
  }
finish:
  IWRC(fsm->release_mmap(fsm), rc);
  return rc;
}

IW_INLINE iwrc _cursor_to_lr(IWKV_cursor cur, IWKV_cursor_op op) {
  iwrc rc = 0;
  IWDB db = cur->lx.db;
  IWLCTX *lx = &cur->lx;
  blkn_t dblk = ADDR2BLK(db->addr);
  if (op < IWKV_CURSOR_NEXT) { // IWKV_CURSOR_BEFORE_FIRST | IWKV_CURSOR_AFTER_LAST
    if (cur->cn) {
      rc = _sblk_sync_and_release(lx, &cur->cn);
      RCRET(rc);
    }
    if (op == IWKV_CURSOR_BEFORE_FIRST) {
      cur->dbaddr = db->addr;
      cur->cnpos = KVBLK_IDXNUM - 1;
    } else {
      cur->dbaddr = -1; // Negative as sign of dbtail
      cur->cnpos = 0;
    }
    return 0;
  }
start:
  if (op < IWKV_CURSOR_EQ) { // IWKV_CURSOR_NEXT | IWKV_CURSOR_PREV
    blkn_t n = 0;
    if (!cur->cn) {
      if (cur->dbaddr) {
        rc = _sblk_at(lx, (cur->dbaddr < 0 ? 0 : cur->dbaddr), 0, &cur->cn);
        cur->dbaddr = 0;
        RCGO(rc, finish);
      } else {
        rc = IW_ERROR_INVALID_STATE;
        goto finish;
      }
    }
    if (op == IWKV_CURSOR_NEXT) {
      if (cur->cnpos + 1 >= cur->cn->pnum) {
        n = cur->cn->n[0];
        if (!n) {
          rc = IWKV_ERROR_NOTFOUND;
          goto finish;
        }
        _sblk_sync_and_release(lx, &cur->cn);
        rc = _sblk_at(lx, BLK2ADDR(n), 0, &cur->cn);
        RCGO(rc, finish);
        cur->cnpos = 0;
        if (IW_UNLIKELY(!cur->cn->pnum)) {
          goto start;
        }
      } else {
        if (cur->cn->flags & SBLK_DB) {
          rc = IW_ERROR_INVALID_STATE;
          goto finish;
        }
        ++cur->cnpos;
      }
    } else { // IWKV_CURSOR_PREV
      if (cur->cnpos == 0) {
        n = cur->cn->p0;
        if (!n || n == dblk) {
          rc = IWKV_ERROR_NOTFOUND;
          goto finish;
        }
        _sblk_sync_and_release(lx, &cur->cn);
        rc = _sblk_at(lx, BLK2ADDR(n), 0, &cur->cn);
        RCGO(rc, finish);
        if (IW_LIKELY(cur->cn->pnum)) {
          cur->cnpos = cur->cn->pnum - 1;
        } else {
          goto start;
        }
      } else {
        if (cur->cn->flags & SBLK_DB) {
          rc = IW_ERROR_INVALID_STATE;
          goto finish;
        }
        --cur->cnpos;
      }
    }
  } else { // IWKV_CURSOR_EQ | IWKV_CURSOR_GE
    if (!lx->key) {
      rc = IW_ERROR_INVALID_STATE;
      goto finish;
    }
    rc = _cursor_get_ge_idx(lx, op, &cur->cnpos);
    if (lx->upper) {
      _sblk_release(lx, &lx->upper);
    }
    if (!rc) {
      cur->cn = lx->lower;
      lx->lower = 0;
    }
  }
finish:
  if (rc && rc != IWKV_ERROR_NOTFOUND && cur->cn) {
    _sblk_release(lx, &cur->cn);
  }
  return rc;
}

//--------------------------  PUBLIC API

static const char *_kv_ecodefn(locale_t locale, uint32_t ecode) {
  if (!(ecode > _IWKV_ERROR_START && ecode < _IWKV_ERROR_END)) {
    return 0;
  }
  switch (ecode) {
    case IWKV_ERROR_NOTFOUND:
      return "Key not found. (IWKV_ERROR_NOTFOUND)";
    case IWKV_ERROR_KEY_EXISTS:
      return "Key exists. (IWKV_ERROR_KEY_EXISTS)";
    case IWKV_ERROR_MAXKVSZ:
      return "Size of Key+value must be not greater than 0xfffffff bytes (IWKV_ERROR_MAXKVSZ)";
    case IWKV_ERROR_CORRUPTED:
      return "Database file invalid or corrupted (IWKV_ERROR_CORRUPTED)";
    case IWKV_ERROR_DUP_VALUE_SIZE:
      return "Value size is not compatible for insertion into sorted values array (IWKV_ERROR_DUP_VALUE_SIZE)";
    case IWKV_ERROR_KEY_NUM_VALUE_SIZE:
      return "Given key is not compatible to store as number (IWKV_ERROR_KEY_NUM_VALUE_SIZE)";
    case IWKV_ERROR_INCOMPATIBLE_DB_MODE:
      return "Incompatible database open mode (IWKV_ERROR_INCOMPATIBLE_DB_MODE)";
  }
  return 0;
}

iwrc iwkv_init(void) {
  static int _kv_initialized = 0;
  if (!__sync_bool_compare_and_swap(&_kv_initialized, 0, 1)) {
    return 0;
  }
  return iwlog_register_ecodefn(_kv_ecodefn);
}

iwrc iwkv_open(const IWKV_OPTS *opts, IWKV *iwkvp) {
  assert(iwkvp && opts);
  int rci;
  iwrc rc = 0;
  uint32_t lv;
  uint64_t llv;
  uint8_t *rp, *mm;
  rc = iw_init();
  RCRET(rc);
  if (opts->random_seed) {
    iwu_rand_seed(opts->random_seed);
  }
  *iwkvp = calloc(1, sizeof(struct IWKV));
  if (!*iwkvp) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  IWKV iwkv = *iwkvp;
  rci = pthread_rwlock_init(&iwkv->rwl, 0);
  if (rci) {
    free(*iwkvp);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  rci = pthread_mutex_init(&iwkv->wk_mtx, 0);
  if (rci) {
    pthread_rwlock_destroy(&iwkv->rwl);
    free(*iwkvp);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  rci = pthread_cond_init(&iwkv->wk_cond, 0);
  if (rci) {
    pthread_rwlock_destroy(&iwkv->rwl);
    pthread_mutex_destroy(&iwkv->wk_mtx);
    free(*iwkvp);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  iwkv_openflags oflags = opts->oflags;
  iwfs_omode omode = IWFS_OREAD;
  if (oflags & IWKV_TRUNC) {
    oflags &= ~IWKV_RDONLY;
    omode |= IWFS_OTRUNC;
  }
  if (!(oflags & IWKV_RDONLY)) {
    omode |= IWFS_OWRITE;
  }
  iwkv->oflags = oflags;
  IWFS_FSM_STATE fsmstate;
  IWFS_FSM_OPTS fsmopts = {
    .exfile = {
      .file = {
        .path       = opts->path,
        .omode      = omode,
        .lock_mode  = (oflags & IWKV_RDONLY) ? IWP_RLOCK : IWP_WLOCK
      },
      .rspolicy     = iw_exfile_szpolicy_fibo,
      .maxoff       = IWKV_MAX_DBSZ
    },
    .bpow = IWKV_FSM_BPOW,      // 64 bytes block size
    .hdrlen = KVHDRSZ,          // Size of custom file header
    .oflags = ((oflags & (IWKV_NOLOCKS | IWKV_RDONLY)) ? IWFSM_NOLOCKS : 0),
    .mmap_all = true
  };
#if defined(IW_TESTS) && !defined(IW_RELEASE)
  fsmopts.oflags |= IWFSM_STRICT;
#endif
  
  rc = iwfs_fsmfile_open(&iwkv->fsm, &fsmopts);
  RCGO(rc, finish);
  IWFS_FSM *fsm  = &iwkv->fsm;
  iwkv->dbs = kh_init(DBS);
  rc = fsm->state(fsm, &fsmstate);
  RCGO(rc, finish);
  
  if (fsmstate.exfile.file.ostatus & IWFS_OPEN_NEW) {
    // Write magic number
    lv = IWKV_MAGIC;
    lv = IW_HTOIL(lv);
    rc = fsm->writehdr(fsm, 0, &lv, sizeof(lv));
    RCGO(rc, finish);
    fsm->sync(fsm, 0);
  } else {
    uint8_t hdr[KVHDRSZ];
    rc = fsm->readhdr(fsm, 0, hdr, KVHDRSZ);
    RCGO(rc, finish);
    rp = hdr;
    memcpy(&lv, rp, sizeof(lv));
    rp += sizeof(lv);
    lv = IW_ITOHL(lv);
    if (lv != IWKV_MAGIC) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    memcpy(&llv, rp, sizeof(llv));
    llv = IW_ITOHLL(llv);
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCGO(rc, finish);
    rc = _db_load_chain(iwkv, llv, mm);
    fsm->release_mmap(fsm);
  }
  
  
  
  
  (*iwkvp)->open = true;
finish:
  if (rc) {
    (*iwkvp)->open = true;
    IWRC(iwkv_close(iwkvp), rc);
  }
  return rc;
}

iwrc iwkv_close(IWKV *iwkvp) {
  ENSURE_OPEN((*iwkvp));
  int rci;
  IWKV iwkv = *iwkvp;
  iwkv->open = false;
  iwrc rc = _wnw(iwkv, _wnw_iwkw_wl);
  RCRET(rc);
  IWDB db = iwkv->first_db;
  while (db) {
    IWDB ndb = db->next;
    
    _db_release_lw(&db);
    db = ndb;
  }
  IWRC(iwkv->fsm.close(&iwkv->fsm), rc);
  if (iwkv->dbs) {
    kh_destroy(DBS, iwkv->dbs);
    iwkv->dbs = 0;
  }
  API_UNLOCK(iwkv, rci, rc);
  pthread_rwlock_destroy(&iwkv->rwl);
  pthread_mutex_destroy(&iwkv->wk_mtx);
  pthread_cond_destroy(&iwkv->wk_cond);
  free(iwkv);
  *iwkvp = 0;
  return rc;
}

iwrc iwkv_sync(IWKV iwkv, iwfs_sync_flags _flags) {
  ENSURE_OPEN(iwkv);
  if (iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  iwrc rc = 0;
  IWFS_FSM *fsm  = &iwkv->fsm;
  pthread_rwlock_rdlock(&iwkv->rwl);
  iwfs_sync_flags flags = IWFS_FDATASYNC | _flags;
  IWRC(fsm->sync(fsm, flags), rc);
  pthread_rwlock_unlock(&iwkv->rwl);
  return rc;
}

iwrc iwkv_db(IWKV iwkv, uint32_t dbid, iwdb_flags_t dbflg, IWDB *dbp) {
  int rci;
  iwrc rc = 0;
  IWDB db = 0;
  *dbp = 0;
  API_RLOCK(iwkv, rci);
  khiter_t ki = kh_get(DBS, iwkv->dbs, dbid);
  if (ki != kh_end(iwkv->dbs)) {
    db = kh_value(iwkv->dbs, ki);
  }
  API_UNLOCK(iwkv, rci, rc);
  RCRET(rc);
  if (db) {
    if (db->dbflg != dbflg) {
      return IWKV_ERROR_INCOMPATIBLE_DB_MODE;
    }
    *dbp = db;
    return 0;
  }
  if (iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  API_WLOCK(iwkv, rci);
  ki = kh_get(DBS, iwkv->dbs, dbid);
  if (ki != kh_end(iwkv->dbs)) {
    db = kh_value(iwkv->dbs, ki);
  }
  if (db) {
    if (db->dbflg != dbflg) {
      return IWKV_ERROR_INCOMPATIBLE_DB_MODE;
    }
    *dbp = db;
  } else {
    rc = _db_create_lw(iwkv, dbid, dbflg, dbp);
  }
  API_UNLOCK(iwkv, rci, rc);
  return rc;
}

iwrc iwkv_db_destroy(IWDB *dbp) {
  assert(dbp && *dbp);
  int rci;
  IWDB db = *dbp;
  IWKV iwkv = db->iwkv;
  if (iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  iwrc rc = _wnw(iwkv, _wnw_iwkw_wl);
  RCGO(rc, finish);
  rc = _iwkv_worker_inc_nolk(iwkv);
  if (!rc) {
    rc = _db_destroy_lw(dbp);
  }
  API_UNLOCK(iwkv, rci, rc);
finish:
  return rc;
}

iwrc iwkv_put(IWDB db, const IWKV_val *key, const IWKV_val *val, iwkv_opflags opflags) {
  if (!db || !db->iwkv || !key || !key->size || !val) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (db->iwkv->oflags & IWKV_RDONLY) {
    return IW_ERROR_READONLY;
  }
  if (((db->dbflg & IWDB_UINT32_KEYS) && key->size != 4) ||
      ((db->dbflg & IWDB_UINT64_KEYS) && key->size != 8)) {
    return IWKV_ERROR_KEY_NUM_VALUE_SIZE;
  }
  int rci;
  iwrc rc = 0;
  IWLCTX lx = {
    .db = db,
    .key = key,
    .val = (IWKV_val *) val,
    .nlvl = -1,
    .op = IWLCTX_PUT,
    .opflags = opflags
  };
  API_DB_WLOCK(db, rci);
  rc = _lx_put_lw(&lx);
  API_DB_UNLOCK(db, rci, rc);
  if (!rc && (lx.opflags & IWKV_SYNC)) {
    rc = iwkv_sync(lx.db->iwkv, IWFS_NO_MMASYNC);
  }
  return rc;
}

iwrc iwkv_get(IWDB db, const IWKV_val *key, IWKV_val *oval) {
  if (!db || !db->iwkv || !key || !oval) {
    return IW_ERROR_INVALID_ARGS;
  }
  int rci;
  iwrc rc = 0;
  IWLCTX lx = {
    .db = db,
    .key = key,
    .val = oval,
    .nlvl = -1
  };
  oval->size = 0;
  API_DB_RLOCK(db, rci);
  rc = _lx_get_lr(&lx);
  API_DB_UNLOCK(db, rci, rc);
  return rc;
}

iwrc iwkv_del(IWDB db, const IWKV_val *key) {
  if (!db || !db->iwkv || !key) {
    return IW_ERROR_INVALID_ARGS;
  }
  int rci;
  iwrc rc = 0;
  IWLCTX lx = {
    .db = db,
    .key = key,
    .nlvl = -1,
    .op = IWLCTX_DEL
  };
  API_DB_WLOCK(db, rci);
  rc = _lx_del_lw(&lx);
  API_DB_UNLOCK(db, rci, rc);
  if (!rc && (lx.opflags & IWKV_SYNC)) {
    rc = iwkv_sync(lx.db->iwkv, IWFS_NO_MMASYNC);
  }
  return rc;
}

IW_INLINE iwrc _cursor_close_lw(IWKV_cursor cur) {
  cur->closed = true;
  if (cur->cn) {
    if (IW_UNLIKELY((cur->cn->flags & SBLK_DURTY) || (cur->cn->kvblk && (cur->cn->kvblk->flags & KVBLK_DURTY)))) {
      // Flush current node
      IWFS_FSM *fsm = &cur->lx.db->iwkv->fsm;
      uint8_t *mm;
      iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCRET(rc);
      _sblk_sync_and_release_mm(&cur->lx, &cur->cn, mm);
      fsm->release_mmap(fsm);
    } else {
      _sblk_release(&cur->lx, &cur->cn);
    }
  }
  return 0;
}

iwrc iwkv_cursor_open(IWDB db,
                      IWKV_cursor *curptr,
                      IWKV_cursor_op op,
                      const IWKV_val *key) {
  if (!db || !db->iwkv || !curptr ||
      (key && op < IWKV_CURSOR_EQ) || op < IWKV_CURSOR_BEFORE_FIRST) {
    return IW_ERROR_INVALID_ARGS;
  }
  int rci;
  iwrc rc = _db_worker_inc_nolk(db);
  RCRET(rc);
  rc = _api_db_rlock(db);
  if (rc) {
    _db_worker_dec_nolk(db);
    return rc;
  }
  RCRET(rc);
  *curptr = calloc(1, sizeof(**curptr));
  if (!curptr) {
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    goto finish;
  }
  IWKV_cursor cur = *curptr;
  cur->lx.db = db;
  cur->lx.key = key;
  cur->lx.nlvl = -1;
  rc = _cursor_to_lr(cur, op);
finish:
  if (rc && cur) {
    IWRC(_cursor_close_lw(cur), rc);
  }
  API_DB_UNLOCK(db, rci, rc);
  if (rc) {
    _db_worker_dec_nolk(db);
  }
  return rc;
}

iwrc iwkv_cursor_close(IWKV_cursor *curp) {
  iwrc rc = 0;
  int rci;
  if (!curp) {
    return IW_ERROR_INVALID_ARGS;
  }
  IWKV_cursor cur = *curp;
  if (cur->closed) {
    free(cur);
    return 0;
  }
  if (!cur->lx.db) {
    return IW_ERROR_INVALID_STATE;
  }
  API_DB_WLOCK(cur->lx.db, rci);
  rc = _cursor_close_lw(cur);
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  IWRC(_db_worker_dec_nolk(cur->lx.db), rc);
  free(cur);
  *curp = 0;
  return rc;
}

iwrc iwkv_cursor_to(IWKV_cursor cur, IWKV_cursor_op op) {
  int rci;
  if (!cur) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!cur->lx.db) {
    return IW_ERROR_INVALID_STATE;
  }
  API_DB_RLOCK(cur->lx.db, rci);
  iwrc rc = _cursor_to_lr(cur, op);
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

iwrc iwkv_cursor_to_key(IWKV_cursor cur, IWKV_cursor_op op, const IWKV_val *key) {
  int rci;
  if (!cur || (op != IWKV_CURSOR_EQ && op != IWKV_CURSOR_GE)) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!cur->lx.db) {
    return IW_ERROR_INVALID_STATE;
  }
  API_DB_RLOCK(cur->lx.db, rci);
  cur->lx.key = key;
  iwrc rc = _cursor_to_lr(cur, op);
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

iwrc iwkv_cursor_get(IWKV_cursor cur,
                     IWKV_val *okey,   /* Nullable */
                     IWKV_val *oval) { /* Nullable */
  int rci;
  iwrc rc = 0;
  if (!cur) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!cur->cn || !cur->lx.db || cur->cnpos >= cur->cn->pnum) {
    return IW_ERROR_INVALID_STATE;
  }
  API_DB_RLOCK(cur->lx.db, rci);
  uint8_t *mm = 0;
  IWFS_FSM *fsm = &cur->lx.db->iwkv->fsm;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  if (!cur->cn->kvblk) {
    rc = _sblk_loadkvblk_mm(&cur->lx, cur->cn, mm);
    RCGO(rc, finish);
  }
  int8_t idx = cur->cn->pi[cur->cnpos];
  if (okey && oval) {
    rc = _kvblk_getkv(cur->cn->kvblk, mm, idx, okey, oval);
  } else if (oval) {
    rc = _kvblk_getvalue(cur->cn->kvblk, mm, idx, oval);
  } else if (okey) {
    rc = _kvblk_getkey(cur->cn->kvblk, mm, idx, okey);
  } else {
    rc = IW_ERROR_INVALID_ARGS;
  }
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

iwrc iwkv_cursor_set(IWKV_cursor cur, IWKV_val *val, iwkv_opflags opflags) {
  int rci;
  iwrc rc = 0;
  if (!cur) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!cur->cn || !cur->lx.db || (cur->cn->flags & SBLK_DB)) {
    return IW_ERROR_INVALID_STATE;
  }
  IWDB db = cur->lx.db;
  API_DB_WLOCK(db, rci);
  rc = _sblk_updatekv(cur->cn, cur->cnpos, 0, val, opflags);
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

iwrc iwkv_cursor_val(IWKV_cursor cur, IWKV_val *oval) {
  return iwkv_cursor_get(cur, 0, oval);
}

iwrc iwkv_cursor_key(IWKV_cursor cur, IWKV_val *okey) {
  return iwkv_cursor_get(cur, okey, 0);
}

static iwrc _cursor_dup_add(IWKV_cursor cur, uint64_t dv, iwkv_opflags opflags) {
  if (!cur) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!cur->cn || !cur->lx.db || (cur->cn->flags & SBLK_DB) || !(cur->lx.db->dbflg & IWDB_DUP_FLAGS)) {
    return IW_ERROR_INVALID_STATE;
  }
  uint8_t data[8];
  IWKV_val val = {.data = data};
  if (cur->lx.db->dbflg & IWDB_DUP_UINT32_VALS) {
    uint32_t lv = dv;
    memcpy(val.data, &lv, sizeof(lv));
    val.size = sizeof(lv);
  } else {
    memcpy(val.data, &dv, sizeof(dv));
    val.size = sizeof(dv);
  }
  return iwkv_cursor_set(cur, &val, opflags);
}

iwrc iwkv_cursor_dup_rm(IWKV_cursor cur, uint64_t dv) {
  return _cursor_dup_add(cur, dv, IWKV_DUP_REMOVE);
}

iwrc iwkv_cursor_dup_add(IWKV_cursor cur, uint64_t dv) {
  return _cursor_dup_add(cur, dv, 0);
}

iwrc iwkv_cursor_dup_num(const IWKV_cursor cur, uint32_t *onum) {
  int rci;
  iwrc rc;
  if (!cur || !onum) {
    return IW_ERROR_INVALID_ARGS;
  }
  *onum = 0;
  if (!cur->cn || !cur->lx.db || (cur->cn->flags & SBLK_DB) || !(cur->lx.db->dbflg & IWDB_DUP_FLAGS)) {
    return IW_ERROR_INVALID_STATE;
  }
  API_DB_RLOCK(cur->lx.db, rci);
  uint32_t vlen, lv;
  uint8_t *vbuf;
  uint8_t *mm = 0;
  int8_t idx = cur->cn->pi[cur->cnpos];
  IWFS_FSM *fsm = &cur->lx.db->iwkv->fsm;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  if (!cur->cn->kvblk) {
    rc = _sblk_loadkvblk_mm(&cur->lx, cur->cn, mm);
    RCGO(rc, finish);
  }
  _kvblk_peek_val(cur->cn->kvblk, idx, mm, &vbuf, &vlen);
  if (vlen < 4) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  memcpy(&lv, vbuf, sizeof(lv));
  lv = IW_ITOHL(lv);
  *onum = lv;
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

iwrc iwkv_cursor_dup_contains(const IWKV_cursor cur, uint64_t dv, bool *out) {
  if (!cur || !out) {
    return IW_ERROR_INVALID_ARGS;
  }
  *out = false;
  if (!cur->cn || !cur->lx.db || (cur->cn->flags & SBLK_DB) || !(cur->lx.db->dbflg & IWDB_DUP_FLAGS)) {
    return IW_ERROR_INVALID_STATE;
  }
  int rci;
  iwrc rc;
  API_DB_RLOCK(cur->lx.db, rci);
  uint32_t len, num;
  uint8_t *rp, *mm = 0;
  int8_t idx = cur->cn->pi[cur->cnpos];
  const int elsz = (cur->lx.db->dbflg & IWDB_DUP_UINT32_VALS) ? 4 : 8;
  IWFS_FSM *fsm = &cur->lx.db->iwkv->fsm;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  if (!cur->cn->kvblk) {
    rc = _sblk_loadkvblk_mm(&cur->lx, cur->cn, mm);
    RCGO(rc, finish);
  }
  _kvblk_peek_val(cur->cn->kvblk, idx, mm, &rp, &len);
  if (len < 4) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  memcpy(&num, rp, sizeof(num));
  num = IW_ITOHL(num); // Number of items
  if (!num) {
    goto finish;
  }
  rp += 4;
  if (elsz < 8) {
    uint32_t lv = dv;
    lv = IW_HTOIL(lv);
    *out = (iwarr_sorted_find(rp, num, elsz, &lv, _u4cmp) != -1);
  } else {
    dv = IW_HTOILL(dv);
    *out = (iwarr_sorted_find(rp, num, elsz, &dv, _u8cmp) != -1);
  }
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

iwrc iwkv_cursor_dup_iter(const IWKV_cursor cur,
                          bool(*visitor)(uint64_t dv, void *opaq),
                          void *opaq,
                          const uint64_t *start,
                          bool down) {
  if (!cur || !visitor) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!cur->cn || !cur->lx.db || (cur->cn->flags & SBLK_DB) || !(cur->lx.db->dbflg & IWDB_DUP_FLAGS)) {
    return IW_ERROR_INVALID_STATE;
  }
  int rci;
  iwrc rc;
  API_DB_RLOCK(cur->lx.db, rci);
  off_t sidx;
  uint32_t num;
  uint8_t *rp, *mm = 0;
  int8_t idx = cur->cn->pi[cur->cnpos];
  const int elsz = (cur->lx.db->dbflg & IWDB_DUP_UINT32_VALS) ? 4 : 8;
  IWFS_FSM *fsm = &cur->lx.db->iwkv->fsm;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCGO(rc, finish);
  if (!cur->cn->kvblk) {
    rc = _sblk_loadkvblk_mm(&cur->lx, cur->cn, mm);
    RCGO(rc, finish);
  }
  _kvblk_peek_val(cur->cn->kvblk, idx, mm, &rp, &num);
  if (num < 4) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  memcpy(&num, rp, sizeof(num));
  num = IW_ITOHL(num); // Number of items
  if (!num) {
    goto finish;
  }
  rp += 4;
  if (start) {
    if (elsz < 8) {
      uint32_t lv = *start;
      lv = IW_HTOIL(lv);
      sidx = iwarr_sorted_find(rp, num, elsz, &lv, _u4cmp);
    } else {
      uint64_t llv = *start;
      llv = IW_HTOILL(llv);
      sidx = iwarr_sorted_find(rp, num, elsz, &llv, _u8cmp);
    }
    if (sidx < 0) {
      rc = IWKV_ERROR_NOTFOUND;
      goto finish;
    }
  } else {
    sidx = down ? num - 1 : 0;
  }
#define _VBODY                    \
  uint64_t dv;                    \
  uint8_t *np = rp + sidx * elsz; \
  if (elsz < 8) {                 \
    uint32_t lv;                  \
    memcpy(&lv, np, elsz);        \
    dv = IW_ITOHL(lv);            \
  } else {                        \
    memcpy(&dv, np, elsz);        \
    dv = IW_ITOHLL(dv);           \
  }                               \
  if (visitor(dv, opaq)) break;
  // !_VBODY
  if (down) {
    for (; sidx >= 0; --sidx) {
      _VBODY
    }
  } else {
    for (; sidx < num; ++sidx) {
      _VBODY
    }
  }
#undef _VBODY
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  API_DB_UNLOCK(cur->lx.db, rci, rc);
  return rc;
}

//--------------------------  DEBUG STAFF

void iwkvd_kvblk(FILE *f, KVBLK *kb, int maxvlen) {
  assert(f && kb && kb->addr);
  uint8_t *mm;
  uint8_t *kbuf, *vbuf;
  uint32_t klen, vlen;
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  blkn_t blkn = ADDR2BLK(kb->addr);
  fprintf(f, "\n === KVBLK[%u] maxoff=%lu, zidx=%d, idxsz=%d, szpow=%u, flg=%x, db=%d\n",
          blkn, kb->maxoff, kb->zidx, kb->idxsz, kb->szpow, kb->flags, kb->db->id);
          
  iwrc rc = fsm->probe_mmap(fsm, 0, &mm, 0);
  if (rc) {
    iwlog_ecode_error3(rc);
    return;
  }
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    KVP *kvp = &kb->pidx[i];
    _kvblk_peek_key(kb, i, mm, &kbuf, &klen);
    _kvblk_peek_val(kb, i, mm, &vbuf, &vlen);
    fprintf(f, "\n    %02d: [%04lu, %02u, %02d]: %.*s:%.*s",
            i, kvp->off, kvp->len, kvp->ridx,
            klen, kbuf, MIN(vlen, maxvlen), vbuf);
  }
  fprintf(f, "\n");
}

#define IWKVD_MAX_VALSZ 96

void iwkvd_sblk(FILE *f, IWLCTX *lx, SBLK *sb, int flags) {
  assert(sb && sb->addr);
  int lkl = 0;
  char lkbuf[SBLK_LKLEN + 1] = {0};
  uint8_t *mm;
  uint8_t *kbuf, *vbuf;
  uint32_t klen, vlen;
  IWFS_FSM *fsm = &sb->db->iwkv->fsm;
  blkn_t blkn = ADDR2BLK(sb->addr);
  iwrc rc = fsm->probe_mmap(fsm, 0, &mm, 0);
  if (rc) {
    iwlog_ecode_error3(rc);
    return;
  }
  rc = _sblk_loadkvblk_mm(lx, sb, mm);
  if (rc) {
    iwlog_ecode_error3(rc);
    return;
  }
  assert(sb->kvblk);
  if (sb->flags & SBLK_DB) {
    lkl = 0;
  } else {
    memcpy(&lkl, mm + sb->addr + SOFF_LKL_U1, 1);
    memcpy(lkbuf, mm + sb->addr + SOFF_LK, lkl);
  }
  fprintf(f, "\n === SBLK[%u] lvl=%d, pnum=%d, flg=%x, kvzidx=%d, p0=%d, db=%d",
          blkn,
          ((IWKVD_PRINT_NO_LEVEVELS & flags) ? -1 : sb->lvl),
          sb->pnum, sb->flags, sb->kvblk->zidx,
          sb->p0,
          sb->kvblk->db->id);
          
          
  if (sb->db->dbflg & IWDB_UINT64_KEYS) {
    uint64_t k;
    memcpy(&k, lkbuf, sizeof(k));
    k = IW_ITOHLL(k);
    fprintf(f, "\n === SBLK[%u] szpow=%d, lkl=%d, lk=%lu\n", blkn, sb->kvblk->szpow, lkl, k);
  } else if (sb->db->dbflg & IWDB_UINT32_KEYS) {
    uint32_t k;
    memcpy(&k, lkbuf, sizeof(k));
    k = IW_ITOHL(k);
    fprintf(f, "\n === SBLK[%u] szpow=%d, lkl=%d, lk=%u\n", blkn, sb->kvblk->szpow, lkl, k);
  } else {
    fprintf(f, "\n === SBLK[%u] szpow=%d, lkl=%d, lk=%s\n", blkn, sb->kvblk->szpow,  lkl, lkbuf);
  }
  
  for (int i = 0, j = 0; i < sb->pnum; ++i, ++j) {
    if (j == 3) {
      fputc('\n', f);
      j = 0;
    }
    if (j == 0) {
      fprintf(f, " === SBLK[%u]", blkn);
    }
    _kvblk_peek_key(sb->kvblk, sb->pi[i], mm, &kbuf, &klen);
    if (flags & IWKVD_PRINT_VALS) {
      _kvblk_peek_val(sb->kvblk, sb->pi[i], mm, &vbuf, &vlen);
      if (sb->db->dbflg & IWDB_UINT64_KEYS) {
        uint64_t k;
        memcpy(&k, kbuf, sizeof(k));
        k = IW_ITOHLL(k);
        fprintf(f, "    [%03d,%03d] %lu:%.*s", i, sb->pi[i], k, MIN(vlen, IWKVD_MAX_VALSZ), vbuf);
      } else if (sb->db->dbflg & IWDB_UINT32_KEYS) {
        uint32_t k;
        memcpy(&k, kbuf, sizeof(k));
        k = IW_ITOHL(k);
        fprintf(f, "    [%03d,%03d] %u:%.*s", i, sb->pi[i], k, MIN(vlen, IWKVD_MAX_VALSZ), vbuf);
      } else {
        fprintf(f, "    [%03d,%03d] %.*s:%.*s", i, sb->pi[i], klen, kbuf, MIN(vlen, IWKVD_MAX_VALSZ), vbuf);
      }
    } else {
      if (sb->db->dbflg & IWDB_UINT64_KEYS) {
        uint64_t k;
        memcpy(&k, kbuf, sizeof(k));
        k = IW_ITOHLL(k);
        fprintf(f, "    [%03d,%03d] %lu", i, sb->pi[i], k);
      } else if (sb->db->dbflg & IWDB_UINT32_KEYS) {
        uint32_t k;
        memcpy(&k, kbuf, sizeof(k));
        k = IW_ITOHL(k);
        fprintf(f, "    [%03d,%03d] %u", i, sb->pi[i], k);
      } else {
        fprintf(f, "    [%03d,%03d] %.*s", i, sb->pi[i], klen, kbuf);
      }
    }
  }
  fprintf(f, "\n\n");
}

IWFS_FSM *iwkvd_fsm(IWKV kv) {
  return &kv->fsm;
}

void iwkvd_db(FILE *f, IWDB db, int flags) {
  assert(db);
  SBLK *sb, *tail;
  IWLCTX lx = {
    .db = db,
    .nlvl = -1
  };
  iwrc rc = _sblk_at(&lx, db->addr, 0, &sb);
  if (rc) {
    iwlog_ecode_error3(rc);
    return;
  }
  rc = _sblk_at(&lx, 0, 0, &tail);
  if (rc) {
    iwlog_ecode_error3(rc);
    return;
  }
  fprintf(f, "\n\n== DB[%d] lvl=%d, blk=%u, dbflg=%x, p0=%u",
          db->id,
          ((IWKVD_PRINT_NO_LEVEVELS & flags) ? -1 : sb->lvl),
          (unsigned int) ADDR2BLK(sb->addr),
          db->dbflg,
          tail->p0);
  if (!(IWKVD_PRINT_NO_LEVEVELS & flags)) {
    fprintf(f, "\n== DB[%d]->n=[", db->id);
    for (int i = 0; i <= sb->lvl; ++i) {
      if (i > 0) {
        fprintf(f, ", %d:%d", i, sb->n[i]);
      } else {
        fprintf(f, "%d:%d", i, sb->n[i]);
      }
    }
    fprintf(f, "]");
  }
  blkn_t blk = sb->n[0];
  while (blk) {
    rc = _sblk_at(&lx, BLK2ADDR(blk), 0, &sb);
    if (rc) {
      iwlog_ecode_error3(rc);
      return;
    }
    iwkvd_sblk(f, &lx, sb, flags);
    blk = sb->n[0];
    _sblk_release(&lx, &sb);
  }
  fflush(f);
}
