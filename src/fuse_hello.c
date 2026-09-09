/*
 * GEFS FUSE Port - Hello World FUSE Filesystem
 * 
 * A minimal FUSE filesystem for testing and initial development
 * Uses FUSE high-level API which is simpler and more portable
 */

#define FUSE_USE_VERSION 30

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

/* Test data */
#define HELLO_DATA "Hello from GEFS FUSE!\n"
#define README_DATA "This is a test GEFS FUSE filesystem.\n"

static int hello_getattr(const char *path, struct stat *stbuf,
                         struct fuse_file_info *fi) {
    (void)fi;
    
    memset(stbuf, 0, sizeof(struct stat));
    
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    
    if (strcmp(path, "/hello.txt") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(HELLO_DATA);
        return 0;
    }
    
    if (strcmp(path, "/README") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(README_DATA);
        return 0;
    }
    
    return -ENOENT;
}

static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFREG | 0444;
    filler(buf, "hello.txt", &st, 0, 0);
    filler(buf, "README", &st, 0, 0);
    
    return 0;
}

static int hello_open(const char *path, struct fuse_file_info *fi) {
    if (strcmp(path, "/hello.txt") != 0 && strcmp(path, "/README") != 0)
        return -ENOENT;
    
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EACCES;
    
    return 0;
}

static int hello_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void)fi;
    const char *data = NULL;
    size_t data_len = 0;
    
    if (strcmp(path, "/hello.txt") == 0) {
        data = HELLO_DATA;
        data_len = strlen(HELLO_DATA);
    } else if (strcmp(path, "/README") == 0) {
        data = README_DATA;
        data_len = strlen(README_DATA);
    } else {
        return -ENOENT;
    }
    
    if (offset >= (off_t)data_len)
        return 0;
    
    size_t len = data_len - offset;
    if (len > size)
        len = size;
    
    memcpy(buf, data + offset, len);
    return len;
}

static const struct fuse_operations hello_oper = {
    .getattr = hello_getattr,
    .readdir = hello_readdir,
    .open = hello_open,
    .read = hello_read,
};

int main(int argc, char *argv[]) {
    printf("GEFS FUSE Hello World Filesystem\n");
    printf("================================\n");
    printf("Version: 0.1 (Test)\n");
    printf("Status: Running\n\n");
    
    printf("Files:\n");
    printf("  /hello.txt  (read-only)\n");
    printf("  /README     (read-only)\n\n");
    
    return fuse_main(argc, argv, &hello_oper, NULL);
}
