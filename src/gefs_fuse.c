/*
 * GEFS FUSE Filesystem Driver (Read Operations)
 *
 * Implements high-level FUSE filesystem callbacks to mount and read
 * GEFS filesystem images on FreeBSD.
 */

#define FUSE_USE_VERSION 30

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "gefs_storage.h"

static GefsCtx g_fs;

/* FUSE getattr callback */
static int
gefs_fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    GefsXdir d;
    int ret;

    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    ret = gefs_walk_path(&g_fs, &g_fs.active_tree, path, &d);
    if (ret < 0)
        return -ENOENT;

    stbuf->st_ino = d.qid.path;
    stbuf->st_mode = gefs_mode_to_posix(d.mode, d.qid.type);
    stbuf->st_nlink = (stbuf->st_mode & S_IFDIR) ? 2 : 1;
    stbuf->st_size = d.length;
    stbuf->st_blksize = GEFS_BLKSZ;
    stbuf->st_blocks = (d.length + 511) / 512;
    stbuf->st_uid = (d.uid >= 0) ? (uid_t)d.uid : getuid();
    stbuf->st_gid = (d.gid >= 0) ? (gid_t)d.gid : getgid();

    if (d.atime > 0)
        stbuf->st_atime = (time_t)(d.atime / 1000000000LL);
    else
        stbuf->st_atime = time(NULL);

    if (d.mtime > 0)
        stbuf->st_mtime = (time_t)(d.mtime / 1000000000LL);
    else
        stbuf->st_mtime = stbuf->st_atime;

    stbuf->st_ctime = stbuf->st_mtime;

    return 0;
}

/* Context for directory fill */
typedef struct {
    void *buf;
    fuse_fill_dir_t filler;
} DirFillCtx;

static int
fill_dir_cb(const GefsXdir *d, void *arg)
{
    DirFillCtx *fc = (DirFillCtx *)arg;
    struct stat st;

    memset(&st, 0, sizeof(st));
    st.st_ino = d->qid.path;
    st.st_mode = gefs_mode_to_posix(d->mode, d->qid.type);
    st.st_size = d->length;

    if (fc->filler(fc->buf, d->name, &st, 0, 0))
        return 1; /* Buffer full */

    return 0;
}

/* FUSE readdir callback */
static int
gefs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi,
                  enum fuse_readdir_flags flags)
{
    GefsXdir d;
    int ret;
    DirFillCtx fc;

    (void)offset;
    (void)fi;
    (void)flags;

    ret = gefs_walk_path(&g_fs, &g_fs.active_tree, path, &d);
    if (ret < 0)
        return -ENOENT;

    if (!(d.qid.type & GEFS_QTDIR) && !(d.mode & GEFS_DMDIR))
        return -ENOTDIR;

    /* Add standard dot entries */
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* Enumerate directory entries from B-tree */
    fc.buf = buf;
    fc.filler = filler;
    gefs_list_dir(&g_fs, &g_fs.active_tree, d.qid.path, fill_dir_cb, &fc);

    return 0;
}

/* FUSE open callback */
static int
gefs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    GefsXdir d;
    int ret;

    ret = gefs_walk_path(&g_fs, &g_fs.active_tree, path, &d);
    if (ret < 0)
        return -ENOENT;

    if ((d.qid.type & GEFS_QTDIR) || (d.mode & GEFS_DMDIR))
        return -EISDIR;

    if ((fi->flags & O_TRUNC) && !g_fs.readonly) {
        gefs_truncate_file(&g_fs, path, 0);
    }

    return 0;
}

/* FUSE create callback */
static int
gefs_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    if (g_fs.readonly) return -EROFS;
    return gefs_create_file(&g_fs, path, mode, NULL);
}

/* FUSE write callback */
static int
gefs_fuse_write(const char *path, const char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
    (void)fi;
    if (g_fs.readonly) return -EROFS;
    ssize_t n = gefs_write_file(&g_fs, path, buf, size, offset);
    if (n < 0) return (int)n;
    return (int)n;
}

/* FUSE truncate callback */
static int
gefs_fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    if (g_fs.readonly) return -EROFS;
    return gefs_truncate_file(&g_fs, path, size);
}

/* FUSE mkdir callback */
static int
gefs_fuse_mkdir(const char *path, mode_t mode)
{
    if (g_fs.readonly) return -EROFS;
    return gefs_create_dir(&g_fs, path, mode);
}

/* FUSE unlink callback */
static int
gefs_fuse_unlink(const char *path)
{
    if (g_fs.readonly) return -EROFS;
    return gefs_unlink_entry(&g_fs, path);
}

/* FUSE rmdir callback */
static int
gefs_fuse_rmdir(const char *path)
{
    if (g_fs.readonly) return -EROFS;
    return gefs_rmdir_entry(&g_fs, path);
}

/* FUSE symlink callback */
static int
gefs_fuse_symlink(const char *target, const char *linkpath)
{
    if (g_fs.readonly) return -EROFS;
    return gefs_create_symlink(&g_fs, target, linkpath);
}

/* FUSE rename callback */
static int
gefs_fuse_rename(const char *oldpath, const char *newpath, unsigned int flags)
{
    (void)flags;
    if (g_fs.readonly) return -EROFS;
    return gefs_rename_entry(&g_fs, oldpath, newpath);
}

/* FUSE chmod callback */
static int
gefs_fuse_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    if (g_fs.readonly) return -EROFS;
    return gefs_chmod_entry(&g_fs, path, mode);
}

/* FUSE chown callback */
static int
gefs_fuse_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    (void)fi;
    if (g_fs.readonly) return -EROFS;
    return gefs_chown_entry(&g_fs, path, uid, gid);
}

/* FUSE utimens callback */
static int
gefs_fuse_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    (void)fi;
    if (g_fs.readonly) return -EROFS;
    return gefs_utimens_entry(&g_fs, path, tv);
}

/* FUSE fsync callback */
static int
gefs_fuse_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)isdatasync;
    (void)fi;
    if (g_fs.readonly) return 0;
    return gefs_sync_fs(&g_fs);
}

/* FUSE read callback */
static int
gefs_fuse_read(const char *path, char *buf, size_t size, off_t offset,
               struct fuse_file_info *fi)
{
    GefsXdir d;
    int ret;

    (void)fi;

    ret = gefs_walk_path(&g_fs, &g_fs.active_tree, path, &d);
    if (ret < 0)
        return -ENOENT;

    if ((d.qid.type & GEFS_QTDIR) || (d.mode & GEFS_DMDIR))
        return -EISDIR;

    ssize_t nread = gefs_read_file(&g_fs, &g_fs.active_tree, &d, buf, size, offset);
    if (nread < 0)
        return -EIO;

    return (int)nread;
}

/* FUSE readlink callback */
static int
gefs_fuse_readlink(const char *path, char *buf, size_t size)
{
    GefsXdir d;
    int ret;

    ret = gefs_walk_path(&g_fs, &g_fs.active_tree, path, &d);
    if (ret < 0)
        return -ENOENT;

    if (!(d.qid.type & GEFS_QTSYMLINK))
        return -EINVAL;

    ssize_t nread = gefs_read_file(&g_fs, &g_fs.active_tree, &d, buf, size - 1, 0);
    if (nread < 0)
        return -EIO;

    buf[nread] = '\0';
    return 0;
}

/* FUSE statfs callback */
static int
gefs_fuse_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(struct statvfs));

    stbuf->f_bsize = GEFS_BLKSZ;
    stbuf->f_frsize = GEFS_BLKSZ;
    stbuf->f_blocks = g_fs.sb.narena * (4096ULL * 1024 * 1024 / GEFS_BLKSZ);
    stbuf->f_bfree = stbuf->f_blocks / 2; /* Approximate */
    stbuf->f_bavail = stbuf->f_bfree;
    stbuf->f_files = g_fs.sb.nextqid;
    stbuf->f_ffree = 1000000;
    stbuf->f_namemax = GEFS_KEYMAX - 9 - 1;

    return 0;
}

/* Operations structure */
static const struct fuse_operations gefs_oper = {
    .getattr    = gefs_fuse_getattr,
    .readdir    = gefs_fuse_readdir,
    .open       = gefs_fuse_open,
    .create     = gefs_fuse_create,
    .read       = gefs_fuse_read,
    .write      = gefs_fuse_write,
    .truncate   = gefs_fuse_truncate,
    .mkdir      = gefs_fuse_mkdir,
    .unlink     = gefs_fuse_unlink,
    .rmdir      = gefs_fuse_rmdir,
    .symlink    = gefs_fuse_symlink,
    .rename     = gefs_fuse_rename,
    .chmod      = gefs_fuse_chmod,
    .chown      = gefs_fuse_chown,
    .utimens    = gefs_fuse_utimens,
    .fsync      = gefs_fuse_fsync,
    .readlink   = gefs_fuse_readlink,
    .statfs     = gefs_fuse_statfs,
};

static void
usage(const char *prog)
{
    fprintf(stderr, "GEFS (Good Enough File System) FUSE Driver (FreeBSD)\n");
    fprintf(stderr, "Usage: %s <gefs-image> <mountpoint> [fuse-options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s <snapshot>  Snapshot to mount (default: main)\n");
    fprintf(stderr, "  -f             Foreground mode\n");
    fprintf(stderr, "  -d             Debug mode\n");
    fprintf(stderr, "  -o allow_other Allow other users to access\n\n");
}

int
main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    const char *image_path = NULL;
    const char *snap_name = "main";
    int ret, i;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    image_path = argv[1];

    /* Parse snapshot override if specified */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            snap_name = argv[++i];
        }
    }

    printf("GEFS FUSE: Opening '%s'...\n", image_path);
    ret = gefs_fs_open(&g_fs, image_path);
    if (ret < 0) {
        fprintf(stderr, "Error: Cannot open GEFS image '%s': %s (%d)\n",
                image_path, strerror(-ret), ret);
        return 1;
    }

    /* Switch snapshot if requested */
    if (strcmp(snap_name, "main") != 0) {
        int flg = 0;
        ret = gefs_opensnap(&g_fs, snap_name, &g_fs.active_tree, &flg);
        if (ret < 0) {
            fprintf(stderr, "Warning: Snapshot '%s' not found, using default\n", snap_name);
        } else {
            snprintf(g_fs.active_snap_name, sizeof(g_fs.active_snap_name), "%s", snap_name);
        }
    }

    printf("GEFS FUSE: Superblock loaded:\n");
    printf("  Block size: %u bytes\n", g_fs.sb.blksz);
    printf("  Arenas: %d\n", g_fs.sb.narena);
    printf("  Active snapshot: '%s' (gen=%ld, tree_height=%d)\n",
           g_fs.active_snap_name, (long)g_fs.active_tree.gen, g_fs.active_tree.ht);

    /* Construct FUSE argument list: argv[0], mountpoint, remaining options */
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, argv[2]); /* mountpoint */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            i++; /* skip snap option and its value */
            continue;
        }
        fuse_opt_add_arg(&args, argv[i]);
    }

    printf("GEFS FUSE: Mounting on '%s'...\n\n", argv[2]);
    ret = fuse_main(args.argc, args.argv, &gefs_oper, NULL);

    gefs_fs_close(&g_fs);
    fuse_opt_free_args(&args);

    return ret;
}
