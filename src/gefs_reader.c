/*
 * GEFS Storage Reader & Diagnostic Tool
 *
 * Inspects GEFS filesystem images: dumps superblock, snapshot tree,
 * lists directory entries, and displays file contents.
 */

#include "gefs_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
list_cb(const GefsXdir *d, void *arg)
{
    const char *indent = (const char *)arg;
    char typechar = '-';

    if (d->qid.type & GEFS_QTDIR)
        typechar = 'd';
    else if (d->qid.type & GEFS_QTSYMLINK)
        typechar = 'l';

    printf("%s%c %04o  %8lu B  qid=%lu  %s\n",
           indent,
           typechar,
           d->mode & 0777,
           (unsigned long)d->length,
           (unsigned long)d->qid.path,
           d->name);
    return 0;
}

int
main(int argc, char *argv[])
{
    GefsCtx fs;
    int ret;
    const char *img;
    const char *path = "/";

    if (argc < 2) {
        printf("Usage: %s <gefs-image> [path]\n", argv[0]);
        printf("  Dumps GEFS filesystem information and lists directory contents.\n");
        return 1;
    }

    img = argv[1];
    const char *snap_name = "main";
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            snap_name = argv[++i];
        } else {
            path = argv[i];
        }
    }

    printf("================================================================\n");
    printf("  GEFS Filesystem Diagnostic Reader\n");
    printf("  Image: %s\n", img);
    printf("================================================================\n\n");

    ret = gefs_fs_open(&fs, img);
    if (ret < 0) {
        fprintf(stderr, "Error: Cannot open GEFS image '%s': %s (%d)\n",
                img, strerror(-ret), ret);
        return 1;
    }

    if (strcmp(snap_name, "main") != 0) {
        int flg = 0;
        ret = gefs_opensnap(&fs, snap_name, &fs.active_tree, &flg);
        if (ret < 0) {
            fprintf(stderr, "Warning: Snapshot '%s' not found, using default\n", snap_name);
        } else {
            snprintf(fs.active_snap_name, sizeof(fs.active_snap_name), "%s", snap_name);
        }
    }

    printf("[Superblock]\n");
    printf("  Block Size:       %u bytes\n", fs.sb.blksz);
    printf("  Buffer Space:     %u bytes\n", fs.sb.bufspc);
    printf("  Arenas:           %d\n", fs.sb.narena);
    printf("  Snapshot Tree Ht: %d\n", fs.sb.snap.ht);
    printf("  Snapshot Root:    0x%lx (hash=0x%lx)\n",
           (unsigned long)fs.sb.snap.bp.addr, (unsigned long)fs.sb.snap.bp.hash);
    printf("  Next QID:         %lu\n", (unsigned long)fs.sb.nextqid);
    printf("  Next Gen:         %lu\n", (unsigned long)fs.sb.nextgen);
    printf("  Flags:            0x%lx\n", (unsigned long)fs.sb.flag);

    printf("\n[Active Snapshot]\n");
    printf("  Label:            '%s'\n", fs.active_snap_name);
    printf("  Generation:       %ld\n", (long)fs.active_tree.gen);
    printf("  Tree Height:      %d\n", fs.active_tree.ht);
    printf("  Root Block:       0x%lx (hash=0x%lx)\n",
           (unsigned long)fs.active_tree.bp.addr, (unsigned long)fs.active_tree.bp.hash);

    printf("\n[Path Lookup: '%s']\n", path);
    GefsXdir d;
    ret = gefs_walk_path(&fs, &fs.active_tree, path, &d);
    if (ret < 0) {
        printf("  Path not found: %s (%d)\n", strerror(-ret), ret);
    } else {
        printf("  Name:   '%s'\n", d.name ? d.name : "/");
        printf("  QID:    path=%lu, vers=%u, type=0x%02x\n",
               (unsigned long)d.qid.path, d.qid.vers, d.qid.type);
        printf("  Mode:   0%o (%s)\n",
               d.mode, (d.qid.type & GEFS_QTDIR) ? "directory" : "regular file");
        printf("  Length: %lu bytes\n", (unsigned long)d.length);
        printf("  UID:    %d, GID: %d\n", d.uid, d.gid);

        if (d.qid.type & GEFS_QTDIR) {
            printf("\n[Directory Listing: '%s']\n", path);
            gefs_list_dir(&fs, &fs.active_tree, d.qid.path, list_cb, "  ");
        } else if (d.length > 0 && d.length <= 4096) {
            char *buf = malloc(d.length + 1);
            if (buf) {
                ssize_t n = gefs_read_file(&fs, &fs.active_tree, &d, buf, d.length, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    printf("\n[File Content (%zd bytes)]:\n%s\n", n, buf);
                }
                free(buf);
            }
        }
    }

    gefs_fs_close(&fs);
    printf("\n✓ Diagnostic complete\n");
    return 0;
}
