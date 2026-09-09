/*
 * mkfs.gefs / gefs-ream - GEFS Filesystem Formatter (FreeBSD Port)
 *
 * Formats a file or disk device with a fresh GEFS filesystem structure.
 * Creates superblocks, arenas with free logs, root snapshot tree,
 * administrative users table, and empty initial root directory.
 */

#include "gefs_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

enum {
    Qmainroot = 0,
    Qadmroot  = 1,
    Qadmuser  = 2,
    Nreamqid  = 3,
};

/* Log operation constants from dat.h */
#define LOG_FREE   0
#define LOG_ALLOC  1
#define LOG_FREE1  2
#define LOG_ALLOC1 3
#define LOG_SYNC   4

#define LOG_HDSZ   (2 + 2 + 8 + GEFS_PTRSZ) /* 36 bytes */

static void
fillxdir(GefsXdir *d, int64_t qid, const char *name, int type, int mode)
{
    memset(d, 0, sizeof(GefsXdir));
    d->qid.path = qid;
    d->qid.vers = 0;
    d->qid.type = type;
    d->mode = mode;
    d->atime = 0;
    d->mtime = 0;
    d->length = 0;
    d->name = (char *)name;
    d->uid = -1;
    d->gid = -1;
    d->muid = 0;
}

/* Helper to set a key-value pair in a block */
static void
setval(GefsBlk *b, GefsKvp *kv)
{
    int off, spc;
    char *p;

    spc = (b->type == GEFS_TLEAF) ? GEFS_LEAFSPC : GEFS_PIVSPC;
    b->valsz += 2 + kv->nk + 2 + kv->nv;
    off = spc - b->valsz;

    p = b->data + 2 * b->nval;
    PACK16(p, off);

    p = b->data + off;
    PACK16(p, kv->nk);          p += 2;
    memmove(p, kv->k, kv->nk);  p += kv->nk;
    PACK16(p, kv->nv);          p += 2;
    memmove(p, kv->v, kv->nv);

    b->nval++;
}

static void
finalize_blk(GefsBlk *b)
{
    if (b->type != GEFS_TDAT)
        PACK16(b->buf, b->type);

    switch (b->type) {
    case GEFS_TPIVOT:
        PACK16(b->buf + 2, b->nval);
        PACK16(b->buf + 4, b->valsz);
        PACK16(b->buf + 6, b->nbuf);
        PACK16(b->buf + 8, b->bufsz);
        break;
    case GEFS_TLEAF:
        PACK16(b->buf + 2, b->nval);
        PACK16(b->buf + 4, b->valsz);
        break;
    case GEFS_TDLIST:
    case GEFS_TLOG: {
        uint64_t logh = gefs_bufhash(b->data, b->logsz);
        PACK16(b->buf + 2, b->logsz);
        PACK64(b->buf + 4, logh);
        gefs_packbp(b->buf + 12, GEFS_PTRSZ, &b->bp);
        break;
    }
    case GEFS_TDAT:
    case GEFS_TARENA:
    case GEFS_TSUPER:
        break;
    }

    if (b->type == GEFS_TLOG || b->type == GEFS_TDLIST) {
        b->bp.hash = gefs_bufhash(b->data, b->logsz);
    } else {
        b->bp.hash = gefs_bufhash(b->buf, GEFS_BLKSZ);
    }
}

static int
write_blk(int fd, GefsBlk *b)
{
    finalize_blk(b);
    ssize_t n = pwrite(fd, b->buf, GEFS_BLKSZ, b->bp.addr);
    if (n != (ssize_t)GEFS_BLKSZ)
        return -1;
    return 0;
}

static void
dir2kv(int64_t upqid, const GefsXdir *d, GefsKvp *kv, char *kbuf, char *vbuf, int vbufsz)
{
    char *p = gefs_packdkey(kbuf, GEFS_KEYMAX, upqid, d->name);
    kv->k = kbuf;
    kv->nk = p - kbuf;
    p = gefs_packdval(vbuf, vbufsz, d);
    kv->v = vbuf;
    kv->nv = p - vbuf;
}

static void
initroot(GefsBlk *r)
{
    char *p, kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX], kbuf2[GEFS_KEYMAX], vbuf2[GEFS_INLMAX];
    GefsKvp kv;
    GefsXdir d;

    /* Entry 1: Root directory entry */
    fillxdir(&d, Qmainroot, "", GEFS_QTDIR, GEFS_DMDIR | 0775);
    dir2kv(-1, &d, &kv, kbuf, vbuf, sizeof(vbuf));
    setval(r, &kv);

    /* Entry 2: Parent link Kent{-1, ""} */
    p = gefs_packdkey(kbuf2, sizeof(kbuf2), Qmainroot, NULL);
    kbuf2[0] = GEFS_KUP; /* Kup */
    kv.k = kbuf2;
    kv.nk = p - kbuf2;
    p = gefs_packdkey(vbuf2, sizeof(vbuf2), -1, "");
    kv.v = vbuf2;
    kv.nv = p - vbuf2;
    setval(r, &kv);
}

static void
initadm(GefsBlk *r, GefsBlk *u, int nu)
{
    char *p, kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX], kbuf2[GEFS_KEYMAX], vbuf2[GEFS_INLMAX];
    char kbuf3[GEFS_KEYMAX], vbuf3[GEFS_INLMAX], kbuf4[GEFS_KEYMAX], vbuf4[GEFS_INLMAX];
    GefsKvp kv;
    GefsXdir d;

    /* 1. Kdat for users file: [Kdat:1][Qadmuser:8][off:8=0] => pointer to u */
    kbuf[0] = GEFS_KDAT;
    PACK64(kbuf + 1, (uint64_t)Qadmuser);
    PACK64(kbuf + 9, 0ULL);
    kv.k = kbuf;
    kv.nk = GEFS_OFFKSZ;
    gefs_packbp(vbuf, sizeof(vbuf), &u->bp);
    kv.v = vbuf;
    kv.nv = GEFS_PTRSZ;
    setval(r, &kv);

    /* 2. Kent for users file inside /adm */
    fillxdir(&d, Qadmuser, "users", GEFS_QTFILE, 0664);
    d.length = nu;
    dir2kv(Qadmroot, &d, &kv, kbuf2, vbuf2, sizeof(vbuf2));
    setval(r, &kv);

    /* 3. Kent for /adm root directory */
    fillxdir(&d, Qadmroot, "", GEFS_QTDIR, GEFS_DMDIR | 0775);
    dir2kv(-1, &d, &kv, kbuf3, vbuf3, sizeof(vbuf3));
    setval(r, &kv);

    /* 4. Kup for /adm */
    p = gefs_packdkey(kbuf4, sizeof(kbuf4), Qadmroot, NULL);
    kbuf4[0] = GEFS_KUP;
    kv.k = kbuf4;
    kv.nk = p - kbuf4;
    p = gefs_packdkey(vbuf4, sizeof(vbuf4), -1, "");
    kv.v = vbuf4;
    kv.nv = p - vbuf4;
    setval(r, &kv);
}

static void
lbl2kv(const char *name, int64_t id, uint32_t flags, GefsKvp *kv, char *kbuf, char *vbuf)
{
    char *p = gefs_packlbl(kbuf, GEFS_KEYMAX, name);
    kv->k = kbuf;
    kv->nk = p - kbuf;

    vbuf[0] = GEFS_KSNAP;
    PACK64(vbuf + 1, id);
    PACK32(vbuf + 9, flags);
    kv->v = vbuf;
    kv->nv = 1 + 8 + 4;
}

static char*
packtree(char *p, int sz, const GefsTree *t)
{
    if (sz < GEFS_TREESZ) return NULL;
    PACK32(p, t->nref);    p += 4;
    PACK32(p, t->nlbl);    p += 4;
    PACK32(p, t->ht);      p += 4;
    PACK32(p, t->flag);    p += 4;
    PACK64(p, t->gen);     p += 8;
    PACK64(p, t->pred);    p += 8;
    PACK64(p, t->succ);    p += 8;
    PACK64(p, t->base);    p += 8;
    gefs_packbp(p, GEFS_PTRSZ, &t->bp); p += GEFS_PTRSZ;
    return p;
}

static void
initsnap(GefsBlk *s, GefsBlk *r, GefsBlk *a, uint64_t *nextgen)
{
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    char kbuf2[GEFS_KEYMAX], vbuf2[GEFS_INLMAX];
    char kbuf3[GEFS_KEYMAX], vbuf3[GEFS_INLMAX];
    char kbuf4[GEFS_KEYMAX], vbuf4[GEFS_INLMAX];
    char kbuf5[GEFS_KEYMAX], vbuf5[GEFS_INLMAX];
    char kbuf6[GEFS_KEYMAX], vbuf6[GEFS_INLMAX];
    GefsTree t;
    GefsKvp kv;
    char *p;

    /* Labels must be sorted: "adm", "empty", "main" */
    lbl2kv("adm", 1, GEFS_LMUT | GEFS_LTSNAP, &kv, kbuf, vbuf);
    setval(s, &kv);
    lbl2kv("empty", 0, 0, &kv, kbuf2, vbuf2);
    setval(s, &kv);
    lbl2kv("main", 2, GEFS_LMUT | GEFS_LTSNAP, &kv, kbuf3, vbuf3);
    setval(s, &kv);

    /* Snap 0: empty */
    p = gefs_packsnap(kbuf4, sizeof(kbuf4), 0);
    kv.k = kbuf4;
    kv.nk = p - kbuf4;
    memset(&t, 0, sizeof(GefsTree));
    t.nref = 2;
    t.nlbl = 1;
    t.ht = 1;
    t.gen = (*nextgen)++;
    t.pred = 0;
    t.succ = 2;
    t.bp = r->bp;
    p = packtree(vbuf4, sizeof(vbuf4), &t);
    kv.v = vbuf4;
    kv.nv = p - vbuf4;
    setval(s, &kv);

    /* Snap 1: adm */
    p = gefs_packsnap(kbuf5, sizeof(kbuf5), 1);
    kv.k = kbuf5;
    kv.nk = p - kbuf5;
    memset(&t, 0, sizeof(GefsTree));
    t.nref = 0;
    t.nlbl = 1;
    t.ht = 1;
    t.gen = (*nextgen)++;
    t.pred = 0;
    t.succ = -1;
    t.bp = a->bp;
    p = packtree(vbuf5, sizeof(vbuf5), &t);
    kv.v = vbuf5;
    kv.nv = p - vbuf5;
    setval(s, &kv);

    /* Snap 2: main */
    p = gefs_packsnap(kbuf6, sizeof(kbuf6), 2);
    kv.k = kbuf6;
    kv.nk = p - kbuf6;
    memset(&t, 0, sizeof(GefsTree));
    t.nref = 0;
    t.nlbl = 1;
    t.ht = 1;
    t.gen = (*nextgen)++;
    t.pred = 0;
    t.succ = -1;
    t.bp = r->bp;
    p = packtree(vbuf6, sizeof(vbuf6), &t);
    kv.v = vbuf6;
    kv.nv = p - vbuf6;
    setval(s, &kv);
}

static char*
packarena(char *p, int sz, int64_t size, int64_t used, const GefsBptr *loghd)
{
    if (sz < GEFS_ARENASZ + GEFS_PTRSZ) return NULL;
    PACK64(p, size);   p += 8;
    PACK64(p, used);   p += 8;
    PACK64(p, 0);      p += 8; /* reserve */
    PACK64(p, 0);      p += 8; /* flags */
    gefs_packbp(p, GEFS_PTRSZ, loghd); p += GEFS_PTRSZ;
    return p;
}

static char*
packsb(char *p0, int sz, const GefsSuper *sb)
{
    char *p = p0;
    int i;

    if (sz < (int)GEFS_BLKSZ) return NULL;
    memset(p0, 0, GEFS_BLKSZ);

    memcpy(p, "gefs9.00", 8); p += 8;
    PACK32(p, sb->blksz);      p += 4;
    PACK32(p, sb->bufspc);     p += 4;
    PACK32(p, sb->narena);     p += 4;
    PACK32(p, sb->snap.ht);    p += 4;
    PACK64(p, sb->snap.bp.addr); p += 8;
    PACK64(p, sb->snap.bp.hash); p += 8;

    /* snapdl.hd and snapdl.tl */
    PACK64(p, -1LL); p += 8;
    PACK64(p, -1ULL); p += 8;
    PACK64(p, -1LL); p += 8;
    PACK64(p, -1ULL); p += 8;

    PACK64(p, sb->flag);       p += 8;
    PACK64(p, sb->nextqid);    p += 8;
    PACK64(p, sb->nextgen);    p += 8;
    PACK64(p, sb->qgen);       p += 8;

    for (i = 0; i < sb->narena; i++) {
        PACK64(p, sb->arenabp[i].addr); p += 8;
        PACK64(p, sb->arenabp[i].hash); p += 8;
    }

    uint64_t xh = gefs_bufhash(p0, p - p0);
    PACK64(p, xh); p += 8;

    return p;
}

int
main(int argc, char *argv[])
{
    const char *dev = NULL;
    const char *username = "gefs";
    int fd;
    struct stat st;
    int64_t sz, asz, off;
    int narena, i;
    GefsSuper sb;

    printf("GEFS Filesystem Formatter (FreeBSD)\n");
    printf("===================================\n\n");

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image-file-or-device> [username]\n", argv[0]);
        return 1;
    }

    dev = argv[1];
    if (argc >= 3)
        username = argv[2];

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open '%s': %s\n", dev, strerror(errno));
        return 1;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "Error: fstat failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    sz = st.st_size;
    if (sz < 128 * 1024 * 1024 + (int64_t)GEFS_BLKSZ) {
        fprintf(stderr, "Error: Image '%s' too small (%ld bytes, minimum 128MB)\n", dev, (long)sz);
        close(fd);
        return 1;
    }

    sz = sz - (sz % GEFS_BLKSZ) - 2 * GEFS_BLKSZ;
    narena = (sz + 4096ULL * 1024 * 1024 * 1024 - 1) / (4096ULL * 1024 * 1024 * 1024);
    if (narena < 8) narena = 8;
    if (narena > 32) narena = 32;

    printf("Formatting '%s':\n", dev);
    printf("  Total usable size: %ld MB\n", (long)(sz / (1024 * 1024)));
    printf("  Block size:        %u bytes\n", (unsigned)GEFS_BLKSZ);
    printf("  Arenas:            %d\n", narena);

    off = GEFS_BLKSZ;
    asz = sz / narena;
    asz = asz - (asz % GEFS_BLKSZ) - 2 * GEFS_BLKSZ;

    GefsBptr *arenabp = calloc(narena, sizeof(GefsBptr));
    if (!arenabp) {
        close(fd);
        return 1;
    }

    /* Initialize each arena */
    for (i = 0; i < narena; i++) {
        int64_t hdaddr = off;
        int64_t logaddr = hdaddr + 2 * GEFS_BLKSZ;

        /* Write initial log block */
        GefsBlk logblk;
        memset(&logblk, 0, sizeof(logblk));
        logblk.type = GEFS_TLOG;
        logblk.bp.addr = logaddr;
        logblk.bp.gen = -1;
        logblk.data = logblk.buf + LOG_HDSZ;

        char *p = logblk.data;
        PACK64(p, logaddr | LOG_FREE);     p += 8;
        PACK64(p, asz - 2 * GEFS_BLKSZ);   p += 8;
        PACK64(p, logaddr | LOG_ALLOC);    p += 8;
        PACK64(p, GEFS_BLKSZ);             p += 8;
        PACK64(p, (uint64_t)LOG_SYNC);     p += 8;
        logblk.logsz = p - logblk.data;

        write_blk(fd, &logblk);

        /* Write arena header 0 & 1 */
        GefsBlk h0, h1;
        memset(&h0, 0, sizeof(h0));
        h0.type = GEFS_TARENA;
        h0.bp.addr = hdaddr;
        h0.data = h0.buf + 2;
        packarena(h0.data, GEFS_BLKSZ - 2, asz, GEFS_BLKSZ, &logblk.bp);
        write_blk(fd, &h0);

        memset(&h1, 0, sizeof(h1));
        h1.type = GEFS_TARENA;
        h1.bp.addr = hdaddr + GEFS_BLKSZ;
        h1.data = h1.buf + 2;
        packarena(h1.data, GEFS_BLKSZ - 2, asz, GEFS_BLKSZ, &logblk.bp);
        write_blk(fd, &h1);

        arenabp[i] = h0.bp;
        off += asz + 2 * GEFS_BLKSZ;
    }

    /* Allocate blocks for root, adm, users, snap */
    int64_t mb_addr = GEFS_BLKSZ + 3 * GEFS_BLKSZ;
    int64_t ab_addr = GEFS_BLKSZ + 4 * GEFS_BLKSZ;
    int64_t ub_addr = GEFS_BLKSZ + 5 * GEFS_BLKSZ;
    int64_t tb_addr = GEFS_BLKSZ + 6 * GEFS_BLKSZ;

    uint64_t nextgen = 1;

    /* 1. Main root directory leaf block */
    GefsBlk mb;
    memset(&mb, 0, sizeof(mb));
    mb.type = GEFS_TLEAF;
    mb.bp.addr = mb_addr;
    mb.data = mb.buf + GEFS_LEAFHDSZ;
    initroot(&mb);
    write_blk(fd, &mb);

    /* 2. Admin users data block */
    GefsBlk ub;
    memset(&ub, 0, sizeof(ub));
    ub.type = GEFS_TDAT;
    ub.bp.addr = ub_addr;
    ub.data = ub.buf;
    char utab[256];
    snprintf(utab, sizeof(utab),
             "-1:adm::%s\n"
             "0:none::\n"
             "1:%s:%s:\n",
             username, username, username);
    int nu = strlen(utab);
    memcpy(ub.data, utab, nu);
    write_blk(fd, &ub);

    /* 3. Admin root directory leaf block */
    GefsBlk ab;
    memset(&ab, 0, sizeof(ab));
    ab.type = GEFS_TLEAF;
    ab.bp.addr = ab_addr;
    ab.data = ab.buf + GEFS_LEAFHDSZ;
    initadm(&ab, &ub, nu);
    write_blk(fd, &ab);

    /* 4. Snapshots root leaf block */
    GefsBlk tb;
    memset(&tb, 0, sizeof(tb));
    tb.type = GEFS_TLEAF;
    tb.bp.addr = tb_addr;
    tb.data = tb.buf + GEFS_LEAFHDSZ;
    initsnap(&tb, &mb, &ab, &nextgen);
    write_blk(fd, &tb);

    /* Setup Superblock */
    memset(&sb, 0, sizeof(sb));
    sb.blksz = GEFS_BLKSZ;
    sb.bufspc = GEFS_BUFSPC;
    sb.narena = narena;
    sb.snap.ht = 1;
    sb.snap.bp = tb.bp;
    sb.flag = 0;
    sb.nextqid = Nreamqid;
    sb.nextgen = nextgen;
    sb.qgen = 0;
    sb.arenabp = arenabp;

    /* Write Superblock 0 (at offset 0) and Superblock 1 (at end) */
    GefsBlk sb0, sb1;
    memset(&sb0, 0, sizeof(sb0));
    sb0.type = GEFS_TSUPER;
    sb0.bp.addr = 0;
    packsb(sb0.buf, GEFS_BLKSZ, &sb);
    write_blk(fd, &sb0);

    memset(&sb1, 0, sizeof(sb1));
    sb1.type = GEFS_TSUPER;
    sb1.bp.addr = sz + GEFS_BLKSZ;
    packsb(sb1.buf, GEFS_BLKSZ, &sb);
    write_blk(fd, &sb1);

    free(arenabp);
    close(fd);

    printf("✓ Successfully formatted GEFS filesystem on '%s'\n", dev);
    printf("  Default user: %s\n", username);
    printf("  Snapshots initialized: 'main', 'adm', 'empty'\n\n");

    return 0;
}
