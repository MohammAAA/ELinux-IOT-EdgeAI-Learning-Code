# obj-m means "build as a loadable module"
obj-m += hello_module.o

# Path to your kernel build directory
# On RPi5: /lib/modules/$(shell uname -r)/build
#Why is the path in /lib/modules/.../build?
#	Kernel modules cannot be compiled like normal C programs because they do not use standard C libraries (like stdio.h).
#	Instead, they must be compiled against the exact configuration and header files of the running Linux kernel.
#	The /lib/modules/$(shell uname -r)/build path is a symbolic link created by your Linux distribution.
#	It points directly to the kernel headers and configuration files needed to build modules for your active kernel version 
KDIR := /lib/modules/$(shell uname -r)/build


# Kernel module compilation uses a method called kbuild (the kernel's built-in build system).
# Our Makefile essentially hands over the control of compilation to the main kernel source tree.


# Default target
all:
    make -C $(KDIR) M=$(PWD) modules
# 	-C $(KDIR): This changes the directory (-C) to the kernel build path (e.g., /lib/modules/6.1.0-rpi5-rpi-v8/build). It tells make to use the main Linux kernel's massive Makefile instead of our own.
#	M=$(PWD): This tells the kernel's Makefile where our module source code is located. The kernel will jump into our directory to find our hello_module.c file.
#	modules: This is the target rule inside the kernel's Makefile. It instructs the kernel build system to compile the files listed in obj-m into a loadable kernel module file ending in .ko (Kernel Object).

# Clean target
clean:
    make -C $(KDIR) M=$(PWD) clean