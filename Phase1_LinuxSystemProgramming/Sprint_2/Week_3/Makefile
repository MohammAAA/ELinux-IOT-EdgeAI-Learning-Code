# SPDX-License-Identifier: GPL-2.0
#
# Makefile for 1_Minimal_kernel_module — an out-of-tree kernel module
#
# obj-m  : Build as a loadable module (NOT built into the kernel)
# obj-y  : Build into the kernel (built-in)
# obj-n  : Don't build
#
# The -m suffix means "module" — kbuild recognizes this and
# handles the .ko linking, modpost, and versioning automatically.

obj-m += 01_Minimal_kernel_module.o

# KDIR — Path to the kernel build tree
#
# This is NOT the kernel source tree. It's the build output directory
# that contains:
#   - Makefile (the kernel's top-level build file)
#   - include/ (generated headers, configs)
#   - scripts/ (kbuild tools: modpost, recordmcount, etc.)
#
# Why switching to the kernel's build output directory?
#	Kernel modules cannot be compiled like normal C programs
#   because they do not use standard C libraries (like stdio.h).
#	Instead, they must be compiled against the exact configuration
#   and header files of the running Linux kernel.
#
#   Kernel module compilation uses a method called kbuild (the kernel's built-in build system).
#   Our Makefile essentially hands over the control of compilation to the main kernel source tree.
#
#
# On Debian/Ubuntu/RPi: /lib/modules/$(uname -r)/build
#   This is a symlink to /usr/src/linux-headers-$(uname -r)/
#   It points directly to the kernel headers and configuration files
#   needed to build modules for our active kernel version 
#
# If this directory doesn't exist, we need:
#   sudo apt install linux-headers-$(uname -r)    # on x86
#   sudo apt install raspberrypi-kernel-headers    # on RPi
KDIR := /lib/modules/$(shell uname -r)/build

# PWD — Current working directory
#
# This tells kbuild where our module source lives.
# kbuild will look here for our .c files and this Makefile.
PWD := $(shell pwd)

# Targets:

# Default target: build the module
all:
	make -C $(KDIR) M=$(PWD) modules
#       Explanation:
# 	    -C $(KDIR): This changes the directory (-C) to the kernel build path (e.g., /lib/modules/6.1.0-rpi5-rpi-v8/build).
#           It tells make to use the main Linux kernel's massive Makefile instead of our own.
#	    M=$(PWD): This tells the kernel's Makefile where our module source code is located.
#           The kernel will jump into our directory to find our 01_Minimal_kernel_module.c file.
#	    modules: This is the target rule inside the kernel's Makefile.
#            It instructs the kernel build system to compile the files listed in obj-m into a loadable kernel module file ending in .ko.


# Clean target: remove all generated build artifacts (like .o, .mod, .order, and .symvers files)
clean:
	make -C $(KDIR) M=$(PWD) clean

# Convenience targets for development:
load:
	sudo insmod 01_Minimal_kernel_module.ko

unload:
	sudo rmmod 01_Minimal_kernel_module

log:
	dmesg | tail -20

# PHONY — Tell make these aren't real files
.PHONY: all clean load unload log