/*
 * 07_test_hello_cdev.c - Userspace test program for hello_cdev driver
 *
 * Tests:
 *   1. Open the device
 *   2. Read 5 times — verify counter increments monotonically
 *   3. Write value 0 — reset counter
 *   4. Read again — verify counter starts from 1
 *   5. Write value 100 — set counter to arbitrary value
 *   6. Read — verify counter continues from 101
 *   7. Close the device
 *
 * Compile:
 *   gcc -Wall -Wextra -o ./bin/test_hello_cdev 07_test_hello_cdev.c
 *
 * Run:
 *   ./bin/test_hello_cdev /dev/hello_cdev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DEVICE_PATH "/dev/hello_cdev"
#define NUM_INITIAL_READS 5

/* Print a colored status message */
#define PASS(fmt, ...) printf("  [PASS] " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) printf("  [FAIL] " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf("  [INFO] " fmt "\n", ##__VA_ARGS__)

static int fd = -1;

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

int main(int argc, char *argv[])
{
    const char *dev_path = DEVICE_PATH;
    int value;
    int expected;
    int errors = 0;

    if (argc > 1)
        dev_path = argv[1];

    printf("=== hello_cdev test ===\n");
    printf("Device: %s\n\n", dev_path);

    /* --- Test 1: Open --- */
    INFO("Opening %s ...", dev_path);
    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        printf("  [ERROR] open() failed: %s\n", strerror(errno));
        printf("  Hint: Is the module loaded? Run: lsmod | grep hello_cdev\n");
        printf("  Hint: Does the device node exist? Run: ls -l %s\n", dev_path);
        return 1;
    }
    PASS("Opened fd=%d\n", fd);

    /* --- Test 2: Read 5 times, verify monotonic increment --- */
    /* This test assumes the module has not been read before (i.e.: the module has just been loaded)*/
    printf("\n[Test] Monotonic counter — reading %d times:\n", NUM_INITIAL_READS);
    for (int i = 0; i < NUM_INITIAL_READS; i++) {
        expected = i + 1;  /* First read should return 1 */
        if (read_counter(&value) < 0) {
            errors++;
            continue;
        }
        if (value == expected) {
            PASS("Read #%d: got %d (expected %d)\n", i + 1, value, expected);
        } else {
            FAIL("Read #%d: got %d (expected %d)\n", i + 1, value, expected);
            errors++;
        }
    }

    /* --- Test 3: Write 0 to reset counter --- */
    printf("\n[Test] Reset counter to 0:\n");
    if (write_counter(0) < 0) {
        errors++;
    } else {
        PASS("Wrote 0 (counter reset)\n");
    }

    /* --- Test 4: Read after reset — should be 1 --- */
    printf("\n[Test] Read after reset:\n");
    if (read_counter(&value) < 0) {
        errors++;
    } else if (value == 1) {
        PASS("Read after reset: got %d (expected 1)\n", value);
    } else {
        FAIL("Read after reset: got %d (expected 1)\n", value);
        errors++;
    }

    /* --- Test 5: Write arbitrary value (100) --- */
    printf("\n[Test] Set counter to 100:\n");
    if (write_counter(100) < 0) {
        errors++;
    } else {
        PASS("Wrote 100\n");
    }

    /* --- Test 6: Read — should be 101 (100 + 1 from atomic_inc) --- */
    printf("\n[Test] Read after setting to 100:\n");
    if (read_counter(&value) < 0) {
        errors++;
    } else if (value == 101) {
        PASS("Read: got %d (expected 101)\n", value);
    } else {
        FAIL("Read: got %d (expected 101)\n", value);
        errors++;
    }

    /* --- Test 7: Small read buffer — should fail --- */
    printf("\n[Test] Read with undersized buffer (should fail):\n");
    {
        char small_buf[1];
        ssize_t ret = read(fd, small_buf, 1);
        if (ret < 0 && errno == EINVAL) {
            PASS("Small read correctly rejected with EINVAL\n");
        } else if (ret > 0) {
            FAIL("Small read should have failed but got %zd bytes\n", ret);
            errors++;
        } else {
            FAIL("Small read returned unexpected error: %s\n", strerror(errno));
            errors++;
        }
    }

    /* --- Test 8: Small write buffer — should fail --- */
    printf("\n[Test] Write with undersized buffer (should fail):\n");
    {
        char small_buf[1] = {42};
        ssize_t ret = write(fd, small_buf, 1);
        if (ret < 0 && errno == EINVAL) {
            PASS("Small write correctly rejected with EINVAL\n");
        } else if (ret > 0) {
            FAIL("Small write should have failed but got %zd bytes\n", ret);
            errors++;
        } else {
            FAIL("Small write returned unexpected error: %s\n", strerror(errno));
            errors++;
        }
    }

    /* --- Close --- */
    printf("\n");
    close(fd);
    PASS("Device closed\n");

    /* --- Summary --- */
    printf("\n=== Results: ");
    if (errors == 0) {
        printf("ALL TESTS PASSED ===\n");
        return 0;
    } else {
        printf("%d TEST(S) FAILED ===\n", errors);
        return 1;
    }
}