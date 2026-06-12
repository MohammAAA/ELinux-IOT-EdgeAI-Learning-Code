/*
 * 02_mpu6050_diagnostics.c — Diagnostic test for MPU-6050
 *
 * Adds compared to 01_mpu6050_test.c: hardware reset, config readback, raw byte printing,
 *       block read vs individual read comparison
 *
 * Build: gcc -o mpu6050_diag 02_mpu6050_diagnostics.c -li2c
 */
#define _POSIX_C_SOURCE 199309L  // Enables nanosleep functionality
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>
#include <math.h>

#define MPU6050_ADDR            0x68
#define MPU6050_REG_SMPLRT_DIV  0x19
#define MPU6050_REG_CONFIG      0x1A
#define MPU6050_REG_GYRO_CONFIG 0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_INT_ENABLE  0x38
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1  0x6B
#define MPU6050_REG_PWR_MGMT_2  0x6C
#define MPU6050_REG_WHO_AM_I    0x75
#define MPU6050_DATA_LEN        14

#define ACCEL_SCALE  16384.0
#define GYRO_SCALE   131.0
#define TEMP_SCALE   340.0
#define TEMP_OFFSET  36.53

static int fd;  /* Global for simplicity in diagnostic program */

static void sleep_ms(unsigned int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

static int read_reg(__u8 reg)
{
    __s32 val = i2c_smbus_read_byte_data(fd, reg);
    if (val < 0)
        fprintf(stderr, "  ERROR reading reg 0x%02x: %s\n", reg, strerror(-val));
    return val;
}

static int write_reg(__u8 reg, __u8 val)
{
    int ret = i2c_smbus_write_byte_data(fd, reg, val);
    if (ret < 0)
        fprintf(stderr, "  ERROR writing 0x%02x to reg 0x%02x: %s\n",
                val, reg, strerror(-ret));
    return ret;
}

/* ─── Diagnostic: Read back all config registers ─── */
static void print_config_registers(void)
{
    printf("\n─── Configuration Register Readback ───\n");
    printf("  PWR_MGMT_1  (0x6B) = 0x%02x  ", read_reg(0x6B));
    printf("(expect 0x00 = awake, clk=internal)\n");

    printf("  PWR_MGMT_2  (0x6C) = 0x%02x  ", read_reg(0x6C));
    printf("(expect 0x00 = no standby)\n");

    printf("  SMPLRT_DIV  (0x19) = 0x%02x  ", read_reg(0x19));
    printf("(expect 0x07 = 125Hz)\n");

    printf("  CONFIG      (0x1A) = 0x%02x  ", read_reg(0x1A));
    printf("(expect 0x06 = DLPF 5Hz)\n");

    printf("  GYRO_CONFIG (0x1B) = 0x%02x  ", read_reg(0x1B));
    {
        int val = read_reg(0x1B);
        int fs_sel = (val >> 3) & 0x03;
        const char *ranges[] = {"±250°/s", "±500°/s", "±1000°/s", "±2000°/s"};
        printf("(FS_SEL=%d = %s)\n", fs_sel, ranges[fs_sel]);
    }

    printf("  ACCEL_CONFIG(0x1C) = 0x%02x  ", read_reg(0x1C));
    {
        int val = read_reg(0x1C);
        int afs_sel = (val >> 3) & 0x03;
        const char *ranges[] = {"±2g", "±4g", "±8g", "±16g"};
        int scales[] = {16384, 8192, 4096, 2048};
        printf("(AFS_SEL=%d = %s, %d LSB/g)\n", afs_sel, ranges[afs_sel], scales[afs_sel]);
    }

    printf("  WHO_AM_I    (0x75) = 0x%02x\n", read_reg(0x75));
}

/* ─── Diagnostic: Print raw bytes from block read ─── */
static void print_raw_block_data(void)
{
    __u8 buf[MPU6050_DATA_LEN];
    __s32 len;

    printf("\n─── Block Read: Raw Bytes ───\n");

    len = i2c_smbus_read_i2c_block_data(fd, MPU6050_REG_ACCEL_XOUT_H,
                                          MPU6050_DATA_LEN, buf);
    printf("  Returned length: %d (expected %d)\n", len, MPU6050_DATA_LEN);

    if (len <= 0) {
        printf("  BLOCK READ FAILED!\n");
        return;
    }

    printf("  Raw bytes: ");
    for (int i = 0; i < len; i++)
        printf("0x%02x ", buf[i]);
    printf("\n");

    /* Parse and display each 16-bit value from block read */
    printf("  Block read parsed values:\n");
    int16_t ax = (buf[0] << 8) | buf[1];
    int16_t ay = (buf[2] << 8) | buf[3];
    int16_t az = (buf[4] << 8) | buf[5];
    int16_t t  = (buf[6] << 8) | buf[7];
    int16_t gx = (buf[8] << 8) | buf[9];
    int16_t gy = (buf[10] << 8) | buf[11];
    int16_t gz = (buf[12] << 8) | buf[13];

    printf("    accel: X=%6d  Y=%6d  Z=%6d\n", ax, ay, az);
    printf("    temp:  %6d  (%.2f °C)\n", t, t / TEMP_SCALE + TEMP_OFFSET);
    printf("    gyro:  X=%6d  Y=%6d  Z=%6d\n", gx, gy, gz);

    /* Compute magnitude */
    double mag = sqrt((ax/(double)ACCEL_SCALE)*(ax/(double)ACCEL_SCALE) +
                      (ay/(double)ACCEL_SCALE)*(ay/(double)ACCEL_SCALE) +
                      (az/(double)ACCEL_SCALE)*(az/(double)ACCEL_SCALE));
    printf("    accel magnitude: %.3f g (expect ~1.000)\n", mag);
}

/* ─── Diagnostic: Compare block read vs individual register reads ─── */
static void compare_block_vs_individual(void)
{
    printf("\n─── Block Read vs Individual Read Comparison ───\n");

    /* Read individual registers for ACCEL_XOUT */
    int ax_h = read_reg(0x3B);  /* ACCEL_XOUT_H */
    int ax_l = read_reg(0x3C);  /* ACCEL_XOUT_L */
    int ay_h = read_reg(0x3D);  /* ACCEL_YOUT_H */
    int ay_l = read_reg(0x3E);  /* ACCEL_YOUT_L */
    int az_h = read_reg(0x3F);  /* ACCEL_ZOUT_H */
    int az_l = read_reg(0x40);  /* ACCEL_ZOUT_L */

    if (ax_h < 0 || ax_l < 0 || ay_h < 0 || ay_l < 0 ||
        az_h < 0 || az_l < 0) {
        printf("  Individual reads FAILED — skipping comparison\n");
        return;
    }

    int16_t ax_ind = (ax_h << 8) | ax_l;
    int16_t ay_ind = (ay_h << 8) | ay_l;
    int16_t az_ind = (az_h << 8) | az_l;

    printf("  Individual reads:  AX=%6d  AY=%6d  AZ=%6d\n",
           ax_ind, ay_ind, az_ind);
    printf("  Individual accel:  AX=%.3fg  AY=%.3fg  AZ=%.3fg\n",
           ax_ind / ACCEL_SCALE, ay_ind / ACCEL_SCALE, az_ind / ACCEL_SCALE);

    double mag_ind = sqrt(
        (ax_ind/(double)ACCEL_SCALE)*(ax_ind/(double)ACCEL_SCALE) +
        (ay_ind/(double)ACCEL_SCALE)*(ay_ind/(double)ACCEL_SCALE) +
        (az_ind/(double)ACCEL_SCALE)*(az_ind/(double)ACCEL_SCALE));
    printf("  Individual magnitude: %.3f g\n", mag_ind);

    /* Now read via block read */
    __u8 buf[MPU6050_DATA_LEN];
    __s32 len = i2c_smbus_read_i2c_block_data(fd, MPU6050_REG_ACCEL_XOUT_H,
                                                MPU6050_DATA_LEN, buf);
    if (len != MPU6050_DATA_LEN) {
        printf("  Block read returned %d bytes (expected %d)\n", len, MPU6050_DATA_LEN);
        return;
    }

    int16_t ax_blk = (buf[0] << 8) | buf[1];
    int16_t ay_blk = (buf[2] << 8) | buf[3];
    int16_t az_blk = (buf[4] << 8) | buf[5];

    printf("  Block read:        AX=%6d  AY=%6d  AZ=%6d\n",
           ax_blk, ay_blk, az_blk);

    /* Compare */
    printf("  ─── Comparison ───\n");
    printf("  AX: individual=%6d  block=%6d  match=%s\n",
           ax_ind, ax_blk, ax_ind == ax_blk ? "YES" : "*** MISMATCH ***");
    printf("  AY: individual=%6d  block=%6d  match=%s\n",
           ay_ind, ay_blk, ay_ind == ay_blk ? "YES" : "*** MISMATCH ***");
    printf("  AZ: individual=%6d  block=%6d  match=%s\n",
           az_ind, az_blk, az_ind == az_blk ? "YES" : "*** MISMATCH ***");

    if (ax_ind == ax_blk && ay_ind == ay_blk && az_ind == az_blk)
        printf("  → Block read and individual reads AGREE\n");
    else
        printf("  → Block read and individual reads DISAGREE — block read is broken!\n");

    /* Key test: is the individual read magnitude correct? */
    if (fabs(mag_ind - 1.0) < 0.1)
        printf("  → Individual read magnitude is correct (~1.0g). Block read is the problem.\n");
    else
        printf("  → Individual read magnitude is ALSO wrong (%.3fg). Hardware/config issue.\n", mag_ind);
}

/* ─── Alternative: Read using raw I2C_RDWR ioctl ─── */
static void read_with_i2c_rdwr(void)
{
    printf("\n─── Raw I2C_RDWR Read (bypasses SMBus layer) ───\n");

    __u8 buf[MPU6050_DATA_LEN];
    __u8 reg = MPU6050_REG_ACCEL_XOUT_H;

    struct i2c_msg msgs[2] = {
        { .addr = MPU6050_ADDR, .flags = 0,        .len = 1,   .buf = &reg },  /* Write: register address */
        { .addr = MPU6050_ADDR, .flags = I2C_M_RD, .len = MPU6050_DATA_LEN, .buf = buf },  /* Read: 14 bytes */
    };

    struct i2c_rdwr_ioctl_data data = {
        .msgs = msgs,
        .nmsgs = 2,
    };

    int ret = ioctl(fd, I2C_RDWR, &data);
    if (ret < 0) {
        printf("  I2C_RDWR failed: %s\n", strerror(errno));
        return;
    }

    printf("  Raw bytes: ");
    for (int i = 0; i < MPU6050_DATA_LEN; i++)
        printf("0x%02x ", buf[i]);
    printf("\n");

    int16_t ax = (buf[0] << 8) | buf[1];
    int16_t ay = (buf[2] << 8) | buf[3];
    int16_t az = (buf[4] << 8) | buf[5];
    int16_t t  = (buf[6] << 8) | buf[7];
    int16_t gx = (buf[8] << 8) | buf[9];
    int16_t gy = (buf[10] << 8) | buf[11];
    int16_t gz = (buf[12] << 8) | buf[13];

    printf("  I2C_RDWR parsed:  AX=%6d  AY=%6d  AZ=%6d\n", ax, ay, az);
    printf("  I2C_RDWR scaled:  AX=%.3fg  AY=%.3fg  AZ=%.3fg\n",
           ax / ACCEL_SCALE, ay / ACCEL_SCALE, az / ACCEL_SCALE);
    printf("  I2C_RDWR temp:    %.2f °C\n", t / TEMP_SCALE + TEMP_OFFSET);
    printf("  I2C_RDWR gyro:    GX=%.2f  GY=%.2f  GZ=%.2f °/s\n",
           gx / GYRO_SCALE, gy / GYRO_SCALE, gz / GYRO_SCALE);

    double mag = sqrt(
        (ax/(double)ACCEL_SCALE)*(ax/(double)ACCEL_SCALE) +
        (ay/(double)ACCEL_SCALE)*(ay/(double)ACCEL_SCALE) +
        (az/(double)ACCEL_SCALE)*(az/(double)ACCEL_SCALE));
    printf("  I2C_RDWR magnitude: %.3f g (expect ~1.000)\n", mag);
}

int main(void)
{
    printf("MPU-6050 Diagnostic Test\n");
    printf("========================\n\n");

    /* Open I2C bus */
    fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) {
        perror("Cannot open /dev/i2c-1");
        return 1;
    }

    if (ioctl(fd, I2C_SLAVE, MPU6050_ADDR) < 0) {
        perror("Cannot set slave address");
        close(fd);
        return 1;
    }

    /* ─── Step 0: Check I2C adapter capabilities ─── */
    unsigned long funcs;
    if (ioctl(fd, I2C_FUNCS, &funcs) == 0) {
        printf("I2C Adapter Capabilities:\n");
        printf("  I2C (raw):           %s\n", (funcs & I2C_FUNC_I2C)           ? "YES" : "NO");
        printf("  SMBus read byte:     %s\n", (funcs & I2C_FUNC_SMBUS_READ_BYTE) ? "YES" : "NO");
        printf("  SMBus write byte:    %s\n", (funcs & I2C_FUNC_SMBUS_WRITE_BYTE)? "YES" : "NO");
        printf("  SMBus read block:    %s\n", (funcs & I2C_FUNC_SMBUS_READ_BLOCK_DATA) ? "YES" : "NO");
        printf("  SMBus read I2C block:%s\n", (funcs & I2C_FUNC_SMBUS_READ_I2C_BLOCK) ? "YES" : "NO");
        printf("  SMBus word:          %s\n", (funcs & I2C_FUNC_SMBUS_READ_WORD_DATA) ? "YES" : "NO");
        printf("\n");
    }

    /* ─── Step 1: WHO_AM_I before anything ─── */
    printf("─── WHO_AM_I (before reset) ───\n");
    printf("  WHO_AM_I = 0x%02x\n\n", read_reg(MPU6050_REG_WHO_AM_I));

    /* ─── Step 2: FULL HARDWARE RESET ─── */
    printf("─── Hardware Reset ───\n");
    printf("  Writing 0x80 to PWR_MGMT_1 (RESET bit)...\n");
    if (write_reg(MPU6050_REG_PWR_MGMT_1, 0x80) < 0) {
        printf("  Reset write failed! Trying to continue...\n");
    }
    sleep_ms(150);  /* Wait for reset to complete (datasheet: >100ms) */

    /* Verify reset took effect: PWR_MGMT_1 should read 0x40 (sleep mode, default after reset) */
    int pwr = read_reg(MPU6050_REG_PWR_MGMT_1);
    printf("  PWR_MGMT_1 after reset: 0x%02x (expect 0x40 = sleep mode)\n", pwr);

    if (pwr != 0x40) {
        printf("  WARNING: PWR_MGMT_1 is not 0x40 after reset. Sensor may not have reset.\n");
    }

    /* ─── Step 3: Wake up the sensor ─── */
    printf("\n─── Wake Sensor ───\n");
    write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    sleep_ms(100);  /* Wait for oscillator to stabilize */

    /* ─── Step 4: Configure ─── */
    printf("─── Configure ───\n");
    write_reg(MPU6050_REG_SMPLRT_DIV, 0x07);
    write_reg(MPU6050_REG_CONFIG, 0x06);
    write_reg(MPU6050_REG_GYRO_CONFIG, 0x00);
    write_reg(MPU6050_REG_ACCEL_CONFIG, 0x00);
    sleep_ms(50);   /* Let settings settle */

    /* ─── Step 5: Read back ALL config registers ─── */
    print_config_registers();

    /* ─── Step 6: Raw block read with byte dump ─── */
    print_raw_block_data();

    /* ─── Step 7: Compare block read vs individual reads ─── */
    compare_block_vs_individual();

    /* ─── Step 8: Try raw I2C_RDWR as alternative ─── */
    read_with_i2c_rdwr();

    /* ─── Step 9: Read 5 samples using whichever method works best ─── */
    printf("\n─── 5 Samples Using Individual Reads (most reliable) ───\n");
    printf("%-4s  %8s %8s %8s  %8s  %8s %8s %8s  %6s\n",
           "#", "Ax(g)", "Ay(g)", "Az(g)",
           "Temp(C)", "Gx(d/s)", "Gy(d/s)", "Gz(d/s)", "Mag(g)");

    for (int i = 0; i < 5; i++) {
        int ax_h = read_reg(0x3B), ax_l = read_reg(0x3C);
        int ay_h = read_reg(0x3D), ay_l = read_reg(0x3E);
        int az_h = read_reg(0x3F), az_l = read_reg(0x40);
        int th  = read_reg(0x41), tl  = read_reg(0x42);
        int gxh = read_reg(0x43), gxl = read_reg(0x44);
        int gyh = read_reg(0x45), gyl = read_reg(0x46);
        int gzh = read_reg(0x47), gzl = read_reg(0x48);

        int16_t ax = (ax_h << 8) | ax_l;
        int16_t ay = (ay_h << 8) | ay_l;
        int16_t az = (az_h << 8) | az_l;
        int16_t t  = (th  << 8) | tl;
        int16_t gx = (gxh << 8) | gxl;
        int16_t gy = (gyh << 8) | gyl;
        int16_t gz = (gzh << 8) | gzl;

        double axg = ax / ACCEL_SCALE;
        double ayg = ay / ACCEL_SCALE;
        double azg = az / ACCEL_SCALE;
        double temp = t / TEMP_SCALE + TEMP_OFFSET;
        double gxd = gx / GYRO_SCALE;
        double gyd = gy / GYRO_SCALE;
        double gzd = gz / GYRO_SCALE;
        double mag = sqrt(axg*axg + ayg*ayg + azg*azg);

        printf("%-4d  %8.3f %8.3f %8.3f  %8.2f  %8.2f %8.2f %8.2f  %6.3f\n",
               i+1, axg, ayg, azg, temp, gxd, gyd, gzd, mag);

        sleep_ms(100);
    }

    /* Put sensor to sleep */
    write_reg(MPU6050_REG_PWR_MGMT_1, 0x40);
    printf("\nSensor put to sleep.\n");

    close(fd);
    return 0;
}