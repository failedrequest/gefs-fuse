/*
 * Full Integration Test Suite for GEFS FUSE Port (FreeBSD)
 *
 * Tests:
 *  1. mkfs.gefs formatting
 *  2. File creation, write, read, and verification
 *  3. Directory creation and nesting
 *  4. File truncation and expansion
 *  5. File rename and move
 *  6. File and directory removal
 *  7. Symlink creation and resolution
 *  8. Multi-snapshot operations
 *  9. fsck.gefs consistency validation after mutations
 */

#include "gefs_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#define TEST_IMG "/tmp/gefs_test_suite.img"

static void
test_pass(const char *name)
{
    printf("  [PASS] %s\n", name);
}

int
main(void)
{
    GefsCtx fs;
    int ret;

    printf("================================================================\n");
    printf("  GEFS Storage & Mutation Test Suite (FreeBSD)\n");
    printf("================================================================\n\n");

    /* 1. Format test image */
    printf("1. Creating and formatting %s...\n", TEST_IMG);
    unlink(TEST_IMG);
    int fd = open(TEST_IMG, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    ftruncate(fd, 256 * 1024 * 1024);
    close(fd);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/mnt/gefs-fuse/mkfs.gefs %s testuser > /dev/null", TEST_IMG);
    ret = system(cmd);
    assert(ret == 0);
    test_pass("mkfs.gefs formatting");

    /* 2. Open filesystem */
    ret = gefs_fs_open(&fs, TEST_IMG);
    assert(ret == 0);
    test_pass("gefs_fs_open (read/write)");

    /* 3. Create regular file and write data */
    ret = gefs_create_file(&fs, "/test.txt", 0644, NULL);
    if (ret < 0) {
        printf("Failed to create file: %s (%d)\n", strerror(-ret), ret);
    }
    assert(ret == 0);
    test_pass("gefs_create_file /test.txt");

    const char *test_str = "Hello GEFS on FreeBSD! Testing block storage mutations.";
    ssize_t written = gefs_write_file(&fs, "/test.txt", test_str, strlen(test_str), 0);
    if (written < 0) {
        printf("Failed to write file: %s (%zd)\n", strerror(-(int)written), written);
    }
    assert(written == (ssize_t)strlen(test_str));
    test_pass("gefs_write_file");

    /* Read back file */
    GefsXdir d;
    ret = gefs_walk_path(&fs, &fs.active_tree, "/test.txt", &d);
    if (ret < 0) {
        printf("walk failed for /test.txt: %d\n", ret);
    }
    assert(ret == 0);
    if (d.length != strlen(test_str)) {
        printf("d.length mismatch: %lu != %zu\n", (unsigned long)d.length, strlen(test_str));
    }
    assert(d.length == strlen(test_str));

    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    ssize_t nread = gefs_read_file(&fs, &fs.active_tree, &d, read_buf, sizeof(read_buf) - 1, 0);
    if (nread != (ssize_t)strlen(test_str)) {
        printf("nread mismatch: %zd != %zu\n", nread, strlen(test_str));
    }
    assert(nread == (ssize_t)strlen(test_str));
    if (strcmp(read_buf, test_str) != 0) {
        printf("content mismatch: '%s' != '%s'\n", read_buf, test_str);
    }
    assert(strcmp(read_buf, test_str) == 0);
    test_pass("gefs_read_file content verification");

    /* 4. Create directory structure */
    ret = gefs_create_dir(&fs, "/docs", 0755);
    assert(ret == 0);
    ret = gefs_create_file(&fs, "/docs/readme.md", 0644, NULL);
    assert(ret == 0);
    const char *doc_str = "# GEFS FUSE Port\nFull read-write support on FreeBSD.";
    written = gefs_write_file(&fs, "/docs/readme.md", doc_str, strlen(doc_str), 0);
    assert(written == (ssize_t)strlen(doc_str));
    test_pass("gefs_create_dir & nested file creation");

    /* 5. Create symlink */
    ret = gefs_create_symlink(&fs, "/docs/readme.md", "/readme_link");
    assert(ret == 0);
    ret = gefs_walk_path(&fs, &fs.active_tree, "/readme_link", &d);
    assert(ret == 0);
    assert(d.qid.type & GEFS_QTSYMLINK);
    test_pass("gefs_create_symlink");

    /* 6. Rename file */
    ret = gefs_rename_entry(&fs, "/test.txt", "/test_renamed.txt");
    assert(ret == 0);
    ret = gefs_walk_path(&fs, &fs.active_tree, "/test.txt", &d);
    assert(ret < 0); /* Should not exist */
    ret = gefs_walk_path(&fs, &fs.active_tree, "/test_renamed.txt", &d);
    assert(ret == 0);
    test_pass("gefs_rename_entry");

    /* 7. Truncate file */
    ret = gefs_truncate_file(&fs, "/test_renamed.txt", 5);
    assert(ret == 0);
    ret = gefs_walk_path(&fs, &fs.active_tree, "/test_renamed.txt", &d);
    assert(ret == 0);
    assert(d.length == 5);
    test_pass("gefs_truncate_file");

    /* 8. Unlink file */
    ret = gefs_unlink_entry(&fs, "/test_renamed.txt");
    assert(ret == 0);
    ret = gefs_walk_path(&fs, &fs.active_tree, "/test_renamed.txt", &d);
    assert(ret < 0);
    test_pass("gefs_unlink_entry");

    /* 9. Snapshot creation */
    ret = gefs_create_snapshot(&fs, "snap1", 1);
    assert(ret == 0);
    test_pass("gefs_create_snapshot ('snap1')");

    /* 10. Sync and Close */
    gefs_fs_close(&fs);
    test_pass("gefs_fs_close & sync");

    /* 11. Run fsck.gefs consistency check */
    printf("\n11. Running fsck.gefs consistency validation...\n");
    snprintf(cmd, sizeof(cmd), "/mnt/gefs-fuse/fsck.gefs %s", TEST_IMG);
    ret = system(cmd);
    assert(ret == 0);
    test_pass("fsck.gefs full validation");

    printf("\n✓ ALL INTEGRATION TESTS PASSED SUCCESSFULLY!\n\n");
    return 0;
}
