// SPDX-License-Identifier: GPL-2.0
/*
 * 08_hello_cdev_with_ioctl.c - Character device driver returning a monotonic counter; This is my first character device driver.
 *
 * Behaviour:
 *   read()  → returns the current counter value (4 bytes, int)
 *             and increments the counter atomically
 *   write() → resets the counter to the value provided (4 bytes, int)
 *             must be called with exactly sizeof(int) bytes
 *
 * This driver demonstrates:
 *   - alloc_chrdev_region / cdev_init / cdev_add registration
 *   - file_operations: open, release, read, write
 *   - container_of pattern for per-device state
 *   - copy_to_user / copy_from_user for userspace boundary
 *   - atomic_t for lock-free counter increments
 *   - mutex for write-side mutual exclusion
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include "08_hello_cdev_with_ioctl.h"

/* ──────────────────────────────────────────────
 * Module parameters
 * ────────────────────────────────────────────── */
static int major_num = 0;   /* 0 = dynamic allocation */
static int minor_num = 0;
module_param(major_num, int, 0444);
MODULE_PARM_DESC(major_num, "Major device number (0=auto-allocate, default: 0)");
module_param(minor_num, int, 0444);
MODULE_PARM_DESC(minor_num, "Minor device number (default: 0)");

/* ──────────────────────────────────────────────
 * Device name — used in /proc/devices and for
 * the /dev node name
 * ────────────────────────────────────────────── */
#define DEVICE_NAME "hello_cdev"

/* ──────────────────────────────────────────────
 * Forward declarations
 * ────────────────────────────────────────────── */
static int hello_open(struct inode *inode, struct file *filp);
static int hello_release(struct inode *inode, struct file *filp);
static ssize_t hello_read(struct file *filp, char __user *buf,
                          size_t count, loff_t *f_pos);
static ssize_t hello_write(struct file *filp, const char __user *buf,
                           size_t count, loff_t *f_pos);
static long hello_ioctl(struct file *filp, unsigned int cmd,
                        unsigned long arg);                           

/* ──────────────────────────────────────────────
 * Per-device state structure
 * ────────────────────────────────────────────── */
struct hello_device {
    struct cdev cdev;         /* Character device — MUST be embedded (i.e.: not a pointer) */
    dev_t dev_num;            /* Device number (major:minor) */
    struct mutex lock;        /* Protects write-side operations */
    atomic_t counter;         /* Monotonic counter returned on read() */
    atomic_t read_count;      /* Total number of read() calls */
    atomic_t write_count;     /* Total number of write() calls */
    atomic_t ioctl_count;     /* Total number of ioctl() calls */
};

/* Single global device instance */
static struct hello_device *hello_dev;

/* ──────────────────────────────────────────────
 * file_operations — the VFS callback table
 *
 * Only fill the operations we support. Any NULL
 * entry means "not supported" — the VFS will
 * return -ENOSYS or -EINVAL to userspace.
 *
 * IMPORTANT: .owner = THIS_MODULE is mandatory.
 * Without it, the kernel won't track the module
 * reference count, and rmmod while the device
 * is open will cause a kernel panic.
 * ────────────────────────────────────────────── */
static const struct file_operations hello_fops = {
    .owner   = THIS_MODULE,
    .open    = hello_open,
    .release = hello_release,
    .read    = hello_read,
    .write   = hello_write,
    .unlocked_ioctl = hello_ioctl,
};

/*
 * hello_open - Called when a userspace process opens /dev/hello_cdev
 *
 * @inode: VFS inode for the device node (contains i_cdev → our cdev, and i_rdev → the major:minor device number)
 * @filp:  The open file object (per-open instance)
 *
 * What we do:
 *   1. Use container_of() to get from inode->i_cdev to our hello_device struct
 *   2. Store that pointer in filp->private_data
 *   3. Log who opened us (for debugging)
 *
 * Returns: 0 on success
 */
static int hello_open(struct inode *inode, struct file *filp)
{
    struct hello_device *dev;

    /*
     * container_of — THE kernel driver pattern.
     *
     * inode->i_cdev points to the cdev INSIDE our hello_device struct.
     * container_of() subtracts the cdev offset to get back to hello_device base address.
     *
     * Why not just use the global hello_dev?
     * Because if we ever support multiple devices (multiple minors),
     * each would have its own hello_device, and only container_of()
     * can find the right one from the inode.
     */
    dev = container_of(inode->i_cdev, struct hello_device, cdev);

    /*
     * filp->private_data — per-open storage.
     *
     * This pointer is passed to every subsequent callback
     * (read, write, ioctl, release) as filp->private_data.
     *
     * We store our device struct here so read/write don't
     * need to do container_of() again.
     */
    filp->private_data = dev;

    /*
     * Non-blocking open check.
     *
     * If O_NONBLOCK is set and we need to do something that
     * could block (e.g., wait for hardware), return -EAGAIN.
     * For hello_cdev, nothing blocks, so we accept O_NONBLOCK.
     */
    if (filp->f_flags & O_NONBLOCK) {
        pr_info("hello_cdev: opened in non-blocking mode by pid %d (comm: %s)\n",
                current->pid, current->comm);
    } else {
        pr_info("hello_cdev: opened by pid %d (comm: %s)\n",
                current->pid, current->comm);
    }

    return 0;
}

/*
 * hello_release - Called when the LAST file descriptor is closed
 *
 * @inode: VFS inode
 * @filp:  The file object being released
 *
 * IMPORTANT: release is NOT called once per close().
 * If a process fork()s, the child inherits the fd.
 * release() only fires when ALL references to this filp are gone.
 *
 * For our simple driver, nothing to clean up per-open.
 * We just log and return.
 */
static int hello_release(struct inode *inode, struct file *filp)
{
    pr_info("hello_cdev: released by pid %d (comm: %s)\n",
            current->pid, current->comm);

    /*
     * We do NOT set filp->private_data = NULL here.
     * The kernel will free the struct file after this returns,
     * so the pointer is about to become invalid anyway.
     */
    return 0;
}

/*
 * hello_read - Return the current counter value to userspace
 *
 * @filp:  Open file (filp->private_data = our device struct)
 * @buf:   USERSPACE buffer — do NOT dereference directly!
 * @count: How many bytes userspace wants
 * @f_pos: File position offset
 *
 * Behaviour:
 *   - Always returns exactly sizeof(int) bytes (4 bytes)
 *   - Atomically increments the counter AFTER reading
 *   - If userspace requests fewer bytes, return -EINVAL
 *   - If userspace requests more bytes, return sizeof(int)
 *
 * Returns: Number of bytes read, or negative error code
 */
static ssize_t hello_read(struct file *filp, char __user *buf,
                          size_t count, loff_t *f_pos)
{
    struct hello_device *dev = filp->private_data;
    int value;
    unsigned int bytes_not_copied;

    /*
     * Validate buffer size.
     *
     * Our device always returns exactly sizeof(int) bytes.
     * Userspace must provide at least that much space.
     */
    if (count < sizeof(int)) {
        pr_warn("hello_cdev: read buffer too small: %zu bytes (need %zu)\n",
                count, sizeof(int));
        return -EINVAL;  /* Invalid argument */
    }

    /*
     * Read the counter atomically.
     *
     * atomic_inc_return() does:
     *   1. Read the current value
     *   2. Increment it
     *   3. Return the NEW value
     *
     * This is a single atomic operation — no race condition possible.
     * Even if two threads call read() simultaneously, each gets
     * a unique incremented value.
     */
    value = atomic_inc_return(&dev->counter);

    /*
     * Track total read count.
     *
     * This is a separate counter for monitoring/diagnostics.
     * It can be read via sysfs (if we add a parameter for it).
     */
    atomic_inc(&dev->read_count);

    /*
     * copy_to_user — THE safe way to write to userspace.
     *
     * void copy_to_user(to, from, n):
     *   to:   userspace destination pointer (buf)
     *   from: kernel source pointer (&value)
     *   n:    number of bytes to copy
     *
     * Why not just *buf = value?
     *   1. buf is a USERSPACE address — dereferencing it directly
     *      in kernel mode works on most architectures but is WRONG.
     *      The address might not be mapped (page not present),
     *      or might be read-only, or might be in high memory
     *      that requires special access methods.
     *   2. copy_to_user handles all these cases safely.
     *   3. It returns the number of bytes NOT copied (0 = full success).
     *
     * ARCHITECTURE NOTE:
     *   On ARM64 (RPi5), the kernel runs with its own page tables.
     *   Userspace addresses (0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF)
     *   are NOT mapped in the kernel page tables.
     *   Accessing them directly would cause a page fault → kernel panic.
     *   copy_to_user temporarily maps the userspace page and copies safely.
     */
    bytes_not_copied = copy_to_user(buf, &value, sizeof(value));
    if (bytes_not_copied != 0) {
        pr_warn("hello_cdev: copy_to_user failed: %u bytes not copied\n",
                bytes_not_copied);
        return -EFAULT;  /* Bad address — userspace buffer is invalid */
    }

    /*
     * Return the number of bytes actually transferred.
     *
     * The VFS expects read() to return the byte count.
     * If we return 0, userspace thinks EOF.
     * If we return negative, it's an error code.
     *
     * Note: we return sizeof(int), NOT count.
     * Our device always produces exactly sizeof(int) bytes.
     * If userspace asked for 1024 bytes, they get 4.
     */
    pr_debug("hello_cdev: read → %d (total reads: %d, pid: %d)\n",
             value, atomic_read(&dev->read_count), current->pid);

    return sizeof(int);
}

/*
 * hello_write - Reset the counter to a value provided by userspace
 *
 * @filp:  Open file (filp->private_data = our device struct)
 * @buf:   USERSPACE buffer containing the new counter value
 * @count: How many bytes userspace is writing
 * @f_pos: File position offset
 *
 * Behaviour:
 *   - Accepts exactly sizeof(int) bytes
 *   - Sets the counter to the provided value
 *   - Rejects writes that are too small
 *   - Silently ignores extra bytes (writes sizeof(int) regardless)
 *
 * Returns: Number of bytes written, or negative error code
 */
static ssize_t hello_write(struct file *filp, const char __user *buf,
                           size_t count, loff_t *f_pos)
{
    struct hello_device *dev = filp->private_data;
    int value;
    unsigned int bytes_not_copied;

    /*
     * Validate write size.
     *
     * We need at least sizeof(int) bytes.
     * If userspace sends less, the data is incomplete.
     */
    if (count < sizeof(int)) {
        pr_warn("hello_cdev: write too small: %zu bytes (need %zu)\n",
                count, sizeof(int));
        return -EINVAL;
    }

    /*
     * copy_from_user — THE safe way to read from userspace.
     *
     * Works exactly like copy_to_user but in reverse:
     *   to:   kernel destination pointer (&value)
     *   from: userspace source pointer (buf)
     *   n:    number of bytes to copy
     *
     * Returns number of bytes NOT copied (0 = success).
     *
     * SECURITY NOTE:
     *   NEVER trust userspace input. Even if this is a "trusted"
     *   program, a malicious process could open /dev/hello_cdev
     *   and write anything. Always validate.
     */
    bytes_not_copied = copy_from_user(&value, buf, sizeof(value));
    if (bytes_not_copied != 0) {
        pr_warn("hello_cdev: copy_from_user failed: %u bytes not copied\n",
                bytes_not_copied);
        return -EFAULT;
    }

    /*
     * Validate the value (optional but good practice).
     *
     * We accept any int value, including negative.
     * If we wanted to restrict to positive values:
     *   if (value < 0) return -EINVAL;
     */

    /*
     * For a monotonic counter, resetting to 0 is the normal case.
     * But allowing arbitrary values makes testing easier.
     */
    pr_info("hello_cdev: write → setting counter from %d to %d (pid: %d)\n",
            atomic_read(&dev->counter), value, current->pid);

    /*
     * Set the new counter value.
     *
     * We use a mutex here even though atomic_set exists,
     * because in a real driver the write operation might
     * need to update multiple fields atomically.
     *
     * Example for a real sensor driver:
     *   mutex_lock(&dev->lock);
     *   dev->sample_rate = value.rate;
     *   dev->range = value.range;
     *   hw_reconfigure(dev);  // Must happen after both updates
     *   mutex_unlock(&dev->lock);
     *
     * For our simple counter, the mutex is overkill but it
     * demonstrates the correct pattern for production drivers.
     */
    mutex_lock(&dev->lock);
    atomic_set(&dev->counter, value);
    mutex_unlock(&dev->lock);

    atomic_inc(&dev->write_count);

    /*
     * Return sizeof(int) — the number of bytes we consumed.
     *
     * If userspace wrote more than sizeof(int), we silently
     * consume only sizeof(int) and return that count.
     * The VFS handles the difference.
     */
    return sizeof(int);
}

/*
 * ──────────────────────────────────────────────
 * Module init — Three-step device registration
 * ──────────────────────────────────────────────
 */
static int __init hello_cdev_init(void)
{
    int ret;
    dev_t dev;

    /*
     * Allocate the device struct.
     *
     * kzalloc = kernel malloc + zero-initialize.
     * GFP_KERNEL = normal allocation, can sleep.
     * Never use GFP_KERNEL in interrupt context.
     *
     * Why kzalloc and not a static/global struct?
     *   Dynamic allocation is the standard pattern for drivers.
     *   It allows multiple device instances and clean resource
     *   tracking in error paths.
     */
    hello_dev = kzalloc(sizeof(*hello_dev), GFP_KERNEL);
    if (!hello_dev) {
        pr_err("hello_cdev: failed to allocate device struct\n");
        return -ENOMEM;
    }

    /*
     * Initialize the mutex BEFORE any code path can use it.
     *
     * Even though we haven't registered the device yet,
     * defensive initialization prevents bugs if the init
     * order changes later.
     */
    mutex_init(&hello_dev->lock);

    /* Initialize counters to 0 (kzalloc already did this, but be explicit) */
    atomic_set(&hello_dev->counter, 0);
    atomic_set(&hello_dev->read_count, 0);

    /*
     * STEP 1: Allocate a device number.
     *
     * alloc_chrdev_region:
     *   &dev      → output: the allocated major:minor
     *   minor_num → starting minor (our module param, default 0)
     *   1         → count of minors (one device)
     *   DEVICE_NAME → appears in /proc/devices
     */
    ret = alloc_chrdev_region(&dev, minor_num, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("hello_cdev: alloc_chrdev_region failed: %d\n", ret);
        goto fail_alloc;
    }
    hello_dev->dev_num = dev;

    pr_info("hello_cdev: allocated major=%d, minor=%d\n",
            MAJOR(dev), MINOR(dev));

    /*
     * STEP 2: Initialize cdev and bind to file_operations.
     *
     * cdev_init zeroes the cdev struct and sets cdev->ops = &hello_fops.
     * We MUST also set cdev->owner = THIS_MODULE for reference counting.
     */
    cdev_init(&hello_dev->cdev, &hello_fops);
    hello_dev->cdev.owner = THIS_MODULE;

    /*
     * STEP 3: Add cdev to the kernel.
     *
     * After this call, the device is LIVE. Any open() call with
     * the correct major/minor will be routed to our file_operations.
     */
    ret = cdev_add(&hello_dev->cdev, hello_dev->dev_num, 1);
    if (ret < 0) {
        pr_err("hello_cdev: cdev_add failed: %d\n", ret);
        goto fail_cdev_add;
    }

    pr_info("hello_cdev: device registered — major=%d minor=%d\n",
            MAJOR(hello_dev->dev_num), MINOR(hello_dev->dev_num));
    pr_info("hello_cdev: create device node with: "
            "sudo mknod /dev/%s c %d %d\n",
            DEVICE_NAME,
            MAJOR(hello_dev->dev_num),
            MINOR(hello_dev->dev_num));

    return 0;

    /*
     * ERROR CLEANUP — goto chain pattern.
     *
     * This is the standard kernel error cleanup pattern.
     * Each goto label undoes ONE step of initialization.
     * The labels are in REVERSE order of the init steps.
     *
     * Why goto and not nested if/else?
     *   1. Flat structure — easy to verify all paths are cleaned up
     *   2. Every error jumps to exactly the right cleanup point
     *   3. This pattern is used in EVERY kernel driver
     *   4. The kernel coding style mandates it
     *
     * Rule: if you allocate in step N, add a "goto fail_step_N"
     *       at the error check of step N+1.
     */
fail_cdev_add:
    unregister_chrdev_region(hello_dev->dev_num, 1);
fail_alloc:
    kfree(hello_dev);
    hello_dev = NULL;
    return ret;
}

/*
 * ──────────────────────────────────────────────
 * Module exit — Reverse-order cleanup
 * ──────────────────────────────────────────────
 */
static void __exit hello_cdev_exit(void)
{
    int final_reads, final_counter;

    /* Snapshot stats BEFORE freeing anything */
    final_reads = atomic_read(&hello_dev->read_count);
    final_counter = atomic_read(&hello_dev->counter);
    /*
     * Cleanup in REVERSE order of initialization.
     *
     * If you unregister the device number first but the cdev
     * is still live, a concurrent open() could route to our
     * driver and access freed memory. Always remove the cdev
     * FIRST, then release the number.
     */

    /* Step 3 undo: remove from kernel's device table */
    cdev_del(&hello_dev->cdev);

    /* Step 1 undo: release the device number range */
    unregister_chrdev_region(hello_dev->dev_num, 1);

    /* Free the device struct */
    kfree(hello_dev);
    hello_dev = NULL;

    pr_info("hello_cdev: unregistered %d total reads, final counter=%d\n",
            final_reads, final_counter);
}

/*
 * hello_ioctl - Handle ioctl commands for hello_cdev
 *
 * @filp: Open file (filp->private_data = our device struct)
 * @cmd:  The encoded ioctl command
 * @arg:  The argument (pointer for _IOR/_IOW, ignored for _IO).
*   Q: why the (arg) is of type (unsigned long), not of type (void *) since it is actually an address,
 *     and the ioctl() syscall also gets it as an address? 
 *  A: At the hardware level, a system call passes arguments in registers, not as pointers.
 *     The kernel's syscall entry stub reads a raw register value — there is no "pointer" at this point. It's just an integer that happens to be an address.
 *     The kernel receives this raw integer as (unsigned long) and then passes it to our driver. It only becomes a "pointer" when our driver explicitly casts it when it uses it.
 *     So (unsigned long) is the honest type; it accurately describes what the CPU register contains. Calling it (void *) would be lying about the nature of the data at the syscall boundary.
 *
 * Validation strategy:
 *   Level 1: Check magic number (_IOC_TYPE)
 *   Level 2: Check command range (_IOC_NR)
 *   Level 3: Check direction and size per command
 *
 * Returns: 0 on success, negative errno on failure
 */
static long hello_ioctl(struct file *filp, unsigned int cmd,
                        unsigned long arg)
{
    pr_info("hello_cdev: Entered hello_ioctl()");
    struct hello_device *dev = filp->private_data;

    atomic_inc(&dev->ioctl_count);

    /* ── Level 1: Magic number checking ── */
    if (_IOC_TYPE(cmd) != HELLO_IOC_MAGIC){
        pr_warn("hello_cdev: Magic number checking failed");
        return -ENOTTY; /* Not typewriter; this is the standard errno for iotctl check failure with reason: unknown command.*/
    }
    /* ── Level 2: Command number range ── */
    if (_IOC_NR(cmd) > HELLO_IOC_MAXNR){
        pr_warn("hello_cdev: cmd number checking failed");
        return -ENOTTY;
    }
    /* ── Level 3: Per-command validation + handling ── */
    switch (cmd) {
    case HELLO_GET_COUNT:
    {
        int value;
        unsigned int not_copied;

        /* Validate direction and size */
        if (_IOC_DIR(cmd) != _IOC_READ) {
            pr_warn("hello_cdev: GET_COUNT: wrong direction %u\n",
                    _IOC_DIR(cmd));
            return -EINVAL;
        }
        if (_IOC_SIZE(cmd) != sizeof(int)) {
            pr_warn("hello_cdev: GET_COUNT: wrong size %u\n",
                    _IOC_SIZE(cmd));
            return -EINVAL;
        }

        /* Read WITHOUT incrementing */
        value = atomic_read(&dev->counter);

        /* Copy to userspace */
        not_copied = copy_to_user((int __user *)arg, &value,
                                  sizeof(value));
        if (not_copied != 0) {
            pr_warn("hello_cdev: GET_COUNT: copy_to_user failed\n");
            return -EFAULT;
        }

        pr_debug("hello_cdev: GET_COUNT → %d\n", value);
        return 0;
    }

    case HELLO_SET_COUNT:
    {
        int value;
        unsigned int not_copied;

        /* Validate direction and size */
        if (_IOC_DIR(cmd) != _IOC_WRITE) {
            pr_warn("hello_cdev: SET_COUNT: wrong direction %u\n",
                    _IOC_DIR(cmd));
            return -EINVAL;
        }
        if (_IOC_SIZE(cmd) != sizeof(int)) {
            pr_warn("hello_cdev: SET_COUNT: wrong size %u\n",
                    _IOC_SIZE(cmd));
            return -EINVAL;
        }

        /* Copy from userspace */
        not_copied = copy_from_user(&value, (int __user *)arg,
                                    sizeof(value));
        if (not_copied != 0) {
            pr_warn("hello_cdev: SET_COUNT: copy_from_user failed\n");
            return -EFAULT;
        }

        /* Validate value */
        if (value < 0) {
            pr_warn("hello_cdev: SET_COUNT: negative value %d rejected\n",
                    value);
            return -EINVAL;
        }

        mutex_lock(&dev->lock);
        atomic_set(&dev->counter, value);
        mutex_unlock(&dev->lock);

        pr_info("hello_cdev: SET_COUNT → %d (pid: %d)\n",
                value, current->pid);
        return 0;
    }

    case HELLO_RESET_COUNT:
    {
        /* Validate direction (no data) */
        if (_IOC_DIR(cmd) != _IOC_NONE) {
            pr_warn("hello_cdev: RESET_COUNT: wrong direction %u\n",
                    _IOC_DIR(cmd));
            return -EINVAL;
        }

        mutex_lock(&dev->lock);
        atomic_set(&dev->counter, 0);
        mutex_unlock(&dev->lock);

        pr_info("hello_cdev: RESET_COUNT (pid: %d)\n", current->pid);
        return 0;
    }

    case HELLO_GET_STATS:
    {
        struct hello_stats stats;
        unsigned int not_copied;

        /* Validate direction and size */
        if (_IOC_DIR(cmd) != _IOC_READ) {
            pr_warn("hello_cdev: GET_STATS: wrong direction %u\n",
                    _IOC_DIR(cmd));
            return -EINVAL;
        }
        if (_IOC_SIZE(cmd) != sizeof(struct hello_stats)) {
            pr_warn("hello_cdev: GET_STATS: wrong size %u (expected %zu)\n",
                    _IOC_SIZE(cmd), sizeof(struct hello_stats));
            return -EINVAL;
        }

        /* Populate the stats struct */
        mutex_lock(&dev->lock);
        stats.counter      = atomic_read(&dev->counter);
        stats.total_reads   = atomic_read(&dev->read_count);
        stats.total_writes  = atomic_read(&dev->write_count);
        stats.total_ioctls  = atomic_read(&dev->ioctl_count);
        mutex_unlock(&dev->lock);

        /* Copy to userspace */
        not_copied = copy_to_user((struct hello_stats __user *)arg,
                                  &stats, sizeof(stats));
        if (not_copied != 0) {
            pr_warn("hello_cdev: GET_STATS: copy_to_user failed\n");
            return -EFAULT;
        }

        pr_debug("hello_cdev: GET_STATS → {counter=%d, reads=%d, writes=%d, ioctls=%d}\n",
                 stats.counter, stats.total_reads,
                 stats.total_writes, stats.total_ioctls);
        return 0;
    }

    default:
        /* This should be unreachable due to Level 2 check */
        return -ENOTTY;
    }
}

module_init(hello_cdev_init);
module_exit(hello_cdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mohammed Abdelalim <MohammedAbdelalim14@gmail.com>");
MODULE_DESCRIPTION("Character device driver — returns monotonic counter on read");
MODULE_VERSION("1.0");