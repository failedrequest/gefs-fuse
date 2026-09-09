/*
 * GEFS Storage Engine Implementation (FreeBSD Port)
 *
 * Implements block caching, B-tree lookup and range scans,
 * directory traversal, file reading, and serialization for GEFS.
 */

#include "gefs_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

/* =========================================================================
 * MetroHash64 (MIT Licensed)
 * ========================================================================= */

#define ROTATE(x, b) (((x) << (b)) | ((x) >> (64 - (b))))

#define HALF_ROUND(a, b, c, d, s, t) \
    a += b; c += d; \
    b = ROTATE(b, s) ^ a; \
    d = ROTATE(d, t) ^ c; \
    a = ROTATE(a, 32);

#define DOUBLE_ROUND(v0, v1, v2, v3) \
    HALF_ROUND(v0, v1, v2, v3, 13, 16); \
    HALF_ROUND(v2, v1, v0, v3, 17, 21); \
    HALF_ROUND(v0, v1, v2, v3, 13, 16); \
    HALF_ROUND(v2, v1, v0, v3, 17, 21);

#define read_u64(ptr) (*(const uint64_t*)(const void*)(ptr))
#define read_u32(ptr) (*(const uint32_t*)(const void*)(ptr))
#define read_u16(ptr) (*(const uint16_t*)(const void*)(ptr))
#define read_u8(ptr)  (*(const uint8_t*)(const void*)(ptr))

uint64_t
metrohash64(const void *key, uint64_t len, uint32_t seed)
{
    static const uint64_t k0 = 0xC83A91E1ULL;
    static const uint64_t k1 = 0x8648DBDBULL;
    static const uint64_t k2 = 0x7BDEC03BULL;
    static const uint64_t k3 = 0x2F5870A5ULL;

    const uint8_t *ptr = (const uint8_t *)key;
    const uint8_t *const end = ptr + len;

    uint64_t hash = ((((uint64_t)seed) + k2) * k0) + len;

    if (len >= 32) {
        uint64_t v[4];
        v[0] = hash;
        v[1] = hash;
        v[2] = hash;
        v[3] = hash;

        do {
            v[0] += read_u64(ptr) * k0; ptr += 8; v[0] = ROTATE(v[0], 29) + v[2];
            v[1] += read_u64(ptr) * k1; ptr += 8; v[1] = ROTATE(v[1], 29) + v[3];
            v[2] += read_u64(ptr) * k2; ptr += 8; v[2] = ROTATE(v[2], 29) + v[0];
            v[3] += read_u64(ptr) * k3; ptr += 8; v[3] = ROTATE(v[3], 29) + v[1];
        } while (ptr <= end - 32);

        v[2] ^= ROTATE(((v[0] + v[3]) * k0) + v[1], 37) * k1;
        v[3] ^= ROTATE(((v[1] + v[2]) * k1) + v[0], 37) * k0;
        v[0] ^= ROTATE(((v[0] + v[2]) * k0) + v[3], 37) * k1;
        v[1] ^= ROTATE(((v[1] + v[3]) * k1) + v[2], 37) * k0;
        hash += v[0] ^ v[1];
    }

    if ((end - ptr) >= 16) {
        uint64_t v0 = hash + (read_u64(ptr) * k2); ptr += 8; v0 = ROTATE(v0, 29) * k3;
        uint64_t v1 = hash + (read_u64(ptr) * k2); ptr += 8; v1 = ROTATE(v1, 29) * k3;
        v0 ^= ROTATE(v0 * k2, 37) * k3;
        hash += v0 ^ v1;
    }

    if ((end - ptr) >= 8) {
        hash += read_u64(ptr) * k3; ptr += 8;
        hash ^= ROTATE(hash, 55) * k1;
    }

    if ((end - ptr) >= 4) {
        hash += read_u32(ptr) * k3; ptr += 4;
        hash ^= ROTATE(hash, 26) * k1;
    }

    if ((end - ptr) >= 2) {
        hash += read_u16(ptr) * k3; ptr += 2;
        hash ^= ROTATE(hash, 48) * k1;
    }

    if ((end - ptr) >= 1) {
        hash += read_u8(ptr) * k3;
        hash ^= ROTATE(hash, 37) * k1;
    }

    hash ^= ROTATE(hash, 28);
    hash *= k0;
    hash ^= ROTATE(hash, 29);

    return hash;
}

uint64_t
gefs_bufhash(const void *src, size_t len)
{
    return metrohash64(src, len, 0x6765);
}

/* =========================================================================
 * Serialization Functions
 * ========================================================================= */

char*
gefs_unpackstr(char *p, char *e, char **s)
{
    int n;
    if (e - p < 3) return NULL;
    n = UNPACK16(p);
    if (e - p < n + 3 || p[n + 2] != 0) return NULL;
    *s = p + 2;
    return p + 3 + n;
}

char*
gefs_packstr(char *p, char *e, const char *s)
{
    int n = strlen(s);
    if (e - p < n + 3) return NULL;
    PACK16(p, n);      p += 2;
    memmove(p, s, n);  p += n;
    *p = 0;            p += 1;
    return p;
}

char*
gefs_packdkey(char *p, int sz, int64_t up, const char *name)
{
    char *ep = p + sz;
    PACK8(p, GEFS_KENT);  p += 1;
    PACK64(p, up);        p += 8;
    if (name != NULL)
        p = gefs_packstr(p, ep, name);
    return p;
}

char*
gefs_unpackdkey(char *p, int sz, int64_t *up)
{
    char key, *ep, *name;
    ep = p + sz;
    if (sz < 9) return NULL;
    key = UNPACK8(p);  p += 1;
    *up = UNPACK64(p); p += 8;
    if (key != GEFS_KENT) return NULL;
    p = gefs_unpackstr(p, ep, &name);
    return name;
}

char*
gefs_packdval(char *p, int sz, const GefsXdir *d)
{
    char *e = p + sz;
    PACK64(p, d->flag);      p += 8;
    PACK64(p, d->qid.path);  p += 8;
    PACK32(p, d->qid.vers);  p += 4;
    PACK8(p, d->qid.type);   p += 1;
    PACK32(p, d->mode);      p += 4;
    PACK64(p, d->atime);     p += 8;
    PACK64(p, d->mtime);     p += 8;
    PACK64(p, d->length);    p += 8;
    PACK32(p, d->uid);       p += 4;
    PACK32(p, d->gid);       p += 4;
    PACK32(p, d->muid);      p += 4;
    if (p > e) return NULL;
    return p;
}

void
gefs_kv2dir(const GefsKvp *kv, GefsXdir *d)
{
    char *k, *ek, *v, *ev;

    memset(d, 0, sizeof(GefsXdir));
    k = kv->k + 9;
    ek = kv->k + kv->nk;
    k = gefs_unpackstr(k, ek, &d->name);

    v = kv->v;
    ev = v + kv->nv;
    d->flag     = UNPACK64(v); v += 8;
    d->qid.path = UNPACK64(v); v += 8;
    d->qid.vers = UNPACK32(v); v += 4;
    d->qid.type = UNPACK8(v);  v += 1;
    d->mode     = UNPACK32(v); v += 4;
    d->atime    = UNPACK64(v); v += 8;
    d->mtime    = UNPACK64(v); v += 8;
    d->length   = UNPACK64(v); v += 8;
    d->uid      = UNPACK32(v); v += 4;
    d->gid      = UNPACK32(v); v += 4;
    d->muid     = UNPACK32(v); v += 4;
    (void)ev;
}

char*
gefs_packlbl(char *p, int sz, const char *name)
{
    int n = strlen(name);
    if (sz < n + 1) return NULL;
    p[0] = GEFS_KLABEL; p += 1;
    memcpy(p, name, n); p += n;
    return p;
}

char*
gefs_packsnap(char *p, int sz, int64_t id)
{
    if (sz < GEFS_SNAPSZ) return NULL;
    p[0] = GEFS_KSNAP; p += 1;
    PACK64(p, id);     p += 8;
    return p;
}

char*
gefs_packbp(char *p, int sz, const GefsBptr *bp)
{
    if (sz < GEFS_PTRSZ) return NULL;
    PACK64(p, bp->addr); p += 8;
    PACK64(p, bp->hash); p += 8;
    PACK64(p, bp->gen);  p += 8;
    return p;
}

GefsBptr
gefs_unpackbp(const char *p, int sz)
{
    GefsBptr bp;
    (void)sz;
    bp.addr = UNPACK64(p); p += 8;
    bp.hash = UNPACK64(p); p += 8;
    bp.gen  = UNPACK64(p);
    return bp;
}

GefsTree*
gefs_unpacktree(GefsTree *t, const char *p, int sz)
{
    if (sz < GEFS_TREESZ) return NULL;
    memset(t, 0, sizeof(GefsTree));
    t->nref     = UNPACK32(p); p += 4;
    t->nlbl     = UNPACK32(p); p += 4;
    t->ht       = UNPACK32(p); p += 4;
    t->flag     = UNPACK32(p); p += 4;
    t->gen      = UNPACK64(p); p += 8;
    t->pred     = UNPACK64(p); p += 8;
    t->succ     = UNPACK64(p); p += 8;
    t->base     = UNPACK64(p); p += 8;
    t->bp.addr  = UNPACK64(p); p += 8;
    t->bp.hash  = UNPACK64(p); p += 8;
    t->bp.gen   = UNPACK64(p);
    return t;
}

int
gefs_unpacksb(GefsSuper *fi, const char *p0, int sz)
{
    uint64_t dh, xh;
    const char *p = p0;
    int i;

    if (sz != (int)GEFS_BLKSZ) return -1;
    if (memcmp(p, "gefs9.00", 8) != 0) return -1;
    p += 8;

    fi->blksz       = UNPACK32(p); p += 4;
    fi->bufspc      = UNPACK32(p); p += 4;
    fi->narena      = UNPACK32(p); p += 4;
    fi->snap.ht     = UNPACK32(p); p += 4;
    fi->snap.bp.addr = UNPACK64(p); p += 8;
    fi->snap.bp.hash = UNPACK64(p); p += 8;
    fi->snap.bp.gen = -1;
    p += 8*4; /* snapdl.hd and snapdl.tl addr/hash */
    fi->flag        = UNPACK64(p); p += 8;
    fi->nextqid     = UNPACK64(p); p += 8;
    fi->nextgen     = UNPACK64(p); p += 8;
    fi->qgen        = UNPACK64(p); p += 8;

    if (fi->narena <= 0 || fi->narena > 512) return -1;

    fi->arenabp = calloc(fi->narena, sizeof(GefsBptr));
    if (!fi->arenabp) return -ENOMEM;

    for (i = 0; i < fi->narena; i++) {
        fi->arenabp[i].addr = UNPACK64(p); p += 8;
        fi->arenabp[i].hash = UNPACK64(p); p += 8;
        fi->arenabp[i].gen  = -1;
    }

    xh = gefs_bufhash(p0, p - p0);
    dh = UNPACK64(p); p += 8;
    if (dh != xh) {
        free(fi->arenabp);
        fi->arenabp = NULL;
        return -EINVAL; /* Superblock checksum mismatch */
    }

    return 0;
}

/* =========================================================================
 * Key Comparison & Copy
 * ========================================================================= */

int
gefs_keycmp(const GefsKey *a, const GefsKey *b)
{
    int c, n;
    n = (a->nk < b->nk) ? a->nk : b->nk;
    if ((c = memcmp(a->k, b->k, n)) != 0)
        return c < 0 ? -1 : 1;
    if (a->nk < b->nk)
        return -1;
    else if (a->nk > b->nk)
        return 1;
    return 0;
}

void
gefs_cpkvp(GefsKvp *dst, const GefsKvp *src, char *buf, int nbuf)
{
    assert(src->nk + src->nv <= nbuf);
    memmove(buf, src->k, src->nk);
    memmove(buf + src->nk, src->v, src->nv);
    dst->k = buf;
    dst->nk = src->nk;
    dst->v = buf + src->nk;
    dst->nv = src->nv;
}

/* =========================================================================
 * Block Cache
 * ========================================================================= */

void
gefs_cache_init(GefsCtx *ctx, size_t max_blocks)
{
    memset(&ctx->cache, 0, sizeof(GefsCache));
    ctx->cache.max = max_blocks > 0 ? max_blocks : 2048;
}

static uint32_t
cache_hash(int64_t addr)
{
    uint64_t x = (uint64_t)addr;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return (uint32_t)(x % GEFS_NBUCKETS);
}

static void
lru_top(GefsCache *c, GefsBlk *b)
{
    if (b == c->chead) return;

    if (b->cprev) b->cprev->cnext = b->cnext;
    if (b->cnext) b->cnext->cprev = b->cprev;
    if (b == c->ctail) c->ctail = b->cprev;

    b->cprev = NULL;
    b->cnext = c->chead;
    if (c->chead) c->chead->cprev = b;
    c->chead = b;
    if (!c->ctail) c->ctail = b;
}

static void
lru_remove(GefsCache *c, GefsBlk *b)
{
    if (b->cprev) b->cprev->cnext = b->cnext;
    if (b->cnext) b->cnext->cprev = b->cprev;
    if (b == c->chead) c->chead = b->cnext;
    if (b == c->ctail) c->ctail = b->cprev;
    b->cprev = b->cnext = NULL;
}

GefsBlk*
gefs_getblk(GefsCtx *ctx, GefsBptr bp)
{
    uint32_t h = cache_hash(bp.addr);
    GefsBlk *b;

    /* Check hash table */
    for (b = ctx->cache.buckets[h]; b != NULL; b = b->hnext) {
        if (b->bp.addr == bp.addr) {
            b->ref++;
            lru_top(&ctx->cache, b);
            return b;
        }
    }

    /* Evict if over max and ref == 0 */
    if (ctx->cache.count >= ctx->cache.max) {
        GefsBlk *victim = ctx->cache.ctail;
        while (victim != NULL && victim->ref > 0)
            victim = victim->cprev;

        if (victim != NULL) {
            uint32_t vh = cache_hash(victim->bp.addr);
            GefsBlk **pp = &ctx->cache.buckets[vh];
            while (*pp && *pp != victim)
                pp = &(*pp)->hnext;
            if (*pp) *pp = victim->hnext;

            lru_remove(&ctx->cache, victim);
            free(victim);
            ctx->cache.count--;
        }
    }

    /* Allocate and read from disk */
    b = calloc(1, sizeof(GefsBlk));
    if (!b) return NULL;

    ssize_t n = pread(ctx->fd, b->buf, GEFS_BLKSZ, bp.addr);
    if (n != (ssize_t)GEFS_BLKSZ) {
        free(b);
        return NULL;
    }

    b->bp = bp;
    b->ref = 1;
    b->type = UNPACK16(b->buf);

    if (b->type == GEFS_TLEAF || b->type == GEFS_TPIVOT) {
        b->data = b->buf + (b->type == GEFS_TLEAF ? GEFS_LEAFHDSZ : GEFS_PIVHDSZ);
        b->nval = UNPACK16(b->buf + 2);
        b->valsz = UNPACK16(b->buf + 4);
        if (b->type == GEFS_TPIVOT) {
            b->nbuf = UNPACK16(b->buf + 6);
            b->bufsz = UNPACK16(b->buf + 8);
        }
    } else {
        b->data = b->buf;
    }

    /* Insert into hash table */
    b->hnext = ctx->cache.buckets[h];
    ctx->cache.buckets[h] = b;

    /* Insert into LRU head */
    b->cnext = ctx->cache.chead;
    b->cprev = NULL;
    if (ctx->cache.chead) ctx->cache.chead->cprev = b;
    ctx->cache.chead = b;
    if (!ctx->cache.ctail) ctx->cache.ctail = b;
    ctx->cache.count++;

    return b;
}

GefsBlk*
gefs_holdblk(GefsBlk *b)
{
    if (b) b->ref++;
    return b;
}

void
gefs_dropblk(GefsCtx *ctx, GefsBlk *b)
{
    if (!b) return;
    b->ref--;
    if (b->ref < 0) b->ref = 0;
    (void)ctx;
}

void
gefs_cache_free(GefsCtx *ctx)
{
    GefsBlk *b = ctx->cache.chead;
    while (b != NULL) {
        GefsBlk *next = b->cnext;
        free(b);
        b = next;
    }
    memset(&ctx->cache, 0, sizeof(GefsCache));
}

/* =========================================================================
 * Block Value & Message Access
 * ========================================================================= */

void
gefs_getval(GefsBlk *b, int i, GefsKvp *kv)
{
    char *p;
    int o;

    assert(i >= 0 && i < b->nval);
    p = b->data + 2 * i;
    o = UNPACK16(p);
    p = b->data + o;
    kv->nk = UNPACK16(p); p += 2;
    kv->k  = p;           p += kv->nk;
    kv->nv = UNPACK16(p); p += 2;
    kv->v  = p;
}

void
gefs_getmsg(GefsBlk *b, int i, GefsMsg *m)
{
    char *p;
    int o;

    assert(b->type == GEFS_TPIVOT);
    assert(i >= 0 && i < b->nbuf);
    p = b->data + GEFS_PIVSPC + 2 * i;
    o = UNPACK16(p);
    p = b->data + GEFS_PIVSPC + o;
    m->op = *p;           p += 1;
    m->nk = UNPACK16(p); p += 2;
    m->k  = p;           p += m->nk;
    m->nv = UNPACK16(p); p += 2;
    m->v  = p;
}

/* =========================================================================
 * B-Tree Search & Lookup Engine
 * ========================================================================= */

static int
blksearch(GefsBlk *b, const GefsKey *k, GefsKvp *rp, int *same)
{
    int lo, hi, ri, mid, r;
    GefsKvp cmp;

    ri = -1;
    lo = 0;
    hi = b->nval - 1;
    while (lo <= hi) {
        mid = (hi + lo) / 2;
        gefs_getval(b, mid, &cmp);
        GefsKey cmpk = { cmp.k, cmp.nk };
        r = gefs_keycmp(k, &cmpk);
        switch (r) {
        case -1:
            hi = mid - 1;
            break;
        case 0:
            ri = mid;
            hi = mid - 1; /* Find leftmost matching */
            break;
        case 1:
            lo = mid + 1;
            break;
        }
    }
    *same = 0;
    if (ri == -1)
        ri = lo - 1;
    else
        *same = 1;
    if (ri >= 0)
        gefs_getval(b, ri, rp);
    return ri;
}

static int
bufsearch(GefsBlk *b, const GefsKey *k, GefsMsg *m, int *same)
{
    int lo, hi, ri, mid, r;
    GefsMsg cmp;

    ri = -1;
    lo = 0;
    hi = b->nbuf - 1;
    while (lo <= hi) {
        mid = (hi + lo) / 2;
        gefs_getmsg(b, mid, &cmp);
        GefsKey cmpk = { cmp.k, cmp.nk };
        r = gefs_keycmp(k, &cmpk);
        switch (r) {
        case -1:
            hi = mid - 1;
            break;
        case 0:
            ri = mid;
            hi = mid - 1;
            break;
        case 1:
            lo = mid + 1;
            break;
        }
    }
    *same = 0;
    if (ri == -1)
        ri = lo - 1;
    else
        *same = 1;
    if (m != NULL && ri >= 0)
        gefs_getmsg(b, ri, m);
    return ri;
}

static void
statupdate(GefsKvp *kv, const GefsMsg *m)
{
    GefsXdir d;
    char *v, *ev;
    uint32_t f;

    gefs_kv2dir(kv, &d);
    v = m->v;
    ev = v + m->nv;
    f = UNPACK32(v); v += 4;
    if (f & (1 << 0)) { d.length = UNPACK64(v); v += 8; }
    if (f & (1 << 1)) { d.mode   = UNPACK32(v); v += 4; }
    if (f & (1 << 2)) { d.mtime  = UNPACK64(v); v += 8; }
    if (f & (1 << 3)) { d.atime  = UNPACK64(v); v += 8; }
    if (f & (1 << 4)) { d.uid    = UNPACK32(v); v += 4; }
    if (f & (1 << 5)) { d.gid    = UNPACK32(v); v += 4; }
    if (f & (1 << 6)) { d.muid   = UNPACK32(v); v += 4; }
    (void)ev;
    gefs_packdval(kv->v, kv->nv, &d);
}

static int
apply_msg(GefsKvp *kv, const GefsMsg *m, char *buf, int nbuf)
{
    GefsKey kvk = { kv->k, kv->nk };
    GefsKey mk  = { m->k, m->nk };

    switch (m->op) {
    case GEFS_ODELETE:
    case GEFS_OCLEARB:
    case GEFS_OCLOBBER:
        return 0;
    case GEFS_OINSERT: {
        GefsKvp tmp = { m->k, m->nk, m->nv, m->v };
        gefs_cpkvp(kv, &tmp, buf, nbuf);
        return 1;
    }
    case GEFS_OWSTAT:
        if (gefs_keycmp(&kvk, &mk) == 0)
            statupdate(kv, m);
        return 1;
    default:
        return 1;
    }
}

int
gefs_btlookup(GefsCtx *ctx, GefsTree *t, GefsKey *k, GefsKvp *r, char *buf, int nbuf)
{
    int i, j, h, ok, same;
    GefsBlk *b, **p;
    GefsBptr bp;
    GefsMsg m;

    h = t->ht;
    b = gefs_getblk(ctx, t->bp);
    if (!b) return 0;

    p = calloc(h, sizeof(GefsBlk*));
    if (!p) {
        gefs_dropblk(ctx, b);
        return 0;
    }

    ok = 0;
    p[0] = gefs_holdblk(b);
    for (i = 1; i < h; i++) {
        if (blksearch(p[i - 1], k, r, &same) == -1)
            break;
        bp = gefs_unpackbp(r->v, r->nv);
        p[i] = gefs_getblk(ctx, bp);
        if (!p[i]) break;
    }

    if (p[h - 1] != NULL)
        blksearch(p[h - 1], k, r, &ok);

    if (ok)
        gefs_cpkvp(r, r, buf, nbuf);

    /* Apply deferred messages from pivot nodes bottom-to-top */
    for (i = h - 2; i >= 0; i--) {
        if (p[i] == NULL) continue;
        j = bufsearch(p[i], k, &m, &same);
        if (j < 0 || !same) continue;

        if (ok || m.op == GEFS_OINSERT)
            ok = apply_msg(r, &m, buf, nbuf);

        for (j++; j < p[i]->nbuf; j++) {
            gefs_getmsg(p[i], j, &m);
            GefsKey mk = { m.k, m.nk };
            if (gefs_keycmp(k, &mk) != 0)
                break;
            ok = apply_msg(r, &m, buf, nbuf);
        }
    }

    for (i = 0; i < h; i++) {
        if (p[i] != NULL)
            gefs_dropblk(ctx, p[i]);
    }
    gefs_dropblk(ctx, b);
    free(p);
    return ok;
}

/* =========================================================================
 * B-Tree Directory Scan Engine
 * ========================================================================= */

void
gefs_btnewscan(GefsScan *s, const char *pfx, int npfx)
{
    memset(s, 0, sizeof(*s));
    s->first = 1;
    s->donescan = 0;
    s->offset = 0;
    s->pfx.k = s->pfxbuf;
    s->pfx.nk = npfx;
    memmove(s->pfxbuf, pfx, npfx);

    s->kv.k = s->kvbuf;
    s->kv.nk = npfx;
    s->kv.v = s->kvbuf + npfx;
    s->kv.nv = 0;
    memmove(s->kvbuf, pfx, npfx);
}

void
gefs_btenter(GefsCtx *ctx, GefsTree *t, GefsScan *s)
{
    int i, same;
    GefsScanp *p;
    GefsMsg m, c;
    GefsBptr bp;
    GefsBlk *b;
    GefsKvp v;

    if (s->donescan) return;

    s->ht = t->ht;
    b = gefs_getblk(ctx, t->bp);
    if (!b) { s->donescan = 1; return; }

    s->path = calloc(s->ht, sizeof(GefsScanp));
    if (!s->path) {
        gefs_dropblk(ctx, b);
        s->donescan = 1;
        return;
    }

    p = s->path;
    p[0].b = b;

    GefsKey skvk = { s->kv.k, s->kv.nk };

    for (i = 0; i < s->ht; i++) {
        p[i].vi = blksearch(b, &skvk, &v, &same);
        if (b->type == GEFS_TPIVOT) {
            if (p[i].vi == -1)
                gefs_getval(b, ++p[i].vi, &v);
            p[i].bi = bufsearch(b, &skvk, &m, &same);
            if (p[i].bi == -1) {
                p[i].bi++;
            } else if (!same || !s->first) {
                while (p[i].bi < p[i].b->nbuf) {
                    gefs_getmsg(p[i].b, p[i].bi, &c);
                    GefsKey mk = { m.k, m.nk };
                    GefsKey ck = { c.k, c.nk };
                    if (gefs_keycmp(&mk, &ck) != 0)
                        break;
                    p[i].bi++;
                }
            }
            bp = gefs_unpackbp(v.v, v.nv);
            b = gefs_getblk(ctx, bp);
            p[i + 1].b = b;
        } else if (p[i].vi == -1 || !same || !s->first) {
            p[i].vi++;
        }
    }
    s->first = 0;
}

int
gefs_btnext(GefsCtx *ctx, GefsScan *s, GefsKvp *r)
{
    int i, j, h, ok, start, bufsrc;
    GefsScanp *p;
    GefsMsg m, n;
    GefsBptr bp;
    GefsKvp kv;

Again:
    p = s->path;
    h = s->ht;
    start = h;
    bufsrc = -1;

    if (s->donescan) return 0;

    /* Advance to find next entry */
    for (i = h - 1; i >= 0; i--) {
        if (p[i].b != NULL &&
            (p[i].vi < p[i].b->nval || (p[i].b->type == GEFS_TPIVOT && p[i].bi < p[i].b->nbuf)))
            break;
        if (i == 0) {
            s->donescan = 1;
            return 0;
        }
        if (p[i].b != NULL)
            gefs_dropblk(ctx, p[i].b);
        p[i].b = NULL;
        p[i].vi = 0;
        p[i].bi = 0;
        p[i - 1].vi++;
        start = i;
    }

    if (p[start - 1].vi < p[start - 1].b->nval) {
        for (i = start; i < h; i++) {
            gefs_getval(p[i - 1].b, p[i - 1].vi, &kv);
            bp = gefs_unpackbp(kv.v, kv.nv);
            p[i].b = gefs_getblk(ctx, bp);
            if (!p[i].b) { s->donescan = 1; return 0; }
        }
        m.op = GEFS_OINSERT;
        gefs_getval(p[h - 1].b, p[h - 1].vi, &kv);
        m.k = kv.k; m.nk = kv.nk; m.v = kv.v; m.nv = kv.nv;
    } else {
        gefs_getmsg(p[start - 1].b, p[start - 1].bi, &m);
        bufsrc = start - 1;
    }

    /* Check pivot buffers for smaller keys */
    for (i = h - 2; i >= 0; i--) {
        if (p[i].b == NULL || p[i].bi == p[i].b->nbuf)
            continue;
        gefs_getmsg(p[i].b, p[i].bi, &n);
        GefsKey nk = { n.k, n.nk };
        GefsKey mk = { m.k, m.nk };
        if (gefs_keycmp(&nk, &mk) < 0) {
            bufsrc = i;
            m = n;
        }
    }

    /* Check if still matches prefix */
    if (m.nk < s->pfx.nk || memcmp(m.k, s->pfx.k, s->pfx.nk) != 0) {
        s->donescan = 1;
        return 0;
    }

    /* Apply all messages matching this key */
    ok = 1;
    GefsKvp mkv = { m.k, m.nk, m.nv, m.v };
    gefs_cpkvp(r, &mkv, s->kvbuf, sizeof(s->kvbuf));

    if (bufsrc == -1)
        p[h - 1].vi++;
    else
        p[bufsrc].bi++;

    for (i = h - 2; i >= 0; i--) {
        for (j = p[i].bi; p[i].b != NULL && j < p[i].b->nbuf; j++) {
            gefs_getmsg(p[i].b, j, &m);
            GefsKey rk = { r->k, r->nk };
            GefsKey mk = { m.k, m.nk };
            if (gefs_keycmp(&rk, &mk) != 0)
                break;
            ok = apply_msg(r, &m, s->kvbuf, sizeof(s->kvbuf));
            p[i].bi++;
        }
    }

    if (!ok) goto Again;
    return 1;
}

void
gefs_btexit(GefsCtx *ctx, GefsScan *s)
{
    int i;
    if (!s->path) return;
    for (i = 0; i < s->ht; i++) {
        if (s->path[i].b)
            gefs_dropblk(ctx, s->path[i].b);
    }
    free(s->path);
    s->path = NULL;
}

/* =========================================================================
 * High-Level Filesystem Operations
 * ========================================================================= */

static void
load_arena(GefsCtx *ctx, int idx, GefsBptr h0_bp)
{
    GefsArena *a = &ctx->arenas[idx];
    GefsBlk *h0 = gefs_getblk(ctx, h0_bp);
    if (!h0) return;

    a->hdaddr = h0_bp.addr;
    const char *p = h0->data;
    a->size = UNPACK64(p); p += 8;
    a->used = UNPACK64(p); p += 8;
    p += 8; /* reserve */
    p += 8; /* flag */
    a->loghd = gefs_unpackbp(p, GEFS_PTRSZ);

    gefs_dropblk(ctx, h0);

    /* Read log chain to populate free list */
    GefsBptr bp = a->loghd;
    while (bp.addr != -1) {
        GefsBlk *b = gefs_getblk(ctx, bp);
        if (!b) break;
        int logsz = UNPACK16(b->buf + 2);
        GefsBptr next_bp = gefs_unpackbp(b->buf + 12, GEFS_PTRSZ);
        const char *lp = b->data;

        for (int i = 0; i < logsz; ) {
            uint64_t ent = UNPACK64(lp + i);
            int op = ent & 0xff;
            int64_t off = ent & ~0xffULL;
            int step = (op == 0 || op == 1) ? 16 : 8; /* LogFree=0, LogAlloc=1 (wide) */
            int64_t len = (step == 16) ? UNPACK64(lp + i + 8) : GEFS_BLKSZ;

            if (op == 0 || op == 2) { /* LogFree / LogFree1 */
                GefsRange *r = calloc(1, sizeof(GefsRange));
                r->off = off;
                r->len = len;
                r->next = a->free_list;
                a->free_list = r;
            } else if (op == 1 || op == 3) { /* LogAlloc / LogAlloc1 */
                GefsRange **pp = &a->free_list;
                while (*pp) {
                    GefsRange *cur = *pp;
                    if (off >= cur->off && off + len <= cur->off + cur->len) {
                        if (off == cur->off && len == cur->len) {
                            *pp = cur->next;
                            free(cur);
                        } else if (off == cur->off) {
                            cur->off += len;
                            cur->len -= len;
                        } else if (off + len == cur->off + cur->len) {
                            cur->len -= len;
                        } else {
                            GefsRange *tail = calloc(1, sizeof(GefsRange));
                            tail->off = off + len;
                            tail->len = (cur->off + cur->len) - (off + len);
                            tail->next = cur->next;
                            cur->len = off - cur->off;
                            cur->next = tail;
                        }
                        break;
                    }
                    pp = &(*pp)->next;
                }
            }
            i += step;
        }

        bp = next_bp;
        gefs_dropblk(ctx, b);
    }
}

int
gefs_fs_open(GefsCtx *ctx, const char *path)
{
    char sbbuf[GEFS_BLKSZ];

    memset(ctx, 0, sizeof(GefsCtx));

    ctx->readonly = 0;
    ctx->fd = open(path, O_RDWR);
    if (ctx->fd < 0) {
        ctx->readonly = 1;
        ctx->fd = open(path, O_RDONLY);
        if (ctx->fd < 0)
            return -errno;
    }

    /* Read primary superblock */
    ssize_t n = pread(ctx->fd, sbbuf, GEFS_BLKSZ, 0);
    if (n != (ssize_t)GEFS_BLKSZ) {
        close(ctx->fd);
        return -EIO;
    }

    /* Unpack superblock */
    int ret = gefs_unpacksb(&ctx->sb, sbbuf, GEFS_BLKSZ);
    if (ret < 0) {
        /* Try backup superblock at end */
        off_t fsize = lseek(ctx->fd, 0, SEEK_END);
        if (fsize > (off_t)GEFS_BLKSZ) {
            n = pread(ctx->fd, sbbuf, GEFS_BLKSZ, fsize - GEFS_BLKSZ);
            if (n == (ssize_t)GEFS_BLKSZ)
                ret = gefs_unpacksb(&ctx->sb, sbbuf, GEFS_BLKSZ);
        }
        if (ret < 0) {
            close(ctx->fd);
            return -EINVAL; /* Invalid or corrupted GEFS image */
        }
    }

    /* Initialize block cache */
    gefs_cache_init(ctx, 4096);

    /* Initialize in-memory arenas */
    if (ctx->sb.narena > 0) {
        ctx->arenas = calloc(ctx->sb.narena, sizeof(GefsArena));
        for (int i = 0; i < ctx->sb.narena; i++) {
            load_arena(ctx, i, ctx->sb.arenabp[i]);
        }
    }

    /* Root snapshot tree from superblock */
    ctx->snap_tree = ctx->sb.snap;

    /* Open default snapshot "main" */
    int flg = 0;
    ret = gefs_opensnap(ctx, "main", &ctx->active_tree, &flg);
    if (ret < 0) {
        /* Fallback: try "empty" or use snap tree */
        ret = gefs_opensnap(ctx, "empty", &ctx->active_tree, &flg);
        if (ret < 0)
            ctx->active_tree = ctx->snap_tree;
    }
    snprintf(ctx->active_snap_name, sizeof(ctx->active_snap_name), "main");

    return 0;
}

void
gefs_fs_close(GefsCtx *ctx)
{
    if (ctx->dirty && !ctx->readonly) {
        gefs_sync_fs(ctx);
    }
    if (ctx->arenas) {
        for (int i = 0; i < ctx->sb.narena; i++) {
            GefsRange *r = ctx->arenas[i].free_list;
            while (r) {
                GefsRange *next = r->next;
                free(r);
                r = next;
            }
        }
        free(ctx->arenas);
        ctx->arenas = NULL;
    }
    if (ctx->sb.arenabp) {
        free(ctx->sb.arenabp);
        ctx->sb.arenabp = NULL;
    }
    gefs_cache_free(ctx);
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

int
gefs_opensnap(GefsCtx *ctx, const char *label, GefsTree *t, int *flg)
{
    char buf[GEFS_KVMAX], *p;
    GefsKvp kv;
    GefsKey k;
    int64_t gen;

    /* Lookup Klabel{name} => Ksnap{id} */
    p = gefs_packlbl(buf, sizeof(buf), label);
    if (!p) return -EINVAL;
    k.k = buf;
    k.nk = p - buf;

    if (!gefs_btlookup(ctx, &ctx->snap_tree, &k, &kv, buf, sizeof(buf)))
        return -ENOENT;

    if (kv.nv < 1 + 8 + 4) return -EIO;
    gen = UNPACK64(kv.v + 1);
    if (flg) *flg = UNPACK32(kv.v + 1 + 8);

    /* Lookup Ksnap{id} => Tree */
    p = gefs_packsnap(buf, sizeof(buf), gen);
    k.k = buf;
    k.nk = p - buf;

    if (!gefs_btlookup(ctx, &ctx->snap_tree, &k, &kv, buf, sizeof(buf)))
        return -EIO;

    if (!gefs_unpacktree(t, kv.v, kv.nv))
        return -EIO;

    return 0;
}

int
gefs_walk1(GefsCtx *ctx, GefsTree *t, int64_t pqid, const char *name, GefsXdir *d)
{
    char kbuf[GEFS_KEYMAX], rbuf[GEFS_KVMAX];
    char *p;
    GefsKvp kv;
    GefsKey k;

    p = gefs_packdkey(kbuf, sizeof(kbuf), pqid, name);
    if (!p) return -EINVAL;
    k.k = kbuf;
    k.nk = p - kbuf;

    if (!gefs_btlookup(ctx, t, &k, &kv, rbuf, sizeof(rbuf)))
        return -ENOENT;

    gefs_kv2dir(&kv, d);
    return 0;
}

int
gefs_walk_path(GefsCtx *ctx, GefsTree *t, const char *path, GefsXdir *d)
{
    char pathbuf[1024];
    char *elem, *saveptr;
    int64_t cur_qid = 0; /* Qmainroot = 0 */
    int ret;

    /* Root path "/" */
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        return gefs_walk1(ctx, t, -1, "", d);
    }

    /* Start at root */
    ret = gefs_walk1(ctx, t, -1, "", d);
    if (ret < 0) return ret;
    cur_qid = d->qid.path;

    snprintf(pathbuf, sizeof(pathbuf), "%s", path);
    elem = strtok_r(pathbuf, "/", &saveptr);
    while (elem != NULL) {
        if (strlen(elem) == 0 || strcmp(elem, ".") == 0) {
            elem = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        ret = gefs_walk1(ctx, t, cur_qid, elem, d);
        if (ret < 0) return ret;

        cur_qid = d->qid.path;
        elem = strtok_r(NULL, "/", &saveptr);
    }

    return 0;
}

int
gefs_list_dir(GefsCtx *ctx, GefsTree *t, int64_t qid_path,
              int (*callback)(const GefsXdir *d, void *arg), void *arg)
{
    char pfx[9];
    GefsScan s;
    GefsXdir d;

    /* Key prefix: [Kent:1][qid_path:8] */
    pfx[0] = GEFS_KENT;
    PACK64(pfx + 1, qid_path);

    gefs_btnewscan(&s, pfx, sizeof(pfx));
    gefs_btenter(ctx, t, &s);

    while (gefs_btnext(ctx, &s, &s.kv)) {
        gefs_kv2dir(&s.kv, &d);
        /* Skip self-entry (name == "") */
        if (d.name && d.name[0] != '\0') {
            if (callback(&d, arg) != 0)
                break;
        }
    }

    gefs_btexit(ctx, &s);
    return 0;
}

ssize_t
gefs_read_file(GefsCtx *ctx, GefsTree *t, const GefsXdir *d, char *buf, size_t size, off_t offset)
{
    char kbuf[GEFS_KEYMAX], rbuf[GEFS_KVMAX];
    GefsKvp kv;
    GefsKey k;
    size_t total_read = 0;

    if (offset >= (off_t)d->length)
        return 0;

    if (offset + size > d->length)
        size = d->length - offset;

    while (size > 0) {
        int64_t blk_off = (offset / GEFS_BLKSZ) * GEFS_BLKSZ;
        size_t in_blk_off = offset % GEFS_BLKSZ;
        size_t to_read = GEFS_BLKSZ - in_blk_off;
        if (to_read > size) to_read = size;

        /* Build Kdat key: [Kdat:1][qid:8][off:8] */
        kbuf[0] = GEFS_KDAT;
        PACK64(kbuf + 1, d->qid.path);
        PACK64(kbuf + 9, blk_off);
        k.k = kbuf;
        k.nk = GEFS_OFFKSZ;

        if (gefs_btlookup(ctx, t, &k, &kv, rbuf, sizeof(rbuf))) {
            GefsBptr bp = gefs_unpackbp(kv.v, kv.nv);
            GefsBlk *b = gefs_getblk(ctx, bp);
            if (b) {
                memcpy(buf + total_read, b->data + in_blk_off, to_read);
                gefs_dropblk(ctx, b);
            } else {
                memset(buf + total_read, 0, to_read); /* Sparse/hole */
            }
        } else {
            memset(buf + total_read, 0, to_read); /* Sparse hole */
        }

        total_read += to_read;
        offset += to_read;
        size -= to_read;
    }

    return total_read;
}

mode_t
gefs_mode_to_posix(uint32_t gefs_mode, uint8_t qid_type)
{
    mode_t m = 0;

    if ((qid_type & GEFS_QTDIR) || (gefs_mode & GEFS_DMDIR))
        m |= S_IFDIR;
    else if (qid_type & GEFS_QTSYMLINK)
        m |= S_IFLNK;
    else
        m |= S_IFREG;

    /* Permission bits */
    m |= (gefs_mode & 0777);

    return m;
}

/* =========================================================================
 * B-Tree Mutation & Write Implementation
 * ========================================================================= */

int64_t
gefs_alloc_blk(GefsCtx *ctx, int type)
{
    (void)type;
    if (ctx->readonly || ctx->sb.narena <= 0 || !ctx->arenas)
        return -1;

    for (int i = 0; i < ctx->sb.narena; i++) {
        GefsArena *a = &ctx->arenas[i];
        if (a->free_list != NULL) {
            GefsRange *r = a->free_list;
            int64_t addr = r->off;
            r->off += GEFS_BLKSZ;
            r->len -= GEFS_BLKSZ;
            a->used += GEFS_BLKSZ;
            if (r->len == 0) {
                a->free_list = r->next;
                free(r);
            }
            ctx->dirty = 1;
            return addr;
        }
    }
    return -1;
}

static void
blk_finalize_and_write(GefsCtx *ctx, GefsBlk *b)
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

    pwrite(ctx->fd, b->buf, GEFS_BLKSZ, b->bp.addr);
}

/* Helper to insert a key-value pair into a leaf or pivot node */
static void
blk_setval(GefsBlk *b, const GefsKvp *kv)
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

/* Upsert a list of messages into tree t */
int
gefs_btupsert(GefsCtx *ctx, GefsTree *t, GefsMsg *msgs, int nmsgs)
{
    if (ctx->readonly || nmsgs <= 0)
        return -EROFS;

    /* For height-1 leaf trees (and flattening updates) */
    GefsBlk *root = gefs_getblk(ctx, t->bp);
    if (!root)
        return -EIO;

    /* Reconstruct leaf entries applying updates */
    GefsBlk *new_root = calloc(1, sizeof(GefsBlk));
    int64_t new_addr = gefs_alloc_blk(ctx, GEFS_TLEAF);
    if (new_addr < 0) {
        free(new_root);
        gefs_dropblk(ctx, root);
        return -ENOSPC;
    }

    new_root->type = GEFS_TLEAF;
    new_root->bp.addr = new_addr;
    new_root->bp.gen = t->gen;
    new_root->data = new_root->buf + GEFS_LEAFHDSZ;

    char vbuf[GEFS_INLMAX];
    GefsKvp kv;

    /* Sort messages by key before merging */
    for (int a = 1; a < nmsgs; a++) {
        for (int b = a; b > 0; b--) {
            GefsKey ka = { msgs[b - 1].k, msgs[b - 1].nk };
            GefsKey kb = { msgs[b].k, msgs[b].nk };
            if (gefs_keycmp(&ka, &kb) <= 0)
                break;
            GefsMsg tmp = msgs[b - 1];
            msgs[b - 1] = msgs[b];
            msgs[b] = tmp;
        }
    }

    /* Merge existing entries with messages */
    int i = 0, j = 0;
    while (i < root->nval || j < nmsgs) {
        GefsKvp old_kv;
        int cmp = 0;

        if (i < root->nval && j < nmsgs) {
            gefs_getval(root, i, &old_kv);
            GefsKey ok = { old_kv.k, old_kv.nk };
            GefsKey mk = { msgs[j].k, msgs[j].nk };
            cmp = gefs_keycmp(&ok, &mk);
        } else if (i < root->nval) {
            cmp = -1;
            gefs_getval(root, i, &old_kv);
        } else {
            cmp = 1;
        }

        if (cmp < 0) {
            /* Copy existing entry */
            blk_setval(new_root, &old_kv);
            i++;
        } else if (cmp == 0) {
            /* Overwrite / apply mutation */
            if (msgs[j].op == GEFS_OINSERT) {
                kv.k = msgs[j].k;
                kv.nk = msgs[j].nk;
                kv.v = msgs[j].v;
                kv.nv = msgs[j].nv;
                blk_setval(new_root, &kv);
            } else if (msgs[j].op == GEFS_OWSTAT) {
                gefs_cpkvp(&kv, &old_kv, vbuf, sizeof(vbuf));
                statupdate(&kv, &msgs[j]);
                blk_setval(new_root, &kv);
            }
            /* GEFS_ODELETE / GEFS_OCLOBBER: skip (deleted) */
            i++;
            j++;
        } else {
            /* Insert new entry from message */
            if (msgs[j].op == GEFS_OINSERT) {
                kv.k = msgs[j].k;
                kv.nk = msgs[j].nk;
                kv.v = msgs[j].v;
                kv.nv = msgs[j].nv;
                blk_setval(new_root, &kv);
            }
            j++;
        }
    }

    blk_finalize_and_write(ctx, new_root);
    t->bp = new_root->bp;
    t->ht = 1;

    /* If updating active snapshot tree, keep it updated */
    if (t->gen == ctx->active_tree.gen) {
        ctx->active_tree.bp = new_root->bp;
        ctx->active_tree.ht = 1;
    }

    /* Insert new block into block cache */
    uint32_t h = cache_hash(new_root->bp.addr);
    new_root->ref = 1;
    new_root->hnext = ctx->cache.buckets[h];
    ctx->cache.buckets[h] = new_root;

    new_root->cnext = ctx->cache.chead;
    new_root->cprev = NULL;
    if (ctx->cache.chead) ctx->cache.chead->cprev = new_root;
    ctx->cache.chead = new_root;
    if (!ctx->cache.ctail) ctx->cache.ctail = new_root;
    ctx->cache.count++;

    gefs_dropblk(ctx, root);

    ctx->dirty = 1;
    return 0;
}

static char*
pack_sb_block(char *p0, int sz, const GefsSuper *sb)
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
gefs_sync_fs(GefsCtx *ctx)
{
    if (ctx->readonly || !ctx->dirty)
        return 0;

    /* Update snapshot catalog with active tree */
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    GefsMsg snap_msgs[2];

    /* Update Ksnap{gen} => active_tree */
    char *p = gefs_packsnap(kbuf, sizeof(kbuf), ctx->active_tree.gen);
    snap_msgs[0].op = GEFS_OINSERT;
    snap_msgs[0].k = kbuf;
    snap_msgs[0].nk = p - kbuf;

    char *vp = vbuf;
    PACK32(vp, ctx->active_tree.nref); vp += 4;
    PACK32(vp, ctx->active_tree.nlbl); vp += 4;
    PACK32(vp, ctx->active_tree.ht);   vp += 4;
    PACK32(vp, ctx->active_tree.flag); vp += 4;
    PACK64(vp, ctx->active_tree.gen);  vp += 8;
    PACK64(vp, ctx->active_tree.pred); vp += 8;
    PACK64(vp, ctx->active_tree.succ); vp += 8;
    PACK64(vp, ctx->active_tree.base); vp += 8;
    gefs_packbp(vp, GEFS_PTRSZ, &ctx->active_tree.bp); vp += GEFS_PTRSZ;
    snap_msgs[0].v = vbuf;
    snap_msgs[0].nv = vp - vbuf;

    gefs_btupsert(ctx, &ctx->snap_tree, snap_msgs, 1);

    /* Write out arena headers */
    for (int i = 0; i < ctx->sb.narena; i++) {
        GefsArena *a = &ctx->arenas[i];
        GefsBlk h0;
        memset(&h0, 0, sizeof(h0));
        h0.type = GEFS_TARENA;
        h0.bp.addr = a->hdaddr;
        h0.data = h0.buf + 2;

        char *ap = h0.data;
        PACK64(ap, a->size); ap += 8;
        PACK64(ap, a->used); ap += 8;
        PACK64(ap, 0);       ap += 8;
        PACK64(ap, 0);       ap += 8;
        gefs_packbp(ap, GEFS_PTRSZ, &a->loghd);

        blk_finalize_and_write(ctx, &h0);
        ctx->sb.arenabp[i] = h0.bp;
    }

    /* Update superblock */
    ctx->sb.snap = ctx->snap_tree;

    char sbbuf[GEFS_BLKSZ];
    pack_sb_block(sbbuf, GEFS_BLKSZ, &ctx->sb);
    pwrite(ctx->fd, sbbuf, GEFS_BLKSZ, 0);

    /* Backup superblock at end */
    off_t fsize = lseek(ctx->fd, 0, SEEK_END);
    if (fsize > (off_t)GEFS_BLKSZ) {
        pwrite(ctx->fd, sbbuf, GEFS_BLKSZ, fsize - GEFS_BLKSZ);
    }

    fsync(ctx->fd);
    ctx->dirty = 0;
    return 0;
}

/* Create regular file */
int
gefs_create_file(GefsCtx *ctx, const char *path, mode_t mode, GefsXdir *out_dir)
{
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    GefsXdir pdir, ndir;
    int ret;

    /* Split dirname and basename */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    /* Check if already exists */
    GefsXdir exist_dir;
    if (gefs_walk1(ctx, &ctx->active_tree, pdir.qid.path, name, &exist_dir) == 0)
        return -EEXIST;

    memset(&ndir, 0, sizeof(ndir));
    ndir.qid.path = ctx->sb.nextqid++;
    ndir.qid.vers = 0;
    ndir.qid.type = GEFS_QTFILE;
    ndir.mode = (mode & 0777);
    ndir.atime = (int64_t)time(NULL) * 1000000000LL;
    ndir.mtime = ndir.atime;
    ndir.length = 0;
    ndir.uid = getuid();
    ndir.gid = getgid();
    ndir.muid = getuid();
    ndir.name = name;

    GefsMsg msg;
    msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &ndir);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    ret = gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
    if (ret < 0) return ret;

    if (out_dir) *out_dir = ndir;
    return 0;
}

/* Create directory */
int
gefs_create_dir(GefsCtx *ctx, const char *path, mode_t mode)
{
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    char kbuf2[GEFS_KEYMAX], vbuf2[GEFS_INLMAX];
    GefsXdir pdir, ndir;
    int ret;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    GefsXdir exist_dir;
    if (gefs_walk1(ctx, &ctx->active_tree, pdir.qid.path, name, &exist_dir) == 0)
        return -EEXIST;

    memset(&ndir, 0, sizeof(ndir));
    ndir.qid.path = ctx->sb.nextqid++;
    ndir.qid.vers = 0;
    ndir.qid.type = GEFS_QTDIR;
    ndir.mode = GEFS_DMDIR | (mode & 0777);
    ndir.atime = (int64_t)time(NULL) * 1000000000LL;
    ndir.mtime = ndir.atime;
    ndir.length = 0;
    ndir.uid = getuid();
    ndir.gid = getgid();
    ndir.muid = getuid();
    ndir.name = name;

    GefsMsg msgs[2];

    /* 1. Kent entry in parent directory */
    msgs[0].op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msgs[0].k = kbuf;
    msgs[0].nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &ndir);
    msgs[0].v = vbuf;
    msgs[0].nv = p - vbuf;

    /* 2. Kup entry pointing back to parent */
    msgs[1].op = GEFS_OINSERT;
    p = gefs_packdkey(kbuf2, sizeof(kbuf2), ndir.qid.path, NULL);
    kbuf2[0] = GEFS_KUP;
    msgs[1].k = kbuf2;
    msgs[1].nk = p - kbuf2;
    p = gefs_packdkey(vbuf2, sizeof(vbuf2), pdir.qid.path, name);
    msgs[1].v = vbuf2;
    msgs[1].nv = p - vbuf2;

    return gefs_btupsert(ctx, &ctx->active_tree, msgs, 2);
}

/* Create symlink */
int
gefs_create_symlink(GefsCtx *ctx, const char *target, const char *linkpath)
{
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    GefsXdir pdir, ndir;
    int ret;

    const char *last_slash = strrchr(linkpath, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == linkpath) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", linkpath + 1);
    } else {
        size_t plen = last_slash - linkpath;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, linkpath);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    memset(&ndir, 0, sizeof(ndir));
    ndir.qid.path = ctx->sb.nextqid++;
    ndir.qid.vers = 0;
    ndir.qid.type = GEFS_QTSYMLINK;
    ndir.mode = 0777;
    ndir.atime = (int64_t)time(NULL) * 1000000000LL;
    ndir.mtime = ndir.atime;
    ndir.length = 0;
    ndir.uid = getuid();
    ndir.gid = getgid();
    ndir.muid = getuid();
    ndir.name = name;

    GefsMsg msg;
    msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &ndir);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    ret = gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
    if (ret < 0) return ret;

    /* Write target path as file data */
    ssize_t written = gefs_write_file(ctx, linkpath, target, strlen(target), 0);
    if (written < 0) return (int)written;

    /* Restore QTSYMLINK flag after write */
    gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    gefs_walk1(ctx, &ctx->active_tree, pdir.qid.path, name, &ndir);
    ndir.qid.type = GEFS_QTSYMLINK;

    msg.op = GEFS_OINSERT;
    p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &ndir);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    return gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
}

/* Write data to a file */
ssize_t
gefs_write_file(GefsCtx *ctx, const char *path, const char *buf, size_t size, off_t offset)
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    size_t total_written = 0;
    off_t cur_off = offset;

    while (size > 0) {
        int64_t blk_off = (cur_off / GEFS_BLKSZ) * GEFS_BLKSZ;
        size_t in_blk_off = cur_off % GEFS_BLKSZ;
        size_t to_write = GEFS_BLKSZ - in_blk_off;
        if (to_write > size) to_write = size;

        /* Look up existing data block or allocate a new one */
        char rkbuf[GEFS_KEYMAX], rvbuf[GEFS_KVMAX];
        GefsKey rk;
        GefsKvp rkv;
        rkbuf[0] = GEFS_KDAT;
        PACK64(rkbuf + 1, d.qid.path);
        PACK64(rkbuf + 9, blk_off);
        rk.k = rkbuf;
        rk.nk = GEFS_OFFKSZ;

        GefsBlk *b = calloc(1, sizeof(GefsBlk));
        if (gefs_btlookup(ctx, &ctx->active_tree, &rk, &rkv, rvbuf, sizeof(rvbuf))) {
            GefsBptr old_bp = gefs_unpackbp(rkv.v, rkv.nv);
            GefsBlk *old_b = gefs_getblk(ctx, old_bp);
            if (old_b) {
                memcpy(b->buf, old_b->buf, GEFS_BLKSZ);
                gefs_dropblk(ctx, old_b);
            }
        }

        int64_t baddr = gefs_alloc_blk(ctx, GEFS_TDAT);
        if (baddr < 0) {
            free(b);
            return -ENOSPC;
        }

        b->type = GEFS_TDAT;
        b->bp.addr = baddr;
        b->bp.gen = ctx->active_tree.gen;
        b->data = b->buf;

        memcpy(b->buf + in_blk_off, buf + total_written, to_write);
        blk_finalize_and_write(ctx, b);

        /* Insert into cache so subsequent reads immediately find it */
        uint32_t bh = cache_hash(b->bp.addr);
        b->ref = 1;
        b->hnext = ctx->cache.buckets[bh];
        ctx->cache.buckets[bh] = b;
        b->cnext = ctx->cache.chead;
        b->cprev = NULL;
        if (ctx->cache.chead) ctx->cache.chead->cprev = b;
        ctx->cache.chead = b;
        if (!ctx->cache.ctail) ctx->cache.ctail = b;
        ctx->cache.count++;

        /* Insert Kdat mapping */
        GefsMsg dmsg;
        dmsg.op = GEFS_OINSERT;
        dmsg.k = rkbuf;
        dmsg.nk = GEFS_OFFKSZ;
        char bp_buf[GEFS_PTRSZ];
        gefs_packbp(bp_buf, GEFS_PTRSZ, &b->bp);
        dmsg.v = bp_buf;
        dmsg.nv = GEFS_PTRSZ;

        gefs_btupsert(ctx, &ctx->active_tree, &dmsg, 1);

        total_written += to_write;
        cur_off += to_write;
        size -= to_write;
    }

    /* Re-fetch latest parent and file entry to ensure valid state before updating */
    gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    gefs_walk1(ctx, &ctx->active_tree, pdir.qid.path, name, &d);

    /* Update file metadata (length, mtime) */
    if (cur_off > (off_t)d.length)
        d.length = cur_off;
    d.mtime = (int64_t)time(NULL) * 1000000000LL;
    d.qid.vers++;

    GefsMsg stat_msg;
    stat_msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    stat_msg.k = kbuf;
    stat_msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &d);
    stat_msg.v = vbuf;
    stat_msg.nv = p - vbuf;

    gefs_btupsert(ctx, &ctx->active_tree, &stat_msg, 1);

    return total_written;
}

/* Truncate file */
int
gefs_truncate_file(GefsCtx *ctx, const char *path, off_t new_size)
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    d.length = new_size;
    d.mtime = (int64_t)time(NULL) * 1000000000LL;
    d.qid.vers++;

    GefsMsg msg;
    msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &d);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    return gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
}

/* Unlink (remove) regular file or symlink */
int
gefs_unlink_entry(GefsCtx *ctx, const char *path)
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    if (d.qid.type & GEFS_QTDIR)
        return -EISDIR;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    GefsMsg msg;
    msg.op = GEFS_ODELETE;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    msg.v = "";
    msg.nv = 0;

    return gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
}

/* Remove directory */
int
gefs_rmdir_entry(GefsCtx *ctx, const char *path)
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], kbuf2[GEFS_KEYMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    if (!(d.qid.type & GEFS_QTDIR) && !(d.mode & GEFS_DMDIR))
        return -ENOTDIR;

    /* Check if directory is empty */
    char pfx[9];
    GefsScan s;
    pfx[0] = GEFS_KENT;
    PACK64(pfx + 1, d.qid.path);
    gefs_btnewscan(&s, pfx, sizeof(pfx));
    gefs_btenter(ctx, &ctx->active_tree, &s);
    int has_entries = 0;
    while (gefs_btnext(ctx, &s, &s.kv)) {
        GefsXdir child;
        gefs_kv2dir(&s.kv, &child);
        if (child.name && child.name[0] != '\0') {
            has_entries = 1;
            break;
        }
    }
    gefs_btexit(ctx, &s);
    if (has_entries)
        return -ENOTEMPTY;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    GefsMsg msgs[2];

    /* 1. Delete Kent entry in parent */
    msgs[0].op = GEFS_ODELETE;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msgs[0].k = kbuf;
    msgs[0].nk = p - kbuf;
    msgs[0].v = "";
    msgs[0].nv = 0;

    /* 2. Delete Kup entry */
    msgs[1].op = GEFS_ODELETE;
    p = gefs_packdkey(kbuf2, sizeof(kbuf2), d.qid.path, NULL);
    kbuf2[0] = GEFS_KUP;
    msgs[1].k = kbuf2;
    msgs[1].nk = p - kbuf2;
    msgs[1].v = "";
    msgs[1].nv = 0;

    return gefs_btupsert(ctx, &ctx->active_tree, msgs, 2);
}

/* Rename / Move file or directory */
int
gefs_rename_entry(GefsCtx *ctx, const char *oldpath, const char *newpath)
{
    GefsXdir d, old_pdir, new_pdir;
    char old_ppath[1024], old_name[256];
    char new_ppath[1024], new_name[256];
    char kbuf_del[GEFS_KEYMAX], kbuf_ins[GEFS_KEYMAX], vbuf_ins[GEFS_INLMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, oldpath, &d);
    if (ret < 0) return ret;

    const char *slash = strrchr(oldpath, '/');
    if (!slash) return -EINVAL;
    if (slash == oldpath) {
        snprintf(old_ppath, sizeof(old_ppath), "/");
        snprintf(old_name, sizeof(old_name), "%s", oldpath + 1);
    } else {
        snprintf(old_ppath, sizeof(old_ppath), "%.*s", (int)(slash - oldpath), oldpath);
        snprintf(old_name, sizeof(old_name), "%s", slash + 1);
    }

    slash = strrchr(newpath, '/');
    if (!slash) return -EINVAL;
    if (slash == newpath) {
        snprintf(new_ppath, sizeof(new_ppath), "/");
        snprintf(new_name, sizeof(new_name), "%s", newpath + 1);
    } else {
        snprintf(new_ppath, sizeof(new_ppath), "%.*s", (int)(slash - newpath), newpath);
        snprintf(new_name, sizeof(new_name), "%s", slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, old_ppath, &old_pdir);
    if (ret < 0) return ret;
    ret = gefs_walk_path(ctx, &ctx->active_tree, new_ppath, &new_pdir);
    if (ret < 0) return ret;

    GefsMsg msgs[2];

    /* 1. Delete old Kent */
    msgs[0].op = GEFS_ODELETE;
    char *p = gefs_packdkey(kbuf_del, sizeof(kbuf_del), old_pdir.qid.path, old_name);
    msgs[0].k = kbuf_del;
    msgs[0].nk = p - kbuf_del;
    msgs[0].v = "";
    msgs[0].nv = 0;

    /* 2. Insert new Kent */
    d.name = new_name;
    d.qid.vers++;
    msgs[1].op = GEFS_OINSERT;
    p = gefs_packdkey(kbuf_ins, sizeof(kbuf_ins), new_pdir.qid.path, new_name);
    msgs[1].k = kbuf_ins;
    msgs[1].nk = p - kbuf_ins;
    p = gefs_packdval(vbuf_ins, sizeof(vbuf_ins), &d);
    msgs[1].v = vbuf_ins;
    msgs[1].nv = p - vbuf_ins;

    return gefs_btupsert(ctx, &ctx->active_tree, msgs, 2);
}

/* Chmod */
int
gefs_chmod_entry(GefsCtx *ctx, const char *path, mode_t mode)
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    d.mode = (d.mode & ~0777) | (mode & 0777);
    d.qid.vers++;

    GefsMsg msg;
    msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &d);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    return gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
}

/* Chown */
int
gefs_chown_entry(GefsCtx *ctx, const char *path, uid_t uid, gid_t gid)
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    if (uid != (uid_t)-1) d.uid = uid;
    if (gid != (gid_t)-1) d.gid = gid;
    d.qid.vers++;

    GefsMsg msg;
    msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &d);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    return gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
}

/* Utime / Utimens */
int
gefs_utimens_entry(GefsCtx *ctx, const char *path, const struct timespec tv[2])
{
    GefsXdir d, pdir;
    char parent_path[1024], name[256];
    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    int ret;

    ret = gefs_walk_path(ctx, &ctx->active_tree, path, &d);
    if (ret < 0) return ret;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == path) {
        snprintf(parent_path, sizeof(parent_path), "/");
        snprintf(name, sizeof(name), "%s", path + 1);
    } else {
        size_t plen = last_slash - path;
        snprintf(parent_path, sizeof(parent_path), "%.*s", (int)plen, path);
        snprintf(name, sizeof(name), "%s", last_slash + 1);
    }

    ret = gefs_walk_path(ctx, &ctx->active_tree, parent_path, &pdir);
    if (ret < 0) return ret;

    if (tv) {
        d.atime = (int64_t)tv[0].tv_sec * 1000000000LL + tv[0].tv_nsec;
        d.mtime = (int64_t)tv[1].tv_sec * 1000000000LL + tv[1].tv_nsec;
    } else {
        d.atime = (int64_t)time(NULL) * 1000000000LL;
        d.mtime = d.atime;
    }
    d.qid.vers++;

    GefsMsg msg;
    msg.op = GEFS_OINSERT;
    char *p = gefs_packdkey(kbuf, sizeof(kbuf), pdir.qid.path, name);
    msg.k = kbuf;
    msg.nk = p - kbuf;
    p = gefs_packdval(vbuf, sizeof(vbuf), &d);
    msg.v = vbuf;
    msg.nv = p - vbuf;

    return gefs_btupsert(ctx, &ctx->active_tree, &msg, 1);
}

/* Snapshot Creation */
int
gefs_create_snapshot(GefsCtx *ctx, const char *snap_name, int mutable_flag)
{
    if (ctx->readonly) return -EROFS;

    char kbuf[GEFS_KEYMAX], vbuf[GEFS_INLMAX];
    char tkbuf[GEFS_KEYMAX], tvbuf[GEFS_INLMAX];
    GefsTree new_snap;
    uint32_t flags = GEFS_LTSNAP | (mutable_flag ? GEFS_LMUT : 0);
    uint64_t new_gen = ctx->sb.nextgen++;

    memset(&new_snap, 0, sizeof(new_snap));
    new_snap.nref = 0;
    new_snap.nlbl = 1;
    new_snap.ht = ctx->active_tree.ht;
    new_snap.bp = ctx->active_tree.bp;
    new_snap.gen = new_gen;
    new_snap.pred = ctx->active_tree.gen;
    new_snap.succ = -1;

    GefsMsg msgs[2];

    /* 1. Insert Klabel{name} => snapid */
    char *p = gefs_packlbl(kbuf, sizeof(kbuf), snap_name);
    msgs[0].op = GEFS_OINSERT;
    msgs[0].k = kbuf;
    msgs[0].nk = p - kbuf;
    vbuf[0] = GEFS_KSNAP;
    PACK64(vbuf + 1, new_gen);
    PACK32(vbuf + 9, flags);
    msgs[0].v = vbuf;
    msgs[0].nv = 1 + 8 + 4;

    /* 2. Insert Ksnap{new_gen} => Tree */
    p = gefs_packsnap(tkbuf, sizeof(tkbuf), new_gen);
    msgs[1].op = GEFS_OINSERT;
    msgs[1].k = tkbuf;
    msgs[1].nk = p - tkbuf;

    char *vp = tvbuf;
    PACK32(vp, new_snap.nref); vp += 4;
    PACK32(vp, new_snap.nlbl); vp += 4;
    PACK32(vp, new_snap.ht);   vp += 4;
    PACK32(vp, new_snap.flag); vp += 4;
    PACK64(vp, new_snap.gen);  vp += 8;
    PACK64(vp, new_snap.pred); vp += 8;
    PACK64(vp, new_snap.succ); vp += 8;
    PACK64(vp, new_snap.base); vp += 8;
    gefs_packbp(vp, GEFS_PTRSZ, &new_snap.bp); vp += GEFS_PTRSZ;
    msgs[1].v = tvbuf;
    msgs[1].nv = vp - tvbuf;

    int ret = gefs_btupsert(ctx, &ctx->snap_tree, msgs, 2);
    if (ret < 0) return ret;

    gefs_sync_fs(ctx);
    return 0;
}

/* Snapshot Deletion */
int
gefs_delete_snapshot(GefsCtx *ctx, const char *snap_name)
{
    if (ctx->readonly) return -EROFS;
    if (strcmp(snap_name, "main") == 0 || strcmp(snap_name, "adm") == 0 || strcmp(snap_name, "empty") == 0)
        return -EPERM;

    char kbuf[GEFS_KEYMAX];
    char *p = gefs_packlbl(kbuf, sizeof(kbuf), snap_name);

    GefsMsg msg;
    msg.op = GEFS_ODELETE;
    msg.k = kbuf;
    msg.nk = p - kbuf;
    msg.v = "";
    msg.nv = 0;

    int ret = gefs_btupsert(ctx, &ctx->snap_tree, &msg, 1);
    if (ret < 0) return ret;

    gefs_sync_fs(ctx);
    return 0;
}
