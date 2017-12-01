#include "iwkv.h"
#include "iwlog.h"
#include "iwarr.h"
#include "iwutils.h"
#include "iwfsmfile.h"
#include "iwcfg.h"
#include "khash.h"
#include <stdbool.h>
#include <pthread.h>

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

// Lower key length in SBLK
#define SBLK_LKLEN 61

// Size of `SBLK` as power of 2
#define SBLK_SZPOW 8

// Size of `IWDB` as power of 2
#define DB_SZPOW 8

// Number of `KV` blocks in KVBLK
#define KVBLK_IDXNUM 63

// Initial `KVBLK` size power of 2 (256 bytes)
#define KVBLK_INISZPOW 8

// KVBLK header size: blen:u1,idxsz:u2
#define KVBLK_HDRSZ 3

// Max non KV size [blen:u1,idxsz:u2,[ps1:vn,pl1:vn,...,ps63,pl63]
#define KVBLK_MAX_NKV_SZ (KVBLK_HDRSZ + KVBLK_IDXNUM * 8)

#define IWKV_ISLIGHT_ERROR(rc_) \
  ((rc_) == IWKV_ERROR_NOTFOUND || (rc_) == IWKV_ERROR_KEY_EXISTS)

#define ADDR2BLK(addr_) ((addr_) >> IWKV_FSM_BPOW)

#define BLK2ADDR(blk_) (((off_t) (blk_)) << IWKV_FSM_BPOW)

volatile int8_t iwkv_next_level = -1;

struct IWKV;
struct IWDB;

typedef uint32_t blkn_t;
typedef uint32_t dbid_t;

/* Key/Value pair */
typedef struct KV {
  uint8_t *key;
  uint8_t *val;
  size_t keysz;
  size_t valsz;
} KV;

/* KV index: Offset and length. */
typedef struct KVP {
  uint32_t off;   /**< KV block offset relative to `end` of KVBLK */
  uint32_t len;   /**< Length of kv pair block */
  uint8_t  ridx;  /**< Position of the auctually persisted slot in `KVBLK` */
} KVP;

typedef enum {
  KVBLK_DURTY = 1 /**< KVBLK data is dury and should be flushed to mm */
} kvblk_flags_t;

typedef enum {
  RMKV_SYNC = 1,
  RMKV_NO_RESIZE = 1 << 1
} kvblk_rmkv_opts_t;

/* KVBLK: [blen:u1,idxsz:u2,[pp1:vn,pl1:vn,...,pp63,pl63]____[[pair],...]] */
typedef struct KVBLK {
  IWDB db;
  off_t addr;                 /**< Block address */
  uint32_t maxoff;            /**< Max pair offset */
  uint16_t idxsz;             /**< Size of KV pairs index in bytes */
  int8_t zidx;                /**< Index of first empty pair slot, or -1 */
  uint8_t szpow;              /**< Block size power of 2 */
  KVP pidx[KVBLK_IDXNUM];     /**< KV pairs index */
  kvblk_flags_t flags;        /**< Flags */
} KVBLK;

typedef enum {
  SBH_FULL_LKEY = 1,          /**< The lowest `SBLK` key is fully contained in `SBLK`. Persistent flag. */
  SBH_DB = 1 << 1,            /**< This block is the database block. */
  SBH_PINNED = 1 << 2,        /**< `SBH` pinned and should not be released. */
  SBH_WLOCKED = 1 << 3,       /**< `SBH` write locked */
  SBH_NO_LOCK = 1 << 4,       /**< Do not use locks when accessing `SBH`(used in debug print routines) */
} sbh_flags_t;

#define SBH_PERSISTENT_FLAGS (SBH_FULL_LKEY)

/* Common header for IWDB and SBLK */
typedef struct SBH {
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sbh_flags_t flags;          /**< Flags */
  uint8_t lvl;                /**< Skip list node level */
} SBH;

/* Address lock node */
typedef struct ALN {
  pthread_rwlock_t rwl;       /**< RW lock */
  int64_t refs;               /**< Locked address refs count */
  bool write_pending;         /**< Pending write lock */
  bool write_upgraded;        /**< Lock is write upgraded */
} ALN;

KHASH_MAP_INIT_INT(ALN, ALN *)

/* Database: [magic:u4,flags:u1,dbid:u4,next_blk:u4,p0:u4,n0-n29:u4] */
struct IWDB {
  // SBH
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sbh_flags_t flags;          /**< Flags */
  uint8_t lvl;                /**< Skip list node level */
  // !SBH
  IWKV iwkv;
  iwdb_flags_t dbflg;         /**< Database flags */
  pthread_mutex_t mtx_ctl;    /**< Main control mutex */
  dbid_t id;                  /**< Database ID */
  uint64_t next_db_addr;      /**< Next IWDB addr */
  struct IWDB *next;          /**< Next IWDB meta */
  struct IWDB *prev;          /**< Prev IWDB meta */
  khash_t(ALN) *aln;          /**< Block id -> ALN node mapping */
};

/* Skiplist block */
typedef struct SBLK {
  // SBH
  IWDB db;                    /**< Database ref */
  off_t addr;                 /**< Block address */
  sbh_flags_t flags;          /**< Flags */
  uint8_t lvl;                /**< Skip list node level */
  // !SBH
  KVBLK *kvblk;               /**< Associated KVBLK */
} SBLK;


KHASH_MAP_INIT_INT(DBS, IWDB)

/** IWKV instance */
struct IWKV {
  pthread_rwlock_t rwl_api;   /**< API RW lock */
  IWFS_FSM fsm;               /**< FSM pool */
  blkn_t  metablk;            /**< Database meta block */
  khash_t(DBS) *dbs;          /**< Database id -> IWDB mapping */
  IWDB dblast;                /**< Last database in chain */
  IWDB first_db;              /**< First database in chain */
  iwkv_openflags oflags;      /**< Open flags */
  bool open;                  /**< True if kvstore is in OPEN state */
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
  SBH *lower;                 /**< Next to upper bound block */
  SBH *upper;                 /**< Upper bound block */
  SBH *nb;                    /**< New block */
  SBH *plower[SLEVELS];       /**< Pinned lower nodes per level */
  SBH *pupper[SLEVELS];       /**< Pinned upper nodes per level */
  int8_t lvl;                 /**< Current level */
  int8_t nlvl;                /**< Level of new inserted `SBLK` node. -1 if no new node inserted */
  iwlctx_op_t op;             /**< Context operation flags */
  iwkv_opflags opflg;         /**< Operation flags */
} IWLCTX;

void iwkvd_kvblk(FILE *f, KVBLK *kb);
void iwkvd_sblk(FILE *f, SBH *sb, int flags);
void iwkvd_db(FILE *f, IWDB db, int flags);

#define ENSURE_OPEN(iwkv_) \
  if (!iwkv_ || !(iwkv_->open)) return IW_ERROR_INVALID_STATE;

#define API_RLOCK(iwkv_, rci_) \
  rci_ = pthread_rwlock_rdlock(&(iwkv_)->rwl_api); \
  if (rci_) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_)

#define API_WLOCK(iwkv_, rci_) \
  rci_ = pthread_rwlock_wrlock(&(iwkv_)->rwl_api); \
  if (rci_) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_)

#define API_UNLOCK(iwkv_, rci_, rc_)  \
  rci_ = pthread_rwlock_unlock(&(iwkv_)->rwl_api); \
  if (rci_) IWRC(iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci_), rc_)

// SBLK
// [u1:flags,kblk:u4,lvl:u1,p0:u4,n0-n29:u4,lkl:u1,lk:u61,pnum:u1,[pi0:u1,...pi62]]:u256
#define SOFF_FLAGS_U1     0
#define SOFF_KBLK_U4      1
#define SOFF_LVL_U1       (SOFF_KBLK_U4 + 4)
#define SOFF_P0_U4        (SOFF_LVL_U1 + 1)
#define SOFF_N0_U4        (SOFF_P0_U4 + 4)
#define SOFF_LKL_U1       (SOFF_N0_U4 + 4 * SLEVELS)
#define SOFF_LK_U61       (SOFF_LKL_U1 + 1)
#define SOFF_PNUM_U1      (SOFF_LK_U61 + SBLK_LKLEN)
#define SOFF_PI0_U1       (SOFF_PNUM_U1 + 1)
#define SOFF_END          (SOFF_PI0_U1 + 1 * KVBLK_IDXNUM)

// DB
// [magic:u4,dbflg:u1,dbid:u4,next_db_blk:u4,p0:u4,n0-n29:u4]
#define DOFF_MAGIC_U4     0
#define DOFF_DBFLG_U1     (DOFF_MAGIC_U4 + 4)
#define DOFF_DBID_U4      (DOFF_DBFLG_U1 + 1)
#define DOFF_NEXTDB_U4    (DOFF_DBID_U4 + 4)
#define DOFF_P0_U4        (DOFF_NEXTDB_U4 + 4)
#define DOFF_N0_U4        (DOFF_P0_U4 + 4)
#define DOFF_END          (DOFF_N0_U4 + 4 * SLEVELS)

//-------------------------- MM ACCESS

#define _mm_u1(mm_, off_) (*((uint8_t*)((mm_) + (off_))))

#define _mm_set_u1(mm_, off_, v_) (*((mm_) + (off_)) = (v_))

IW_INLINE uint32_t _mm_u4(uint8_t *mm, off_t off) {
  uint32_t lv;
  memcpy(&lv, mm + off, sizeof(lv));
  return IW_ITOHL(lv);
}

IW_INLINE void _mm_set_u4(uint8_t *mm, off_t off, uint32_t lv) {
  lv = IW_HTOIL(lv);
  memcpy(mm + off, &lv, sizeof(lv));
}

IW_INLINE uint64_t _mm_u8(uint8_t *mm, off_t off) {
  uint64_t llv;
  memcpy(&llv, mm + off, sizeof(llv));
  return IW_ITOHLL(llv);
}

IW_INLINE void _mm_set_u8(uint8_t *mm, off_t off, uint64_t llv) {
  llv = IW_HTOILL(llv);
  memcpy(mm + off, &llv, sizeof(llv));
}

//-------------------------- SBH

IW_INLINE uint8_t _sbh_u1(SBH *sbh, off_t off, uint8_t *mm) {
  return _mm_u1(mm + sbh->addr, off);
}

IW_INLINE void _sbh_set_u1(SBH *sbh, off_t off, uint8_t *mm, uint8_t v) {
  _mm_set_u1(mm + sbh->addr, off, v);
}

IW_INLINE uint32_t _sbh_u4(SBH *sbh, off_t off, uint8_t *mm) {
  return _mm_u4(mm + sbh->addr, off);
}

IW_INLINE void _sbh_set_u4(SBH *sbh, off_t off, uint8_t *mm, uint32_t lv) {
  _mm_set_u4(mm + sbh->addr, off, lv);
}

IW_INLINE uint64_t _sbh_u8(SBH *sbh, off_t off, uint8_t *mm) {
  return _mm_u8(mm + sbh->addr, off);
}

IW_INLINE void _sbh_set_u8(SBH *sbh, off_t off, uint8_t *mm, uint64_t llv) {
  _mm_set_u8(mm + sbh->addr, off, llv);
}

IW_INLINE blkn_t _sbh_n(SBH *sbh, uint8_t *mm, uint32_t n) {
  assert(sbh && mm && n < SLEVELS);
  return _sbh_u4(sbh, ((sbh->flags & SBH_DB) ? DOFF_N0_U4 : SOFF_N0_U4) + n * sizeof(uint32_t), mm);
}

IW_INLINE void _sbh_set_n(SBH *sbh, uint8_t *mm, uint32_t n, blkn_t val) {
  assert(sbh && mm && n < SLEVELS);
  _sbh_set_u4(sbh, ((sbh->flags & SBH_DB) ? DOFF_N0_U4 : SOFF_N0_U4) + n * sizeof(uint32_t), mm, val);
}

IW_INLINE blkn_t _sbh_p0(SBH *sbh, uint8_t *mm, blkn_t val) {
  assert(sbh && mm);
  return _sbh_u4(sbh, ((sbh->flags & SBH_DB) ? DOFF_P0_U4 : SOFF_P0_U4), mm);
}

IW_INLINE void _sbh_set_p0(SBH *sbh, uint8_t *mm, blkn_t val) {
  assert(sbh && mm);
  _sbh_set_u4(sbh, ((sbh->flags & SBH_DB) ? DOFF_P0_U4 : SOFF_P0_U4), mm, val);
}

//--------------------------  ADDRESS LOCKING

IW_INLINE iwrc _aln_release(IWDB db, blkn_t blkn) {
  int rci = pthread_mutex_lock(&db->mtx_ctl);
  if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  khiter_t k = kh_get(ALN, db->aln, blkn);
  if (k != kh_end(db->aln)) {
    ALN *aln = kh_value(db->aln, k);
    assert(aln);
    pthread_rwlock_unlock(&aln->rwl);
    if (--aln->refs < 1 && !aln->write_pending) {
      kh_del(ALN, db->aln, k);
      free(aln);
    }
  }
  pthread_mutex_unlock(&db->mtx_ctl);
  return 0;
}

IW_INLINE iwrc _aln_acquire_read(IWDB db, blkn_t blkn) {
  ALN *aln;
  int rci;
  iwrc rc = 0;
  rci = pthread_mutex_lock(&db->mtx_ctl);
  if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  khiter_t k = kh_get(ALN, db->aln, blkn);
  if (k == kh_end(db->aln)) {
    aln = malloc(sizeof(*aln));
    if (!aln) {
      rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      goto finish;
    }
    aln->write_pending = false;
    k = kh_put(ALN, db->aln, blkn, &rci);
    if (rci != -1) {
      kh_value(db->aln, k) = aln;
    } else {
      rc = IW_ERROR_FAIL;
      free(aln);
      goto finish;
    }
    aln->refs = 1;
    pthread_rwlock_init(&aln->rwl, 0);
  } else {
    aln = kh_value(db->aln, k);
    aln->refs++;
  }
finish:
  pthread_mutex_unlock(&db->mtx_ctl);
  if (!rc) {
    rci = pthread_rwlock_rdlock(&aln->rwl);
    if (rci) {
      return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
    }
  }
  return rc;
}

IW_INLINE iwrc _aln_acquire_write_upgrade(IWDB db, blkn_t blkn) {
  ALN *aln;
  int rci;
  iwrc rc = 0;
  rci = pthread_mutex_lock(&db->mtx_ctl);
  if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  khiter_t k = kh_get(ALN, db->aln, blkn);
  if (k == kh_end(db->aln)) {
    aln = malloc(sizeof(*aln));
    if (!aln) {
      rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      goto finish;
    }
    k = kh_put(ALN, db->aln, blkn, &rci);
    if (rci != -1) {
      kh_value(db->aln, k) = aln;
    } else {
      rc = IW_ERROR_FAIL;
      free(aln);
      goto finish;
    }
    aln->refs = 1;
    pthread_rwlock_init(&aln->rwl, 0);
  } else {
    assert(aln->refs > 0);
    aln = kh_value(db->aln, k);
    pthread_rwlock_unlock(&aln->rwl);
    aln->refs--;
  }
  aln->write_pending = true;
finish:
  pthread_mutex_unlock(&db->mtx_ctl);
  if (!rc) {
    rci = pthread_rwlock_wrlock(&aln->rwl);
    if (rci) {
      if (aln->refs < 1) {
        rci = pthread_mutex_lock(&db->mtx_ctl);
        if (rci) return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
        if (aln->refs < 1) {
          kh_del(ALN, db->aln, k);
          free(aln);
        }
        pthread_mutex_unlock(&db->mtx_ctl);
      }
      return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
    }
    aln->refs = 1;
    aln->write_pending = false;
  }
  return rc;
}

IW_INLINE void _kv_dispose(IWKV_val *key, IWKV_val *val) {
  if (key) {
    if (key->data) {
      free(key->data);
    }
    key->size = 0;
    key->data = 0;
  }
  if (val) {
    if (val->data) {
      free(val->data);
    }
    val->size = 0;
    val->data = 0;
  }
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
  rci = pthread_mutex_init(&db->mtx_ctl, 0);
  if (rci) {
    free(db);
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  // [magic:u4,dbflg:u1,dbid:u4,next_db_addr:u4,p0:u4,n0-n29:u4]
  db->flags = SBH_DB;
  db->addr = addr;
  db->db = db;
  db->iwkv = iwkv;
  db->aln = kh_init(ALN);
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
  *dbp = db;
finish:
  if (rc)  {
    kh_destroy(ALN, (*dbp)->aln);
    pthread_mutex_destroy(&db->mtx_ctl);
    free(db);
  }
  return rc;
}

static void _db_save(IWDB db, uint8_t *mm) {
  uint32_t lv;
  uint8_t *wp = mm + db->addr;
  db->next_db_addr = db->next ? db->next->addr : 0;
  // [magic:u4,dbflg:u1,dbid:u4,next_db_addr:u4,p0:u4,n0-n29:u4]
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
    iwkv->dblast = db;
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
  pthread_mutex_destroy(&(*dbp)->mtx_ctl);
  kh_destroy(ALN, (*dbp)->aln);
  free(*dbp);
  *dbp = 0;
}

static iwrc _db_destroy_lw(IWDB *dbp) {
  iwrc rc;
  uint8_t *mm;
  IWDB db = *dbp;
  IWDB prev = db->prev;
  IWDB next = db->next;
  IWFS_FSM *fsm = &db->iwkv->fsm;

  kh_del(DBS, db->iwkv->dbs, db->id);
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  if (prev) {
    prev->next = next;
    _db_save(prev, mm);
  }
  if (next) {
    next->prev = prev;
    _db_save(next, mm);
  }
  fsm->release_mmap(fsm);
  if (db->iwkv->first_db && db->iwkv->first_db->addr == db->addr) {
    uint64_t llv;
    db->iwkv->first_db = next;
    llv = next ? next->addr : 0;
    llv = IW_HTOILL(llv);
    rc = fsm->writehdr(fsm, sizeof(uint32_t) /*skip magic*/, &llv, sizeof(llv));
  }
  if (db->iwkv->dblast && db->iwkv->dblast->addr == db->addr) {
    db->iwkv->dblast = prev;
  }
  //
  // TODO!!!: dispose all of `SBLK` & `KVBLK` blocks used by db
  //
  IWRC(fsm->deallocate(fsm, db->addr, (1 << DB_SZPOW)), rc);
  _db_release_lw(dbp);
  return rc;
}

static iwrc _db_create_lw(IWKV iwkv, dbid_t dbid, iwdb_flags_t dbflg, IWDB *odb) {
  iwrc rc;
  int rci;
  uint8_t *mm;
  off_t baddr = 0, blen;
  IWFS_FSM *fsm = &iwkv->fsm;
  *odb = 0;
  IWDB db = calloc(1, sizeof(struct IWDB));
  if (!db) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  rc = fsm->allocate(fsm, (1 << DB_SZPOW), &baddr, &blen,
                     IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
  if (rc) {
    _db_release_lw(&db);
    return rc;
  }
  db->iwkv = iwkv;
  db->dbflg = dbflg;
  db->addr = baddr;
  db->id = dbid;
  db->prev = iwkv->dblast;
  db->aln = kh_init(ALN);
  if (!iwkv->first_db) {
    uint64_t llv;
    iwkv->first_db = db;
    llv = db->addr;
    llv = IW_HTOILL(llv);
    rc = fsm->writehdr(fsm, sizeof(uint32_t) /*skip magic*/, &llv, sizeof(llv));
  } else if (iwkv->dblast) {
    iwkv->dblast->next = db;
  }
  iwkv->dblast = db;
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
  fsm->release_mmap(fsm);
  *odb = db;
finish:
  if (rc) {
    fsm->deallocate(fsm, baddr, blen);
    _db_release_lw(&db);
  }
  return rc;
}

//--------------------------  KVBLK

static iwrc _kvblk_create(IWDB db, int8_t kvbpow, KVBLK **oblk) {
  KVBLK *kblk;
  off_t baddr = 0, blen;
  IWFS_FSM *fsm = &db->iwkv->fsm;
  if (kvbpow < KVBLK_INISZPOW) {
    kvbpow = KVBLK_INISZPOW;
  }
  iwrc rc = fsm->allocate(fsm, (1ULL << kvbpow), &baddr, &blen,
                          IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
  RCRET(rc);
  kblk = calloc(1, sizeof(*kblk));
  if (!kblk) {
    rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    IWRC(fsm->deallocate(fsm, baddr, blen), rc);
    *oblk = 0;
    return rc;
  }
  kblk->db = db;
  kblk->addr = baddr;
  kblk->szpow = KVBLK_INISZPOW;
  kblk->idxsz = 2 * IW_VNUMSIZE(0) * KVBLK_IDXNUM;
  *oblk = kblk;
  return rc;
}

IW_INLINE void _kvblk_release(KVBLK **kbp) {
  assert(kbp && *kbp);
  free(*kbp);
  *kbp = 0;
}

IW_INLINE iwrc _kvblk_destroy(KVBLK **kbp) {
  assert(kbp && *kbp && (*kbp)->db && (*kbp)->szpow && (*kbp)->addr);
  KVBLK *blk = *kbp;
  IWFS_FSM *fsm = &blk->db->iwkv->fsm;
  iwrc rc = fsm->deallocate(fsm, blk->addr, 1ULL << blk->szpow);
  _kvblk_release(kbp);
  return rc;
}

IW_INLINE void _kvblk_peek_key(const KVBLK *kb,
                               uint8_t idx,
                               const uint8_t *mm,
                               uint8_t **obuf,
                               uint32_t *olen) {
  assert(idx < KVBLK_IDXNUM);
  if (kb->pidx[idx].len) {
    uint32_t klen, step;
    const uint8_t *rp = mm + kb->addr + (1ULL << kb->szpow) - kb->pidx[idx].off;
    IW_READVNUMBUF(rp, klen, step);
    assert(klen);
    rp += step;
    *obuf = (uint8_t *) rp;
    *olen = klen;
  } else {
    *obuf = 0;
    *olen = 0;
  }
}

IW_INLINE void _kvblk_peek_val(const KVBLK *kb,
                               uint8_t idx,
                               const uint8_t *mm,
                               uint8_t **obuf,
                               uint32_t *olen) {
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

static iwrc _kvblk_getkv(uint8_t *mm, KVBLK *kb, uint8_t idx, IWKV_val *key, IWKV_val *val) {
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

static iwrc _kvblk_at_mm(IWDB db, off_t addr, uint8_t *mm, KVBLK **blkp) {
  uint8_t *rp, *sp;
  uint16_t sv;
  int step;
  iwrc rc = 0;
  KVBLK *kb = calloc(1, sizeof(*kb));

  *blkp = 0;
  if (!kb) {
    return iwrc_set_errno(IW_ERROR_ALLOC, errno);
  }
  rp = mm + addr;
  kb->db = db;
  kb->addr = addr;
  kb->maxoff = 0;
  kb->zidx = -1;
  IW_READBV(rp, kb->szpow, kb->szpow);
  IW_READSV(rp, sv, kb->idxsz);
  if (IW_UNLIKELY(kb->idxsz > 2 * 4 * KVBLK_IDXNUM)) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  sp = rp;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    IW_READVNUMBUF(rp, kb->pidx[i].off, step);
    rp += step;
    IW_READVNUMBUF(rp, kb->pidx[i].len, step);
    rp += step;
    if (IW_UNLIKELY(rp - sp > kb->idxsz)) {
      rc = IWKV_ERROR_CORRUPTED;
      goto finish;
    }
    kb->pidx[i].ridx = i;
    if (kb->pidx[i].len) {
      if (IW_UNLIKELY(!kb->pidx[i].off)) {
        rc = IWKV_ERROR_CORRUPTED;
        goto finish;
      }
      if (kb->pidx[i].off > kb->maxoff) {
        kb->maxoff = kb->pidx[i].off;
      }
    } else if (kb->zidx == -1) {
      kb->zidx = i;
    }
  }
  *blkp = kb;
finish:
  if (rc) {
    _kvblk_release(&kb);
  }
  return rc;
}

IW_INLINE iwrc _kvblk_at(IWDB db, off_t addr, KVBLK **blkp) {
  iwrc rc;
  uint8_t *mm;
  IWFS_FSM *fsm = &db->iwkv->fsm;
  *blkp = 0;
  rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  rc = _kvblk_at_mm(db, addr, mm, blkp);
  IWRC(fsm->release_mmap(fsm), rc);
  return rc;
}

static void _kvblk_sync(KVBLK *kb, uint8_t *mm) {
  if (!(kb->flags & KVBLK_DURTY)) {
    return;
  }
  uint8_t *szp;
  uint16_t sp;
  uint8_t *wp = mm + kb->addr;
  memcpy(wp, &kb->szpow, 1);
  wp += 1;
  szp = wp;
  wp += sizeof(uint16_t);
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    KVP *kvp = &kb->pidx[i];
    IW_SETVNUMBUF(sp, wp, kvp->off);
    wp += sp;
    IW_SETVNUMBUF(sp, wp, kvp->len);
    wp += sp;
  }
  sp = wp - szp - sizeof(uint16_t);
  kb->idxsz = sp;
  sp = IW_HTOIS(sp);
  memcpy(szp, &sp, sizeof(uint16_t));
  kb->flags &= ~KVBLK_DURTY;
}

IW_INLINE off_t _kvblk_compacted_offset(KVBLK *kb) {
  off_t coff = 0;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    KVP *kvp = kb->pidx + i;
    coff += kvp->len;
  }
  return coff;
}

static int _kvblk_sort_kv(const void *v1, const void *v2) {
  uint32_t o1 = ((KVP *) v1)->off > 0 ? ((KVP *) v1)->off : -1UL;
  uint32_t o2 = ((KVP *) v2)->off > 0 ? ((KVP *) v2)->off : -1UL;
  return o1 > o2 ? 1 : o1 < o2 ? -1 : 0;
}

static void _kvblk_compact(KVBLK *kb, uint8_t *mm) {
  uint8_t i;
  off_t coff = _kvblk_compacted_offset(kb);
  if (coff == kb->maxoff) { // already compacted
    return;
  }
  KVP tidx[KVBLK_IDXNUM];
  uint8_t *wp = mm + kb->addr + (1ULL << kb->szpow);
  memcpy(tidx, kb->pidx, sizeof(tidx));
  qsort(tidx, KVBLK_IDXNUM, sizeof(KVP), _kvblk_sort_kv);
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
      memmove(wp - noff, wp - kvp->off, kvp->len);
      kvp->off = noff;
    }
    coff += kvp->len;
    kb->maxoff = coff;
  }
  for (i = 0; i < KVBLK_IDXNUM; ++i) {
    if (!kb->pidx[i].len)  {
      kb->zidx = i;
      break;
    }
  }
  if (i == KVBLK_IDXNUM) {
    kb->zidx = -1;
  }
  kb->flags |= KVBLK_DURTY;
  _kvblk_sync(kb, mm);
}

IW_INLINE uint64_t _kvblk_datasize(KVBLK *kb) {
  uint64_t dsz = KVBLK_HDRSZ + kb->idxsz;
  for (int i = 0; i < KVBLK_IDXNUM; ++i) {
    dsz += kb->pidx[i].len;
  }
  return dsz;
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

iwrc _kvblk_rmkv(KVBLK *kb, uint8_t idx, kvblk_rmkv_opts_t opts) {
  uint64_t sz;
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
  if (kb->zidx < 0 || idx < kb->zidx) {
    kb->zidx = idx;
  }
  kb->flags |= KVBLK_DURTY;
  if (!(RMKV_NO_RESIZE & opts)) {
    rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
    RCGO(rc, finish);
    uint64_t kbsz = 1ULL << kb->szpow;
    uint64_t dsz = _kvblk_datasize(kb);
    uint8_t dpow = 1;
    sz = kbsz / 2;
    while ((kb->szpow - dpow) > KVBLK_INISZPOW && dsz < sz / 2) {
      sz = sz / 2;
      dpow++;
    }
    if ((kb->szpow - dpow) >= KVBLK_INISZPOW && dsz < kbsz / 2) { // We can shrink kvblock
      _kvblk_compact(kb, mm);
      off_t naddr = kb->addr, nlen = kbsz;
      off_t maxoff = _kvblk_maxkvoff(kb);
      memmove(mm + kb->addr + sz - maxoff,
              mm + kb->addr + kbsz - maxoff,
              maxoff);
      fsm->release_mmap(fsm);
      mm = 0;
      rc = fsm->reallocate(fsm, sz, &naddr, &nlen, IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
      RCGO(rc, finish);
      kb->addr = naddr;
      kb->szpow = kb->szpow - dpow;
      opts |= RMKV_SYNC;
    }
  }
  if (RMKV_SYNC & opts) {
    if (!mm) {
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
    }
    _kvblk_sync(kb, mm);
  }
finish:
  if (mm) {
    fsm->release_mmap(fsm);
  }
  return rc;
}

static iwrc _kvblk_addkv(KVBLK *kb, const IWKV_val *key, const IWKV_val *val, int8_t *oidx) {
  iwrc rc = 0;
  off_t msz;    // max available free space
  off_t rsz;    // required size to add new key/value pair
  off_t noff;   // offset of new kvpair from end of block
  uint8_t *mm, *wp;
  size_t i, sp;
  KVP *kvp;
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  off_t psz = (key->size + val->size) + IW_VNUMSIZE(key->size); // required size
  bool compacted = false;
  *oidx = -1;

  if (psz > IWKV_MAX_KVSZ) {
    return IWKV_ERROR_MAXKVSZ;
  }
  if (kb->zidx < 0) {
    return _IWKV_ERROR_KVBLOCK_FULL;
  }

start:
  msz = (1ULL << kb->szpow) - KVBLK_HDRSZ - kb->idxsz - kb->maxoff;
  noff = kb->maxoff + psz;
  rsz = psz + IW_VNUMSIZE(noff) + IW_VNUMSIZE(psz) - 2;
  if (msz < rsz) { // not enough space
    if (!compacted) {
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
      _kvblk_compact(kb, mm);
      compacted = true;
      fsm->release_mmap(fsm);
      goto start;
    } else { // resize the whole block
      off_t nsz = (rsz - msz) + (1ULL << kb->szpow);
      uint8_t npow = kb->szpow;
      while ((1ULL << ++npow) < nsz);
      off_t naddr = kb->addr,
            nlen = (1ULL << kb->szpow);
      rc = fsm->reallocate(fsm, (1ULL << npow), &naddr, &nlen, IWFSM_ALLOC_NO_OVERALLOCATE | IWFSM_SOLID_ALLOCATED_SPACE);
      RCGO(rc, finish);
      assert(nlen == (1ULL << npow));
      // Move pairs area
      // [hdr..[pairs]] =reallocate=> [hdr..[pairs]_____] =memove=> [hdr.._____[pairs]]
      rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
      RCGO(rc, finish);
      memmove(mm + naddr + nlen - kb->maxoff, mm + naddr + (1ULL << kb->szpow) - kb->maxoff, kb->maxoff);
      fsm->release_mmap(fsm);
      kb->addr = naddr;
      kb->szpow = npow;
      goto start;
    }
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
  wp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  // [klen:vn,key,value]
  IW_SETVNUMBUF(sp, wp, key->size);
  wp += sp;
  memcpy(wp, key->data, key->size);
  wp += key->size;
  memcpy(wp, val->data, val->size);
  fsm->release_mmap(fsm);
finish:
  return rc;
}

static iwrc _kvblk_updatev(KVBLK *kb, int8_t *idxp, const IWKV_val *key, const IWKV_val *val) {
  assert(*idxp < KVBLK_IDXNUM);
  int32_t klen, i;
  size_t sz;
  int8_t idx = *idxp;
  uint8_t *mm, *wp, *sp;
  KVP *kvp = &kb->pidx[idx];
  IWFS_FSM *fsm = &kb->db->iwkv->fsm;
  size_t rsize = IW_VNUMSIZE(key->size) + key->size + val->size; // required size
  iwrc rc = fsm->acquire_mmap(fsm, 0, &mm, 0);
  RCRET(rc);
  wp = mm + kb->addr + (1ULL << kb->szpow) - kvp->off;
  sp = wp;
  IW_READVNUMBUF(wp, klen, sz);
  wp += sz;
  if (klen != key->size || memcmp(wp, key->data, key->size)) {
    rc = IWKV_ERROR_CORRUPTED;
    goto finish;
  }
  wp += klen;
  if (rsize <= kvp->len) {
    memcpy(wp, val->data, val->size);
    wp += val->size;
    if ((wp - sp) != kvp->len) {
      kvp->len = wp - sp;
      kb->flags |= KVBLK_DURTY;
    }
  } else {
    KVP tidx[KVBLK_IDXNUM];
    uint32_t koff = kb->pidx[idx].off;
    memcpy(tidx, kb->pidx, KVBLK_IDXNUM * sizeof(kb->pidx[0]));
    qsort(tidx, KVBLK_IDXNUM, sizeof(KVP), _kvblk_sort_kv);
    kb->flags |= KVBLK_DURTY;
    for (i = 0; i < KVBLK_IDXNUM; ++i) {
      if (tidx[i].off == koff) {
        if (koff - (i > 0 ? tidx[i - 1].off : 0) >= rsize) {
          memcpy(wp, val->data, val->size);
          wp += val->size;
          kvp->len = wp - sp;
        } else {
          fsm->release_mmap(fsm);
          mm = 0;
          rc = _kvblk_rmkv(kb, idx, RMKV_NO_RESIZE);
          RCGO(rc, finish);
          rc = _kvblk_addkv(kb, key, val, idxp);
        }
        break;
      }
    }
  }
finish:
  if (mm) {
    IWRC(fsm->release_mmap(fsm), rc);
  }
  return rc;
}

//--------------------------  SBLK

IW_INLINE void _sblk_release(SBLK **sblkp) {
  assert(sblkp && *sblkp);
  if (!((*sblkp)->flags & SBH_NO_LOCK)) {
    _aln_release((*sblkp)->kvblk->db, ADDR2BLK((*sblkp)->addr));
  }
  _kvblk_release(&(*sblkp)->kvblk);
  free(*sblkp);
  *sblkp = 0;
}

IW_INLINE iwrc _sblk_destroy(SBLK **sblkp) {
  assert(sblkp && *sblkp && (*sblkp)->kvblk && (*sblkp)->addr);
  iwrc rc;
  SBLK *sblk = *sblkp;
  IWFS_FSM *fsm = &sblk->kvblk->db->iwkv->fsm;
  off_t kvb_addr = sblk->kvblk->addr, sblk_addr = sblk->addr;
  uint8_t kvb_szpow = sblk->kvblk->szpow;
  _sblk_release(sblkp);
  rc = fsm->deallocate(fsm, sblk_addr, 1 << SBLK_SZPOW);
  IWRC(fsm->deallocate(fsm, kvb_addr, 1ULL << kvb_szpow), rc);
  return rc;
}

IW_INLINE void _sblk_sync(SBLK *sblk, uint8_t *mm) {
  assert(sblk && sblk->kvblk && sblk->addr);
  if (sblk->kvblk->flags & KVBLK_DURTY) {
    _kvblk_sync(sblk->kvblk, mm);
  }
}

static uint8_t _sblk_genlevel() {
  int8_t lvl;
  if (iwkv_next_level >= 0) {
    lvl = iwkv_next_level;
    iwkv_next_level = -1;
    return lvl;
  }
  uint32_t r = iwu_rand_u32();
  for (lvl = 0; lvl < SLEVELS && !(r & 1); ++lvl) {
    r >>= 1;
  }
  return IW_UNLIKELY(lvl >= SLEVELS) ? SLEVELS - 1 : lvl;
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
      return "Size of Key+value must be lesser than 0xfffffff bytes (IWKV_ERROR_MAXKVSZ)";
    case IWKV_ERROR_MAXDBSZ:
      return "Database file size reached its maximal limit: 0x3fffffffc0 bytes (IWKV_ERROR_MAXDBSZ)";
    case IWKV_ERROR_CORRUPTED:
      return "Database file invalid or corrupted (IWKV_ERROR_CORRUPTED)";
  }
  return 0;
}

iwrc iwkv_init(void) {
  static int _kv_initialized = 0;
  if (!__sync_bool_compare_and_swap(&_kv_initialized, 0, 1)) {
    return 0;  // initialized already
  }
  int64_t t;
  iwrc rc = iw_init();
  RCRET(rc);
  rc = iwp_current_time_ms(&t);
  RCRET(rc);
  iwu_rand_seed(t / 1000);
  return iwlog_register_ecodefn(_kv_ecodefn);
}
