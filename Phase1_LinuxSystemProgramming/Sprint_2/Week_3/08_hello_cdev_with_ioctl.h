/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 08_hello_cdev_with_ioctl.h - Shared ioctl definitions for hello_cdev
 *
 * This file is included by BOTH:
 *   - The kernel module (hello_cdev.c)
 *   - The userspace test program (test_hello_cdev.c)
 *
 * The ioctl command encoding must be identical on both sides,
 * otherwise commands will be misinterpreted.
 *
 * NOTE: Only include this header for the ioctl definitions.
 * Kernel-specific headers (linux/module.h, linux/cdev.h) should
 * NOT be in this shared file.
 * 
 * Since this header file is shared between userspace and kernel space,
 * this file must only contain compile-time constructs that produce identical results regardless of which compiler processes them.
 * The header must not contain:
 *  - pointers (rule; No pointers in structs in the shared header files)
 *  - addresses
 *  - No kernel-only types (spinlock_t, struct cdev, etc.)
 *  - No #include <linux/module.h> or other kernel-internal headers
 *  - No __user annotations (as this is a kernel-only macro)
 * 
 * 
 * Another rule: Use __u8, __u16, __u32, __u64 for portability (they provide guaranteed sizes across all architectures)
 * 
 * This way, the header can be compiled independently by two different compilers.
 */

#ifndef HELLO_CDEV_H
#define HELLO_CDEV_H

#include <linux/ioctl.h>  /* Available in both kernel and userspace via uapi */
#include <linux/types.h>  /* UAPI — provides __u32, __u64, etc. */

/* ──────────────────────────────────────────────
 * ioctl definitions
 * ────────────────────────────────────────────── */

/*
 * Magic number — must be unique to this driver.
 * Using lowercase 'h' for hello_cdev.
 * Check conflicts: grep -rh "_IO.*'" include/ | sort -u
 */
#define HELLO_IOC_MAGIC    'h'

/*
 * Command numbers — sequential, starting from 0.
 * HELLO_IOC_MAXNR must equal the highest command number.
 */
#define HELLO_IOC_GET_COUNT    0   /* _IOR: read counter without incrementing */
#define HELLO_IOC_SET_COUNT    1   /* _IOW: set counter to a specific value */
#define HELLO_IOC_RESET_COUNT  2   /* _IO:  reset counter to zero */
#define HELLO_IOC_GET_STATS    3   /* _IOR: read statistics struct */

#define HELLO_IOC_MAXNR        3   /* Update when adding new commands */

/*
 * Encoded command definitions.
 *
 * _IOR: kernel → userspace (read from driver)
 * _IOW: userspace → kernel (write to driver)
 * _IO:  no data transfer
 *
 * The third argument to _IOR/_IOW is the C type of the data (ex.: int).
 * The macro encodes sizeof() of this type into the command.
 */
#define HELLO_GET_COUNT    _IOR(HELLO_IOC_MAGIC, HELLO_IOC_GET_COUNT, int)
#define HELLO_SET_COUNT    _IOW(HELLO_IOC_MAGIC, HELLO_IOC_SET_COUNT, int)
#define HELLO_RESET_COUNT  _IO(HELLO_IOC_MAGIC, HELLO_IOC_RESET_COUNT)
#define HELLO_GET_STATS    _IOR(HELLO_IOC_MAGIC, HELLO_IOC_GET_STATS, struct hello_stats)

/* Statistics struct — returned by HELLO_GET_STATS */
struct hello_stats {
    __u32 counter;       /* Current counter value */
    __u32 total_reads;   /* Total number of read() calls */
    __u32 total_writes;  /* Total number of write() calls */
    __u32 total_ioctls;  /* Total number of ioctl() calls */
};

#endif /* HELLO_CDEV_H */