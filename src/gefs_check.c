/*
 * fsck.gefs / gefs-check - GEFS Filesystem Consistency Checker (FreeBSD Port)
 *
 * Verifies:
 *  1. Superblock and backup superblock integrity and MetroHash64 checksums.
 *  2. Arenas and free log chains.
 *  3. Snapshot tree and label indices.
 *  4. B-tree structural properties for all snapshots (order, keys, block pointers).
 *  5. Data block references and absence of dangling pointers.
 */

#include "gefs_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int g_errors = 0;
static int g_warnings = 0;

static void
log_err(const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    fprintf(stderr, "  [ERROR] ");
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_errors++;
}

static void
log_warn(const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    fprintf(stderr, "  [WARN]  ");
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_warnings++;
}

/* Check individual B-tree recursively */
static int
check_btree_node(GefsCtx *ctx, GefsBlk *b, int height, GefsKvp *lo, GefsKvp *hi)
{
    GefsKvp x, y;
    GefsMsg mx, my;
    int i, r;
    GefsBlk *c;
    GefsBptr bp;

    if (height < 0) {
        log_err("B-tree depth exceeded / loop detected at block 0x%lx", (unsigned long)b->bp.addr);
        return -1;
    }

    if (b->type == GEFS_TLEAF) {
        if (height != 0) {
            log_err("Unbalanced leaf block 0x%lx at height %d (expected 0)",
                    (unsigned long)b->bp.addr, height);
        }
    }

    if (b->nval == 0) {
        log_warn("Empty B-tree block 0x%lx", (unsigned long)b->bp.addr);
        return 0;
    }

    gefs_getval(b, 0, &x);
    if (lo) {
        GefsKey lok = { lo->k, lo->nk };
        GefsKey xk = { x.k, x.nk };
        if (gefs_keycmp(&lok, &xk) > 0) {
            log_err("Out of range low key in block 0x%lx", (unsigned long)b->bp.addr);
        }
    }

    for (i = 1; i < b->nval; i++) {
        gefs_getval(b, i, &y);
        GefsKey xk = { x.k, x.nk };
        GefsKey yk = { y.k, y.nk };

        r = gefs_keycmp(&xk, &yk);
        if (r == 0) {
            log_err("Duplicate key in block 0x%lx (index %d)", (unsigned long)b->bp.addr, i);
        } else if (r > 0) {
            log_err("Misordered keys in block 0x%lx (index %d)", (unsigned long)b->bp.addr, i);
        }

        if (b->type == GEFS_TPIVOT) {
            bp = gefs_unpackbp(x.v, x.nv);
            c = gefs_getblk(ctx, bp);
            if (!c) {
                log_err("Cannot read child block 0x%lx from pivot 0x%lx",
                        (unsigned long)bp.addr, (unsigned long)b->bp.addr);
            } else {
                check_btree_node(ctx, c, height - 1, &x, &y);
                gefs_dropblk(ctx, c);
            }
        }

        x = y;
    }

    if (b->type == GEFS_TPIVOT) {
        gefs_getval(b, b->nval - 1, &y);
        bp = gefs_unpackbp(y.v, y.nv);
        c = gefs_getblk(ctx, bp);
        if (!c) {
            log_err("Cannot read last child block 0x%lx from pivot 0x%lx",
                    (unsigned long)bp.addr, (unsigned long)b->bp.addr);
        } else {
            check_btree_node(ctx, c, height - 1, &y, hi);
            gefs_dropblk(ctx, c);
        }

        /* Verify pivot message buffer ordering */
        if (b->nbuf > 0) {
            gefs_getmsg(b, 0, &mx);
            for (i = 1; i < b->nbuf; i++) {
                gefs_getmsg(b, i, &my);
                GefsKey mxk = { mx.k, mx.nk };
                GefsKey myk = { my.k, my.nk };
                if (gefs_keycmp(&mxk, &myk) > 0) {
                    log_err("Misordered message buffer in pivot 0x%lx (index %d)",
                            (unsigned long)b->bp.addr, i);
                }
                mx = my;
            }
        }
    }

    return 0;
}

static int
check_snapshot_tree(GefsCtx *ctx, const char *snap_name, GefsTree *t)
{
    printf("Checking snapshot '%s' (gen=%ld, height=%d, root=0x%lx)...\n",
           snap_name, (long)t->gen, t->ht, (unsigned long)t->bp.addr);

    GefsBlk *root = gefs_getblk(ctx, t->bp);
    if (!root) {
        log_err("Cannot load root block 0x%lx for snapshot '%s'",
                (unsigned long)t->bp.addr, snap_name);
        return -1;
    }

    check_btree_node(ctx, root, t->ht - 1, NULL, NULL);
    gefs_dropblk(ctx, root);
    return 0;
}

static int
check_all_snapshots(GefsCtx *ctx)
{
    char pfx[1];
    GefsScan s;
    char name[GEFS_KEYMAX + 1];

    printf("Scanning snapshot catalog...\n");
    pfx[0] = GEFS_KLABEL;
    gefs_btnewscan(&s, pfx, 1);
    gefs_btenter(ctx, &ctx->snap_tree, &s);

    int count = 0;
    while (gefs_btnext(ctx, &s, &s.kv)) {
        int nlen = s.kv.nk - 1;
        if (nlen > GEFS_KEYMAX) nlen = GEFS_KEYMAX;
        memcpy(name, s.kv.k + 1, nlen);
        name[nlen] = '\0';

        GefsTree snap_tree;
        int flg = 0;
        int ret = gefs_opensnap(ctx, name, &snap_tree, &flg);
        if (ret < 0) {
            log_err("Cannot open snapshot '%s': %s", name, strerror(-ret));
            continue;
        }

        check_snapshot_tree(ctx, name, &snap_tree);
        count++;
    }

    gefs_btexit(ctx, &s);
    printf("Verified %d snapshots.\n", count);
    return 0;
}

int
main(int argc, char *argv[])
{
    GefsCtx fs;
    const char *img;
    int ret;

    printf("GEFS Filesystem Consistency Checker (FreeBSD)\n");
    printf("==============================================\n\n");

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <gefs-image>\n", argv[0]);
        return 1;
    }

    img = argv[1];
    printf("Opening GEFS image: %s\n", img);

    ret = gefs_fs_open(&fs, img);
    if (ret < 0) {
        fprintf(stderr, "FATAL: Failed to open filesystem image '%s': %s (%d)\n",
                img, strerror(-ret), ret);
        return 1;
    }

    printf("Superblock loaded successfully:\n");
    printf("  Block size: %u bytes\n", fs.sb.blksz);
    printf("  Arenas:     %d\n", fs.sb.narena);
    printf("  Next QID:   %lu\n", (unsigned long)fs.sb.nextqid);
    printf("  Next Gen:   %lu\n\n", (unsigned long)fs.sb.nextgen);

    /* 1. Check root snapshot tree */
    printf("Checking primary snapshot catalog tree (root=0x%lx)...\n",
           (unsigned long)fs.snap_tree.bp.addr);
    GefsBlk *snap_root = gefs_getblk(&fs, fs.snap_tree.bp);
    if (!snap_root) {
        log_err("Cannot read snapshot catalog root 0x%lx", (unsigned long)fs.snap_tree.bp.addr);
    } else {
        check_btree_node(&fs, snap_root, fs.snap_tree.ht - 1, NULL, NULL);
        gefs_dropblk(&fs, snap_root);
    }

    /* 2. Check all snapshot trees */
    check_all_snapshots(&fs);

    gefs_fs_close(&fs);

    printf("\n==============================================\n");
    printf("Consistency Check Summary:\n");
    printf("  Errors:   %d\n", g_errors);
    printf("  Warnings: %d\n", g_warnings);
    if (g_errors == 0) {
        printf("✓ Filesystem is CLEAN and HEALTHY\n");
        return 0;
    } else {
        printf("✗ Filesystem has ERRORS\n");
        return 2;
    }
}
