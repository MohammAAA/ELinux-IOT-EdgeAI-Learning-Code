/*
 * 09_test_hello_cdev_with_ioctl.c - Userspace test for hello_cdev with ioctl
 *
 * Compile: gcc -Wall -Wextra -o ./bin/test_hello_cdev_with_ioctl 09_test_hello_cdev_with_ioctl.c
 * Run:     ./bin/test_hello_cdev_with_ioctl /dev/hello_cdev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Include the shared ioctl header */
#include "08_hello_cdev_with_ioctl.h"

#define DEVICE_PATH "/dev/hello_cdev"

#define PASS(fmt, ...) printf("  [PASS] " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) printf("  [FAIL] " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf("  [INFO] " fmt "\n", ##__VA_ARGS__)

static int fd = -1;
static int errors = 0;

static int read_counter(int *value)
{
    ssize_t ret = read(fd, value, sizeof(int));
    if (ret < 0) {
        printf("  [ERROR] read() failed: %s\n", strerror(errno));
        return -1;
    }
    if (ret != sizeof(int)) {
        FAIL("read() returned %zd bytes, expected %zu\n", ret, sizeof(int));
        return -1;
    }
    return 0;
}

static int write_counter(int value)
{
    ssize_t ret = write(fd, &value, sizeof(int));
    if (ret < 0) {
        printf("  [ERROR] write() failed: %s\n", strerror(errno));
        return -1;
    }
    if (ret != sizeof(int)) {
        FAIL("write() returned %zd bytes, expected %zu\n", ret, sizeof(int));
        return -1;
    }
    return 0;
}

/* ──────────────────────────────────────────────
 * ioctl Tests
 * ────────────────────────────────────────────── */

static void test_ioctl_get_count(void)
{
    int value;
    int ret;

    INFO("HELLO_GET_COUNT — read counter without incrementing\n");

    ret = ioctl(fd, HELLO_GET_COUNT, &value);
    if (ret < 0) {
        FAIL("ioctl(GET_COUNT) failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    /* Read the counter via read() to verify it increments */
    int after_read;
    if (read_counter(&after_read) == 0) {
        if (after_read == value + 1) {
            PASS("GET_COUNT=%d, next read()=%d (incremented correctly)\n",
                 value, after_read);
        } else {
            FAIL("GET_COUNT=%d, but next read()=%d (expected %d)\n",
                 value, after_read, value + 1);
            errors++;
        }
    }
}

static void test_ioctl_set_count(void)
{
    int value = 42;
    int ret;

    INFO("HELLO_SET_COUNT — set counter to 42\n");

    ret = ioctl(fd, HELLO_SET_COUNT, value);
    if (ret < 0) {
        FAIL("ioctl(SET_COUNT) failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    /* Verify via GET_COUNT */
    int verify;
    ret = ioctl(fd, HELLO_GET_COUNT, &verify);
    if (ret < 0) {
        FAIL("verify GET_COUNT failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    if (verify == 42) {
        PASS("SET_COUNT=42, GET_COUNT=%d (correct)\n", verify);
    } else {
        FAIL("SET_COUNT=42, but GET_COUNT=%d\n", verify);
        errors++;
    }
}

static void test_ioctl_reset_count(void)
{
    /* First, set a known value */
    int value = 999;
    int ret;

    INFO("HELLO_RESET_COUNT — reset counter to 0\n");

    ret = ioctl(fd, HELLO_SET_COUNT, value);
    if (ret < 0) {
        FAIL("pre-set failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    /* Reset */
    ret = ioctl(fd, HELLO_RESET_COUNT);
    if (ret < 0) {
        FAIL("ioctl(RESET_COUNT) failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    /* Verify */
    int verify;
    ret = ioctl(fd, HELLO_GET_COUNT, &verify);
    if (ret < 0) {
        FAIL("verify GET_COUNT failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    if (verify == 0) {
        PASS("RESET_COUNT succeeded, counter=%d\n", verify);
    } else {
        FAIL("RESET_COUNT failed, counter=%d (expected 0)\n", verify);
        errors++;
    }
}

static void test_ioctl_get_stats(void)
{
    struct hello_stats stats;
    int ret;

    INFO("HELLO_GET_STATS — read statistics\n");

    ret = ioctl(fd, HELLO_GET_STATS, &stats);
    if (ret < 0) {
        FAIL("ioctl(GET_STATS) failed: %s\n", strerror(errno));
        errors++;
        return;
    }

    PASS("Stats: counter=%d, reads=%d, writes=%d, ioctls=%d\n",
         stats.counter, stats.total_reads,
         stats.total_writes, stats.total_ioctls);

    /* Basic sanity checks */
    if (stats.total_reads < 0 || stats.total_writes < 0 || stats.total_ioctls < 0) {
        FAIL("Negative counter in stats\n");
        errors++;
    }
    if (stats.total_ioctls < 4) {
        FAIL("ioctl_count=%d but we've made at least 4 ioctl calls\n",
             stats.total_ioctls);
        errors++;
    }
}

static void test_ioctl_bad_direction(void)
{
    int ret;

    INFO("Wrong direction — pass a write command as read (should fail)\n");

    /*
     * We can't easily construct a "wrong direction" from userspace
     * because the macros encode the direction at compile time.
     * Instead, test with a garbage command number (wrong magic).
     */
    unsigned long garbage_cmd = _IO('Z', 99);  /* Different magic */
    ret = ioctl(fd, garbage_cmd, 0);
    if (ret < 0 && errno == ENOTTY) {
        PASS("Wrong magic correctly rejected with ENOTTY\n");
    } else if (ret == 0) {
        FAIL("Wrong magic should have been rejected\n");
        errors++;
    } else {
        FAIL("Wrong magic returned unexpected error: %s\n", strerror(errno));
        errors++;
    }
}

static void test_ioctl_set_negative(void)
{
    int value = -1;
    int ret;

    INFO("SET_COUNT with negative value (should fail with EINVAL)\n");

    ret = ioctl(fd, HELLO_SET_COUNT, value);
    if (ret < 0 && errno == EINVAL) {
        PASS("Negative value correctly rejected with EINVAL\n");
    } else if (ret == 0) {
        FAIL("Negative value should have been rejected\n");
        errors++;
    } else {
        FAIL("Unexpected error: %s\n", strerror(errno));
        errors++;
    }
}

/* ──────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *dev_path = DEVICE_PATH;
    int value=0;

    if (argc > 1)
        dev_path = argv[1];

    printf("=== hello_cdev ioctl test ===\n");
    printf("Device: %s\n\n", dev_path);

    /* Open */
    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        printf("  [ERROR] open() failed: %s\n", strerror(errno));
        return 1;
    }
    PASS("Opened fd=%d\n", fd);

    /* ── ioctl tests ── */
    printf("\n[ioctl Tests]\n");

    test_ioctl_reset_count();
    test_ioctl_get_count();
    test_ioctl_set_count();
    test_ioctl_reset_count();
    test_ioctl_get_stats();
    test_ioctl_bad_direction();
    test_ioctl_set_negative();

    /* ── Verify read still works after ioctl ── */
    printf("\n[Post-ioctl Read/Write Tests]\n");

    INFO("read() after ioctl RESET — should return 1\n");
    if (read_counter(&value) == 0 && value == 1) {
        PASS("read after reset: %d (expected 1)\n", value);
    } else {
        FAIL("read after reset: %d (expected 1)\n", value);
        errors++;
    }

    INFO("Final stats:\n");
    test_ioctl_get_stats();

    /* Close */
    printf("\n");
    close(fd);
    PASS("Device closed\n");

    /* Summary */
    printf("\n=== Results: ");
    if (errors == 0) {
        printf("ALL TESTS PASSED ===\n");
        return 0;
    } else {
        printf("%d TEST(S) FAILED ===\n", errors);
        return 1;
    }
}