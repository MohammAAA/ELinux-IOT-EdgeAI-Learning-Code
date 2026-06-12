CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -std=c11 -O2
LDFLAGS = -li2c -lm
TARGET  = mpu6050_diag
SRC     = 02_mpu6050_diagnostics.c

all: $(TARGET)

 $(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

# Cross-compile for RPi5 (ARM64)
arm64: CC = aarch64-linux-gnu-gcc
arm64: $(TARGET)

.PHONY: all clean arm64