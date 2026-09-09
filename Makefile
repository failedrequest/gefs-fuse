# Makefile for GEFS FUSE Port
# Builds the FUSE filesystem and utilities

CC = cc
CFLAGS = -Wall -Wextra -O2
FUSE_CFLAGS = -I/usr/local/include
FUSE_LIBS = -L/usr/local/lib -lfuse3 -Wl,-rpath=/usr/local/lib

# Targets
TARGETS = gefs-fuse gefs-hello gefs-reader mkfs.gefs fsck.gefs gefs-test

# Source files
GEFS_STORAGE_SRC = src/gefs_storage.c
GEFS_FUSE_SRC = src/gefs_fuse.c $(GEFS_STORAGE_SRC)
GEFS_HELLO_SRC = src/fuse_hello.c
GEFS_READER_SRC = src/gefs_reader.c $(GEFS_STORAGE_SRC)
GEFS_REAM_SRC = src/gefs_ream.c $(GEFS_STORAGE_SRC)
GEFS_CHECK_SRC = src/gefs_check.c $(GEFS_STORAGE_SRC)
GEFS_TEST_SRC = src/gefs_test.c $(GEFS_STORAGE_SRC)

.PHONY: all clean test install

# Default target
all: $(TARGETS)

# FUSE Hello World - simple test filesystem
gefs-hello: $(GEFS_HELLO_SRC)
	@echo "Building gefs-hello..."
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $^ $(FUSE_LIBS)
	@echo "✓ Built: $@"

# Main GEFS FUSE filesystem
gefs-fuse: $(GEFS_FUSE_SRC)
	@echo "Building gefs-fuse..."
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -I. -o $@ $^ $(FUSE_LIBS)
	@echo "✓ Built: $@"

# GEFS storage reader - test/debug utility
gefs-reader: $(GEFS_READER_SRC)
	@echo "Building gefs-reader..."
	$(CC) $(CFLAGS) -I. -o $@ $^
	@echo "✓ Built: $@"

# mkfs.gefs - GEFS formatter
mkfs.gefs: $(GEFS_REAM_SRC)
	@echo "Building mkfs.gefs..."
	$(CC) $(CFLAGS) -I. -o $@ $^
	@echo "✓ Built: $@"

# fsck.gefs - GEFS consistency checker
fsck.gefs: $(GEFS_CHECK_SRC)
	@echo "Building fsck.gefs..."
	$(CC) $(CFLAGS) -I. -o $@ $^
	@echo "✓ Built: $@"

# gefs-test - Storage and mutation test runner
gefs-test: $(GEFS_TEST_SRC)
	@echo "Building gefs-test..."
	$(CC) $(CFLAGS) -I. -o $@ $^
	@echo "✓ Built: $@"

# Run automated test suite
test: gefs-test mkfs.gefs fsck.gefs
	@echo "Running GEFS Automated Test Suite..."
	./gefs-test

# Run full stress test script (fsx + fio + report generation)
stress: all
	@echo "Running GEFS full stress test..."
	./scripts/run_stress_test.sh
	@echo "=========================================="
	@echo ""
	@echo "Create test mount point:"
	@echo "  mkdir -p /tmp/gefs-test"
	@echo ""
	@echo "Mount the filesystem:"
	@echo "  ./gefs-hello /tmp/gefs-test"
	@echo ""
	@echo "In another terminal, test it:"
	@echo "  ls -la /tmp/gefs-test"
	@echo "  cat /tmp/gefs-test/hello.txt"
	@echo ""
	@echo "Unmount:"
	@echo "  fusermount -u /tmp/gefs-test"
	@echo ""

# Install (optional)
install: $(TARGETS)
	@echo "Installing to /usr/local/bin..."
	mkdir -p /usr/local/bin
	cp $(TARGETS) /usr/local/bin/
	@echo "✓ Installation complete"

# Clean up
clean:
	@echo "Cleaning up..."
	rm -f $(TARGETS)
	rm -f *.o
	@echo "✓ Clean complete"

# Help
help:
	@echo "GEFS FUSE Makefile"
	@echo "=================="
	@echo ""
	@echo "Targets:"
	@echo "  make all      - Build all targets (default)"
	@echo "  make gefs-hello - Build hello world FUSE filesystem"
	@echo "  make gefs-fuse  - Build main GEFS FUSE filesystem"
	@echo "  make test     - Show test instructions"
	@echo "  make install  - Install to /usr/local/bin"
	@echo "  make clean    - Remove built binaries"
	@echo "  make help     - Show this help"
	@echo ""

# Show variables
vars:
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "FUSE_CFLAGS: $(FUSE_CFLAGS)"
	@echo "FUSE_LIBS: $(FUSE_LIBS)"
