/*
 * mpu6050_test.c — Userspace I2C test for MPU-6050
 *
 * Verifies I2C communication with the MPU-6050 sensor via /dev/i2c-1.
 * Reads WHO_AM_I, initializes the sensor, and reads sensor data in a loop.
 *
 * Usage: ./bin/mpu6050_test [i2c_bus] [sample_count]
 *   i2c_bus     : I2C bus number (default: 1)
 *   sample_count: Number of samples to read (default: 20)
 *
 * Build: gcc -o bin/mpu6050_test 01_mpu6050_test.c -li2c
 *
 */
#define _POSIX_C_SOURCE 199309L  // Enables nanosleep functionality

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h> // to use nanosleep()
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>

/* ─── MPU-6050 Register Map ─── */
#define MPU6050_REG_SMPLRT_DIV     0x19
#define MPU6050_REG_CONFIG         0x1A
#define MPU6050_REG_GYRO_CONFIG    0x1B
#define MPU6050_REG_ACCEL_CONFIG   0x1C
#define MPU6050_REG_PWR_MGMT_1     0x6B
#define MPU6050_REG_PWR_MGMT_2     0x6C
#define MPU6050_REG_WHO_AM_I       0x75

/* Data registers — 14 consecutive bytes starting at 0x3B */
#define MPU6050_REG_ACCEL_XOUT_H   0x3B
#define MPU6050_DATA_LEN           14

/* WHO_AM_I expected value */
#define MPU6050_WHO_AM_I           0x68

/* Accelerometer range: ±2g (sensitivity: 16384 LSB/g) */
#define ACCEL_SCALE  16384.0

/* Gyroscope range: ±250°/s (sensitivity: 131 LSB/(°/s)) */
#define GYRO_SCALE   131.0

/* Temperature formula: °C = raw/340.0 + 36.53 */
#define TEMP_SCALE   340.0
#define TEMP_OFFSET  36.53

/* Default I2C address */
#define MPU6050_ADDR 0x68

/* ─── Sensor data structure ─── */
struct mpu6050_data {
    int16_t accel_x, accel_y, accel_z;
    int16_t temp;
    int16_t gyro_x, gyro_y, gyro_z;
};

/* ─── Function prototypes ─── */
static int  mpu6050_open_bus(const char *bus_path);
static int  mpu6050_check_who_am_i(int fd);
static int  mpu6050_init(int fd);
static int  mpu6050_read_data(int fd, struct mpu6050_data *data);
static void mpu6050_print_data(const struct mpu6050_data *data, int sample_num);
static void mpu6050_sleep_ms(unsigned int milliseconds);

/* ─── Main ─── */
int main(int argc, char *argv[])
{
    int fd;
    int bus = 1;
    int sample_count = 20;
    struct mpu6050_data data;

    /* Parse optional arguments */
    if (argc >= 2)
        bus = atoi(argv[1]);
    if (argc >= 3)
        sample_count = atoi(argv[2]);

    if (sample_count <= 0) {
        fprintf(stderr, "Sample count must be > 0\n");
        return 1;
    }

    /* Open I2C bus */
    char bus_path[32];
    snprintf(bus_path, sizeof(bus_path), "/dev/i2c-%d", bus);

    printf("MPU-6050 Userspace Test\n");
    printf("=======================\n");
    printf("Bus: %s | Address: 0x%02x | Samples: %d\n\n",
           bus_path, MPU6050_ADDR, sample_count);

    fd = mpu6050_open_bus(bus_path);
    if (fd < 0)
        return 1;

    /* Step 1: Verify the device */
    if (mpu6050_check_who_am_i(fd) < 0) {
        close(fd);
        return 1;
    }

    /* Step 2: Initialize the sensor */
    if (mpu6050_init(fd) < 0) {
        close(fd);
        return 1;
    }

    /* Step 3: Read sensor data in a loop */
    printf("Reading %d samples at ~10 Hz...\n\n",sample_count);
    printf("%-6s  %8s %8s %8s  %8s  %8s %8s %8s\n",
           "#", "Ax(g)", "Ay(g)", "Az(g)",
           "Temp(°C)", "Gx(°/s)", "Gy(°/s)", "Gz(°/s)");
    printf("──────  ──────── ──────── ────────  ────────  ──────── ──────── ────────\n");

    int success_count = 0;
    int fail_count = 0;

    for (int i = 0; i < sample_count; i++) {
        if (mpu6050_read_data(fd, &data) < 0) {
            fprintf(stderr, "Sample %d: READ FAILED\n", i + 1);
            fail_count++;
            mpu6050_sleep_ms(100);
            continue;
        }
        mpu6050_print_data(&data, i + 1);
        success_count++;
        mpu6050_sleep_ms(100);  /* ~10 Hz */
    }

    printf("\n");
    printf("Results: %d success, %d failed out of %d samples\n",
           success_count, fail_count, sample_count);

    /* Step 4: Put sensor back to sleep (optional, good practice) */
    if (i2c_smbus_write_byte_data(fd, MPU6050_REG_PWR_MGMT_1, 0x40) < 0) {
        fprintf(stderr, "Warning: failed to put sensor to sleep\n");
    } else {
        printf("Sensor put to sleep.\n");
    }

    close(fd);
    return (fail_count > 0) ? 1 : 0;
}

/* ─── Open I2C bus and set slave address ─── */
static int mpu6050_open_bus(const char *bus_path)
{
    int fd;

    fd = open(bus_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", bus_path, strerror(errno));
        fprintf(stderr, "  → Is I2C enabled? Run: sudo raspi-config\n");
        fprintf(stderr, "  → Is i2c-dev loaded? Run: sudo modprobe i2c-dev\n");
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, MPU6050_ADDR) < 0) {
        fprintf(stderr, "Error: cannot set slave address 0x%02x: %s\n",
                MPU6050_ADDR, strerror(errno));
        close(fd);
        return -1;
    }

    printf("Opened %s, set slave address 0x%02x\n", bus_path, MPU6050_ADDR);
    return fd;
}

/* ─── Verify the device via WHO_AM_I register ─── */
static int mpu6050_check_who_am_i(int fd)
{
    __s32 who_am_i;

    who_am_i = i2c_smbus_read_byte_data(fd, MPU6050_REG_WHO_AM_I);
    if (who_am_i < 0) {
        fprintf(stderr, "Error: WHO_AM_I read failed: %s\n", strerror(-who_am_i));
        fprintf(stderr, "  → Check I2C wiring (SDA/SCL)\n");
        fprintf(stderr, "  → Check sensor power (VCC/GND)\n");
        return -1;
    }

    printf("WHO_AM_I: 0x%02x ", who_am_i);

    switch (who_am_i) {
    case 0x68:
        printf("(MPU-6050 confirmed ✓)\n");
        return 0;
    case 0x70:
        printf("(MPU-6500 detected — variant, should work)\n");
        return 0;
    case 0x71:
        printf("(MPU-9250 detected — different device!)\n");
        fprintf(stderr, "Error: unexpected device\n");
        return -1;
    case 0xFF:
        printf("(0xFF — NO DEVICE or bus error)\n");
        fprintf(stderr, "Error: no device at address 0x%02x\n", MPU6050_ADDR);
        fprintf(stderr, "  → Check wiring: SDA↔GPIO2(pin3), SCL↔GPIO3(pin5)\n");
        fprintf(stderr, "  → Check pull-up resistors\n");
        fprintf(stderr, "  → Run: sudo i2cdetect -y 1\n");
        return -1;
    case 0x00:
        printf("(0x00 — possible bus contention)\n");
        fprintf(stderr, "Error: device returning 0x00\n");
        return -1;
    default:
        printf("(unknown device, expected 0x68)\n");
        fprintf(stderr, "Warning: unexpected WHO_AM_I value\n");
        return -1;
    }
}

/* ─── Initialize the MPU-6050 ─── */
static int mpu6050_init(int fd)
{
    int ret;

    printf("Initializing MPU-6050...\n");

    /* 1. Wake up: clear sleep bit in PWR_MGMT_1 */
    ret = i2c_smbus_write_byte_data(fd, MPU6050_REG_PWR_MGMT_1, 0x00);
    if (ret < 0) {
        fprintf(stderr, "Error: PWR_MGMT_1 write failed: %s\n", strerror(-ret));
        return -1;
    }
    printf("  PWR_MGMT_1 = 0x00 (woken up, clock = internal 8MHz)\n");

    /* Wait for oscillator to stabilize */
    mpu6050_sleep_ms(100);

    /* 2. Set sample rate divider: Sample Rate = Gyro Rate / (1 + SMPLRT_DIV)
     *    With DLPF enabled (CONFIG ≠ 0 or 7), Gyro Rate = 1kHz
     *    SMPLRT_DIV = 7 → 1kHz / (1+7) = 125 Hz */
    ret = i2c_smbus_write_byte_data(fd, MPU6050_REG_SMPLRT_DIV, 0x07);
    if (ret < 0) {
        fprintf(stderr, "Error: SMPLRT_DIV write failed\n");
        return -1;
    }
    printf("  SMPLRT_DIV = 0x07 (125 Hz sample rate)\n");

    /* 3. Configure Digital Low Pass Filter (DLPF)
     *    CONFIG = 0x06 → Bandwidth 5Hz, delay 18.6ms, Fs=1kHz */
    ret = i2c_smbus_write_byte_data(fd, MPU6050_REG_CONFIG, 0x06);
    if (ret < 0) {
        fprintf(stderr, "Error: CONFIG write failed\n");
        return -1;
    }
    printf("  CONFIG = 0x06 (DLPF 5Hz bandwidth)\n");

    /* 4. Gyroscope configuration: ±250°/s (FS_SEL = 0) */
    ret = i2c_smbus_write_byte_data(fd, MPU6050_REG_GYRO_CONFIG, 0x00);
    if (ret < 0) {
        fprintf(stderr, "Error: GYRO_CONFIG write failed\n");
        return -1;
    }
    printf("  GYRO_CONFIG = 0x00 (±250°/s)\n");

    /* 5. Accelerometer configuration: ±2g (AFS_SEL = 0) */
    ret = i2c_smbus_write_byte_data(fd, MPU6050_REG_ACCEL_CONFIG, 0x00);
    if (ret < 0) {
        fprintf(stderr, "Error: ACCEL_CONFIG write failed\n");
        return -1;
    }
    printf("  ACCEL_CONFIG = 0x00 (±2g)\n");

    printf("Initialization complete.\n\n");
    return 0;
}

/* ─── Read all 14 bytes of sensor data ─── */
static int mpu6050_read_data(int fd, struct mpu6050_data *data)
{
    __u8 buf[MPU6050_DATA_LEN];
    __s32 len;

    /* Burst read 14 bytes starting from ACCEL_XOUT_H (0x3B) */
    len = i2c_smbus_read_i2c_block_data(fd, MPU6050_REG_ACCEL_XOUT_H,
                                          MPU6050_DATA_LEN, buf);
    if (len != MPU6050_DATA_LEN) {
        if (len < 0)
            fprintf(stderr, "Block read failed: %s\n", strerror(-len));
        else
            fprintf(stderr, "Block read: expected %d bytes, got %d\n",
                    MPU6050_DATA_LEN, len);
        return -1;
    }

    /* Parse big-endian register pairs into 16-bit signed integers */
    data->accel_x = (buf[0]  << 8) | buf[1];
    data->accel_y = (buf[2]  << 8) | buf[3];
    data->accel_z = (buf[4]  << 8) | buf[5];
    data->temp    = (buf[6]  << 8) | buf[7];
    data->gyro_x  = (buf[8]  << 8) | buf[9];
    data->gyro_y  = (buf[10] << 8) | buf[11];
    data->gyro_z  = (buf[12] << 8) | buf[13];

    return 0;
}

/* ─── Print sensor data with unit conversions ─── */

static void mpu6050_print_data(const struct mpu6050_data *data, int sample_num)
{
    double ax = data->accel_x / ACCEL_SCALE;
    double ay = data->accel_y / ACCEL_SCALE;
    double az = data->accel_z / ACCEL_SCALE;
    double temp = data->temp / TEMP_SCALE + TEMP_OFFSET;
    double gx = data->gyro_x / GYRO_SCALE;
    double gy = data->gyro_y / GYRO_SCALE;
    double gz = data->gyro_z / GYRO_SCALE;

    printf("%-6d  %8.3f %8.3f %8.3f  %8.2f  %8.2f %8.2f %8.2f\n",
           sample_num, ax, ay, az, temp, gx, gy, gz);
}

/* ─── Portable millisecond sleep ─── */
static void mpu6050_sleep_ms(unsigned int milliseconds)
{
    struct timespec ts;
    ts.tv_sec  = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}