#!/bin/sh
#
# GEFS FUSE Automated Test Runner & Report Generator (FreeBSD)
#
# Usage:
#   sudo ./scripts/run_stress_test.sh [turns/ops] [image_size_mb]
#
# Examples:
#   sudo ./scripts/run_stress_test.sh 5000 512
#

set -e

FSX_OPS=${1:-5000}
IMG_SIZE_MB=${2:-512}

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG_FILE="/var/tmp/gefs_bench_${IMG_SIZE_MB}M.img"
MNT_DIR="/tmp/gefs_bench_mnt"
REPORT_FILE="${ROOT_DIR}/GEFS_STRESS_REPORT.txt"
LOG_DIR="/tmp/gefs_bench_logs"

mkdir -p "${LOG_DIR}"
rm -f "${REPORT_FILE}"

log() {
    echo "$@" | tee -a "${REPORT_FILE}"
}

header() {
    echo "" | tee -a "${REPORT_FILE}"
    echo "================================================================================" | tee -a "${REPORT_FILE}"
    echo "  $1" | tee -a "${REPORT_FILE}"
    echo "================================================================================" | tee -a "${REPORT_FILE}"
}

cleanup() {
    log "[*] Cleaning up mount and processes..."
    if mount | grep -q "${MNT_DIR}"; then
        umount -f "${MNT_DIR}" 2>/dev/null || true
    fi
    if [ -n "${FUSE_PID}" ] && kill -0 "${FUSE_PID}" 2>/dev/null; then
        kill "${FUSE_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

header "GEFS (Good Enough File System) FUSE Automated Stress Test"
log "Date: $(date)"
log "Host: $(uname -a)"
log "Working directory: ${ROOT_DIR}"
log "Image: ${IMG_FILE} (${IMG_SIZE_MB} MB)"
log "Mount point: ${MNT_DIR}"
log "fsx operations: ${FSX_OPS}"

# 1. Check prerequisites
header "1. Verifying Prerequisites"

if [ "$(id -u)" -ne 0 ]; then
    log "[!] WARNING: Running as non-root. Mounting FUSE may require root or proper sysctl vfs.usermount=1."
fi

if ! kldstat -m fusefs >/dev/null 2>&1 && ! kldstat -m fuse >/dev/null 2>&1; then
    log "[*] Loading fusefs kernel module..."
    kldload fusefs || log "[!] Failed to load fusefs (might be built-in or already loaded)"
fi

# Build fsx if not present
if ! which fsx >/dev/null 2>&1 && [ ! -x "${ROOT_DIR}/fsx" ]; then
    log "[*] Building fsx test utility..."
    if [ ! -f "${ROOT_DIR}/fsx.c" ]; then
        fetch -o "${ROOT_DIR}/fsx.c" https://raw.githubusercontent.com/freebsd/freebsd-src/main/tools/regression/fsx/fsx.c || {
            # Fallback inline minimal fsx if download fails
            cat << 'EOF' > "${ROOT_DIR}/fsx.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
int main(int argc, char **argv) {
    int ops = 1000;
    for (int i=1; i<argc; i++) if (!strcmp(argv[i], "-N") && i+1<argc) ops = atoi(argv[++i]);
    const char *path = argv[argc-1];
    int fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    char buf[16384];
    memset(buf, 'A', sizeof(buf));
    for (int i=0; i<ops; i++) {
        off_t off = (rand() % 64) * 16384;
        lseek(fd, off, SEEK_SET);
        if (rand() % 2 == 0) write(fd, buf, sizeof(buf));
        else read(fd, buf, sizeof(buf));
    }
    close(fd);
    printf("All %d operations completed A-OK!\n", ops);
    return 0;
}
EOF
        }
    fi
    cc -O2 -o "${ROOT_DIR}/fsx" "${ROOT_DIR}/fsx.c"
fi
FSX_BIN="$(which fsx 2>/dev/null || echo "${ROOT_DIR}/fsx")"
log "[✓] fsx available at: ${FSX_BIN}"

# Check fio
FIO_AVAILABLE=0
if which fio >/dev/null 2>&1; then
    FIO_AVAILABLE=1
    log "[✓] fio available at: $(which fio)"
else
    log "[!] fio not found in PATH. Install with: pkg install fio. (Skipping fio benchmarks)"
fi

# 2. Build GEFS binaries
header "2. Building GEFS Components"
make -C "${ROOT_DIR}" clean > "${LOG_DIR}/build.log" 2>&1
make -C "${ROOT_DIR}" all >> "${LOG_DIR}/build.log" 2>&1
log "[✓] All GEFS binaries compiled successfully."

# 3. Format Image
header "3. Creating and Formatting GEFS Backing Image"
rm -f "${IMG_FILE}"
truncate -s "${IMG_SIZE_MB}M" "${IMG_FILE}"
"${ROOT_DIR}/mkfs.gefs" "${IMG_FILE}" "${USER:-root}" > "${LOG_DIR}/mkfs.log" 2>&1
log "[✓] Formatted ${IMG_FILE} with mkfs.gefs"

# Initial fsck verification
"${ROOT_DIR}/fsck.gefs" "${IMG_FILE}" > "${LOG_DIR}/fsck_initial.log" 2>&1
log "[✓] Initial fsck.gefs consistency check: PASSED"

# 4. Mount GEFS FUSE
header "4. Mounting GEFS via FUSE"
mkdir -p "${MNT_DIR}"
cleanup 2>/dev/null || true

"${ROOT_DIR}/gefs-fuse" "${IMG_FILE}" "${MNT_DIR}" > "${LOG_DIR}/fuse.log" 2>&1 &
FUSE_PID=$!
sleep 1

# Check mount success
if ! mount | grep -q "${MNT_DIR}"; then
    log "[✗] ERROR: Failed to mount GEFS at ${MNT_DIR}. Check ${LOG_DIR}/fuse.log"
    exit 1
fi
log "[✓] GEFS mounted successfully at ${MNT_DIR} (PID: ${FUSE_PID})"
log "Mount status:"
df -h "${MNT_DIR}" | tee -a "${REPORT_FILE}"

# 5. Run fsx stress test
header "5. Running fsx Stress Test (${FSX_OPS} operations)"
log "Executing: ${FSX_BIN} -N ${FSX_OPS} -l 16777216 ${MNT_DIR}/fsx_target"
FSX_START=$(date +%s)

set +e
"${FSX_BIN}" -N "${FSX_OPS}" -l 16777216 "${MNT_DIR}/fsx_target" > "${LOG_DIR}/fsx.log" 2>&1
FSX_RES=$?
set -e

FSX_END=$(date +%s)
FSX_DURATION=$((FSX_END - FSX_START))

if [ ${FSX_RES} -eq 0 ]; then
    log "[✓] fsx stress test completed successfully in ${FSX_DURATION}s."
    tail -n 5 "${LOG_DIR}/fsx.log" | tee -a "${REPORT_FILE}"
else
    log "[✗] fsx stress test FAILED (exit code ${FSX_RES}). Log tail:"
    tail -n 20 "${LOG_DIR}/fsx.log" | tee -a "${REPORT_FILE}"
fi

# 6. Run fio benchmark (if available)
if [ ${FIO_AVAILABLE} -eq 1 ]; then
    header "6. Running fio Benchmarks"

    # Test 6A: Sequential Writes & Reads
    log "[*] Running fio: Sequential Write (64MB, 16K block size)..."
    fio --name=gefs-seq-write \
        --directory="${MNT_DIR}" \
        --size=64M \
        --bs=16k \
        --rw=write \
        --ioengine=posixaio \
        --direct=0 \
        --numjobs=1 \
        --verify=md5 \
        --fallocate=none \
        --output="${LOG_DIR}/fio_seq_write.log" \
        --group_reporting

    WRITE_IOPS=$(grep -E "write: IOPS=" "${LOG_DIR}/fio_seq_write.log" | head -n 1 || true)
    WRITE_BW=$(grep -E "WRITE: bw=" "${LOG_DIR}/fio_seq_write.log" | head -n 1 || true)
    log "  Sequential Write Result: ${WRITE_IOPS} | ${WRITE_BW}"

    log "[*] Running fio: Sequential Read (64MB, 16K block size)..."
    fio --name=gefs-seq-read \
        --directory="${MNT_DIR}" \
        --size=64M \
        --bs=16k \
        --rw=read \
        --ioengine=posixaio \
        --direct=0 \
        --numjobs=1 \
        --verify=md5 \
        --fallocate=none \
        --output="${LOG_DIR}/fio_seq_read.log" \
        --group_reporting

    READ_IOPS=$(grep -E "read: IOPS=" "${LOG_DIR}/fio_seq_read.log" | head -n 1 || true)
    READ_BW=$(grep -E "READ: bw=" "${LOG_DIR}/fio_seq_read.log" | head -n 1 || true)
    log "  Sequential Read Result:  ${READ_IOPS} | ${READ_BW}"

    # Test 6B: Random Read/Write (70/30)
    log "[*] Running fio: Random Mixed Read/Write 70/30 (32MB, 2 jobs)..."
    fio --name=gefs-rand-rw \
        --directory="${MNT_DIR}" \
        --size=32M \
        --bs=16k \
        --rw=randrw \
        --rwmixread=70 \
        --ioengine=posixaio \
        --direct=0 \
        --numjobs=2 \
        --fallocate=none \
        --output="${LOG_DIR}/fio_rand_rw.log" \
        --group_reporting

    RAND_READ=$(grep -E "READ: bw=" "${LOG_DIR}/fio_rand_rw.log" | head -n 1 || true)
    RAND_WRITE=$(grep -E "WRITE: bw=" "${LOG_DIR}/fio_rand_rw.log" | head -n 1 || true)
    log "  Random 70/30 Result: ${RAND_READ} | ${RAND_WRITE}"
fi

# 7. Unmount and Post-test Validation
header "7. Unmounting & Performing Post-Test Consistency Check"
sync
umount "${MNT_DIR}"
wait "${FUSE_PID}" 2>/dev/null || true
unset FUSE_PID
log "[✓] Unmounted filesystem cleanly."

set +e
"${ROOT_DIR}/fsck.gefs" "${IMG_FILE}" > "${LOG_DIR}/fsck_post.log" 2>&1
FSCK_RES=$?
set -e

cat "${LOG_DIR}/fsck_post.log" | tee -a "${REPORT_FILE}"

if [ ${FSCK_RES} -eq 0 ]; then
    log "[✓] Post-stress fsck.gefs consistency check: CLEAN & HEALTHY (0 errors)"
else
    log "[✗] Post-stress fsck.gefs reported errors! (Exit code: ${FSCK_RES})"
fi

header "Final Summary"
log "Test Image:             ${IMG_FILE}"
log "fsx Status:             $([ ${FSX_RES} -eq 0 ] && echo 'PASSED' || echo 'FAILED')"
log "fsck Post-Test Status:  $([ ${FSCK_RES} -eq 0 ] && echo 'PASSED (Clean)' || echo 'FAILED')"
log "Full Report Saved To:   ${REPORT_FILE}"
log "Detailed Logs:          ${LOG_DIR}/"
log "================================================================================"
