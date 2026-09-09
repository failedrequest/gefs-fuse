/*
 * GEFS Storage Format Parser & Tree Engine (FreeBSD Port)
 *
 * Implements complete reading, B-tree lookup, directory iteration,
 * and data block access for GEFS filesystem images.
 */

#ifndef GEFS_STORAGE_H
#define GEFS_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Constants matching Plan 9 GEFS dat.h */
#define GEFS_LGBLK       14
#define GEFS_BLKSZ       (1ULL << GEFS_LGBLK) /* 16384 bytes */
#define GEFS_KEYMAX      256
#define GEFS_INLMAX      512
#define GEFS_PTRSZ       24                   /* off[8] hash[8] gen[8] */
#define GEFS_PPTRSZ      26                   /* off[8] hash[8] gen[8] fill[2] */
#define GEFS_OFFKSZ      17                   /* type[1] qid[8] off[8] */
#define GEFS_SNAPSZ      9                    /* tag[1] snapid[8] */
#define GEFS_TREESZ      (4+4+4+4+8+8+8+8+GEFS_PTRSZ) /* 64 bytes */
#define GEFS_KVMAX       (GEFS_KEYMAX + GEFS_INLMAX)
#define GEFS_KPMAX       (GEFS_KEYMAX + GEFS_PTRSZ)
#define GEFS_ARENASZ     (8+8+8+8)

#define GEFS_PIVHDSZ     10
#define GEFS_LEAFHDSZ    6
#define GEFS_LOGHDSZ     (2+2+8+GEFS_PTRSZ)
#define GEFS_ROOTSZ      (4+GEFS_PTRSZ)
#define GEFS_BUFSPC      ((GEFS_BLKSZ - GEFS_PIVHDSZ) / 2) /* 8187 */
#define GEFS_PIVSPC      (GEFS_BLKSZ - GEFS_PIVHDSZ - GEFS_BUFSPC) /* 8187 */
#define GEFS_LEAFSPC     (GEFS_BLKSZ - GEFS_LEAFHDSZ)     /* 16378 */
#define GEFS_MSGMAX      (1 + (GEFS_KVMAX > GEFS_KPMAX ? GEFS_KVMAX : GEFS_KPMAX))

/* Block types */
enum {
    GEFS_TDAT    = 0,
    GEFS_TPIVOT  = 1,
    GEFS_TLEAF   = 2,
    GEFS_TLOG    = 4,
    GEFS_TDLIST  = 5,
    GEFS_TARENA  = 6,
    GEFS_TSUPER  = 0x6765,
};

/* Key types */
enum {
    GEFS_KDAT    = 0,  /* qid[8] off[8] => pointer to data page */
    GEFS_KENT    = 1,  /* pqid[8] name[n] => serialized Xdir */
    GEFS_KUP     = 2,  /* qid[8] => parent dir Kent */
    GEFS_KLABEL  = 3,  /* name[] => snapid[] */
    GEFS_KSNAP   = 4,  /* sid[8] => Tree */
    GEFS_KDLIST  = 5,  /* snap[8] gen[8] => hd[ptr], tl[ptr] */
};

/* Message operations */
enum {
    GEFS_ONOP     = 0,
    GEFS_OINSERT  = 1,
    GEFS_ODELETE  = 2,
    GEFS_OCLEARB  = 3,
    GEFS_OCLOBBER = 4,
    GEFS_OWSTAT   = 5,
    GEFS_ORELINK  = 6,
    GEFS_OREPREV  = 7,
};

/* 9P QID types */
#define GEFS_QTDIR     0x80
#define GEFS_QTAPPEND  0x40
#define GEFS_QTEXCL    0x20
#define GEFS_QTAUTH    0x08
#define GEFS_QTTMP     0x04
#define GEFS_QTSYMLINK 0x02
#define GEFS_QTFILE    0x00

/* Plan 9 mode bits */
#define GEFS_DMDIR     0x80000000
#define GEFS_DMAPPEND  0x40000000
#define GEFS_DMEXCL    0x20000000
#define GEFS_DMAUTH    0x08000000
#define GEFS_DMTMP     0x04000000

/* Label flags */
#define GEFS_LMUT      (1 << 0)
#define GEFS_LAUTO     (1 << 1)
#define GEFS_LTSNAP    (1 << 2)

/* QID structure */
typedef struct {
    uint64_t path;
    uint32_t vers;
    uint8_t  type;
} GefsQid;

/* Block pointer (24 bytes on disk) */
typedef struct {
    int64_t  addr;
    uint64_t hash;
    int64_t  gen;
} GefsBptr;

#define GEFS_ZB ((GefsBptr){-1, (uint64_t)-1, -1})

/* Key */
typedef struct {
    char *k;
    int   nk;
} GefsKey;

/* Value */
typedef struct {
    short nv;
    char *v;
} GefsVal;

/* Key-Value pair */
typedef struct {
    char *k;
    int   nk;
    short nv;
    char *v;
} GefsKvp;

/* Message (with op) */
typedef struct {
    char  op;
    char *k;
    int   nk;
    short nv;
    char *v;
} GefsMsg;

/* Xdir - directory entry metadata */
typedef struct {
    uint64_t flag;
    GefsQid  qid;
    uint32_t mode;
    int64_t  atime; /* nanoseconds */
    int64_t  mtime; /* nanoseconds */
    uint64_t length;
    int32_t  uid;
    int32_t  gid;
    int32_t  muid;
    char    *name;
} GefsXdir;

/* Tree (snapshot tree metadata) */
typedef struct {
    int32_t  nref;
    int32_t  nlbl;
    int32_t  ht;
    uint32_t flag;
    GefsBptr bp;
    int64_t  gen;
    int64_t  pred;
    int64_t  succ;
    int64_t  base;
} GefsTree;

/* In-memory block cache entry */
typedef struct GefsBlk GefsBlk;
struct GefsBlk {
    GefsBlk *cnext;
    GefsBlk *cprev;
    GefsBlk *hnext;

    int16_t  type;
    int16_t  nval;
    int16_t  valsz;
    int16_t  nbuf;
    int16_t  bufsz;
    int16_t  logsz;

    GefsBptr bp;
    long     ref;
    char    *data;
    char     buf[GEFS_BLKSZ];
};

/* Block cache */
#define GEFS_NBUCKETS 1024
typedef struct {
    GefsBlk *chead;
    GefsBlk *ctail;
    GefsBlk *buckets[GEFS_NBUCKETS];
    size_t   count;
    size_t   max;
} GefsCache;

/* Scan path entry */
typedef struct {
    int      bi;
    int      vi;
    GefsBlk *b;
} GefsScanp;

/* Tree Scan state */
typedef struct {
    int64_t    offset;
    char       first;
    char       donescan;
    int        ht;
    GefsKvp    kv;
    GefsKey    pfx;
    char       kvbuf[GEFS_KVMAX];
    char       pfxbuf[GEFS_KEYMAX];
    GefsScanp *path;
} GefsScan;

/* Superblock state */
typedef struct {
    int      blksz;
    int      bufspc;
    int      narena;
    GefsTree snap;
    uint64_t flag;
    uint64_t nextqid;
    uint64_t nextgen;
    uint64_t qgen;
    GefsBptr *arenabp;
} GefsSuper;

/* Free range in arena allocator */
typedef struct GefsRange GefsRange;
struct GefsRange {
    int64_t    off;
    int64_t    len;
    GefsRange *next;
};

/* In-memory Arena state */
typedef struct {
    int64_t    hdaddr;
    int64_t    size;
    int64_t    used;
    GefsBptr   loghd;
    GefsRange *free_list;
} GefsArena;

/* Top-level filesystem context */
typedef struct {
    int        fd;
    GefsSuper  sb;
    GefsCache  cache;
    GefsTree   snap_tree;  /* Root snapshot tree */
    GefsTree   active_tree;/* Active snapshot tree ("main") */
    char       active_snap_name[64];
    GefsArena *arenas;
    int        dirty;
    int        readonly;
} GefsCtx;

/* Byte packing/unpacking macros */
#define UNPACK8(p)   (((uint8_t*)(p))[0])
#define UNPACK16(p)  ((uint16_t)((((uint8_t*)(p))[0]<<8)|(((uint8_t*)(p))[1])))
#define UNPACK32(p)  ((uint32_t)((((uint8_t*)(p))[0]<<24)|(((uint8_t*)(p))[1]<<16)|\
                     (((uint8_t*)(p))[2]<<8)|(((uint8_t*)(p))[3])))
#define UNPACK64(p)  (((uint64_t)UNPACK32(p))<<32 | (uint64_t)UNPACK32((p)+4))

#define PACK8(p,v)   do{(p)[0]=(uint8_t)(v);}while(0)
#define PACK16(p,v)  do{(p)[0]=(uint8_t)((v)>>8);(p)[1]=(uint8_t)(v);}while(0)
#define PACK32(p,v)  do{(p)[0]=(uint8_t)((v)>>24);(p)[1]=(uint8_t)((v)>>16);\
                        (p)[2]=(uint8_t)((v)>>8);(p)[3]=(uint8_t)(v);}while(0)
#define PACK64(p,v)  do{uint64_t _v=(uint64_t)(v);\
                        (p)[0]=(uint8_t)(_v>>56);(p)[1]=(uint8_t)(_v>>48);\
                        (p)[2]=(uint8_t)(_v>>40);(p)[3]=(uint8_t)(_v>>32);\
                        (p)[4]=(uint8_t)(_v>>24);(p)[5]=(uint8_t)(_v>>16);\
                        (p)[6]=(uint8_t)(_v>>8); (p)[7]=(uint8_t)(_v);}while(0)

/* Hash functions */
uint64_t metrohash64(const void *key, uint64_t len, uint32_t seed);
uint64_t gefs_bufhash(const void *src, size_t len);

/* Serialization functions */
char*    gefs_unpackstr(char *p, char *e, char **s);
char*    gefs_packstr(char *p, char *e, const char *s);
char*    gefs_packdkey(char *p, int sz, int64_t up, const char *name);
char*    gefs_unpackdkey(char *p, int sz, int64_t *up);
char*    gefs_packdval(char *p, int sz, const GefsXdir *d);
void     gefs_kv2dir(const GefsKvp *kv, GefsXdir *d);
char*    gefs_packlbl(char *p, int sz, const char *name);
char*    gefs_packsnap(char *p, int sz, int64_t id);
char*    gefs_packbp(char *p, int sz, const GefsBptr *bp);
GefsBptr gefs_unpackbp(const char *p, int sz);
GefsTree* gefs_unpacktree(GefsTree *t, const char *p, int sz);
int      gefs_unpacksb(GefsSuper *fi, const char *p0, int sz);

/* Key comparison */
int      gefs_keycmp(const GefsKey *a, const GefsKey *b);
void     gefs_cpkvp(GefsKvp *dst, const GefsKvp *src, char *buf, int nbuf);

/* Block cache */
void     gefs_cache_init(GefsCtx *ctx, size_t max_blocks);
GefsBlk* gefs_getblk(GefsCtx *ctx, GefsBptr bp);
GefsBlk* gefs_holdblk(GefsBlk *b);
void     gefs_dropblk(GefsCtx *ctx, GefsBlk *b);
void     gefs_cache_free(GefsCtx *ctx);

/* Block value access */
void     gefs_getval(GefsBlk *b, int i, GefsKvp *kv);
void     gefs_getmsg(GefsBlk *b, int i, GefsMsg *m);

/* B-Tree operations */
int      gefs_btlookup(GefsCtx *ctx, GefsTree *t, GefsKey *k, GefsKvp *r, char *buf, int nbuf);
void     gefs_btnewscan(GefsScan *s, const char *pfx, int npfx);
void     gefs_btenter(GefsCtx *ctx, GefsTree *t, GefsScan *s);
int      gefs_btnext(GefsCtx *ctx, GefsScan *s, GefsKvp *r);
void     gefs_btexit(GefsCtx *ctx, GefsScan *s);

/* High-level filesystem operations */
int      gefs_fs_open(GefsCtx *ctx, const char *path);
void     gefs_fs_close(GefsCtx *ctx);
int      gefs_opensnap(GefsCtx *ctx, const char *label, GefsTree *t, int *flg);
int      gefs_walk_path(GefsCtx *ctx, GefsTree *t, const char *path, GefsXdir *d);
int      gefs_walk1(GefsCtx *ctx, GefsTree *t, int64_t pqid, const char *name, GefsXdir *d);
ssize_t  gefs_read_file(GefsCtx *ctx, GefsTree *t, const GefsXdir *d, char *buf, size_t size, off_t offset);
int      gefs_list_dir(GefsCtx *ctx, GefsTree *t, int64_t qid_path,
                       int (*callback)(const GefsXdir *d, void *arg), void *arg);

/* Mutation & Write Support */
int64_t  gefs_alloc_blk(GefsCtx *ctx, int type);
int      gefs_btupsert(GefsCtx *ctx, GefsTree *t, GefsMsg *msgs, int nmsgs);
int      gefs_sync_fs(GefsCtx *ctx);
int      gefs_create_file(GefsCtx *ctx, const char *path, mode_t mode, GefsXdir *out_dir);
int      gefs_create_dir(GefsCtx *ctx, const char *path, mode_t mode);
int      gefs_create_symlink(GefsCtx *ctx, const char *target, const char *linkpath);
ssize_t  gefs_write_file(GefsCtx *ctx, const char *path, const char *buf, size_t size, off_t offset);
int      gefs_truncate_file(GefsCtx *ctx, const char *path, off_t new_size);
int      gefs_unlink_entry(GefsCtx *ctx, const char *path);
int      gefs_rmdir_entry(GefsCtx *ctx, const char *path);
int      gefs_rename_entry(GefsCtx *ctx, const char *oldpath, const char *newpath);
int      gefs_chmod_entry(GefsCtx *ctx, const char *path, mode_t mode);
int      gefs_chown_entry(GefsCtx *ctx, const char *path, uid_t uid, gid_t gid);
int      gefs_utimens_entry(GefsCtx *ctx, const char *path, const struct timespec tv[2]);

/* Snapshot management */
int      gefs_create_snapshot(GefsCtx *ctx, const char *snap_name, int mutable_flag);
int      gefs_delete_snapshot(GefsCtx *ctx, const char *snap_name);

/* Convert GefsXdir mode to POSIX mode_t */
mode_t   gefs_mode_to_posix(uint32_t gefs_mode, uint8_t qid_type);

#endif /* GEFS_STORAGE_H */
