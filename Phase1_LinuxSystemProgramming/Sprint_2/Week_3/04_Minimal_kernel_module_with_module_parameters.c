// SPDX-License-Identifier: GPL-2.0
/*
 * 04_Minimal_kernel_module_with_module_parameters.c - Kernel module with configurable parameters
 * This will be treated as V1.1 of 01_Minimal_kernel_module.c
 *
 * Demonstrates:
 *   - module_param() for basic types (int, bool, charp, ...)
 *   - module_param_named() for name/variable mismatch
 *   - module_param_string() for fixed-size string buffers
 *   - MODULE_PARM_DESC() for documentation
 *   - Parameter validation at module_init time
 *   - sysfs exposure via /sys/module/hello_module/parameters/
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <generated/utsrelease.h>

/*
 * ──────────────────────────────────────────────
 * Module Parameters
 * ──────────────────────────────────────────────
 *
 * Design rules:
 *   1. Always provide a sensible default value
 *   2. Always add MODULE_PARM_DESC() for every parameter
 *   3. Always validate in __init — reject invalid values with pr_err
 *   4. Use 0644 permissions for configurable params
 *   5. Use 0444 for read-only counters/status
 */

/* --- buffer_size: configurable internal buffer size --- */
static int buffer_size = 1024;
module_param(buffer_size, int, 0644);
MODULE_PARM_DESC(buffer_size,
    "Internal buffer size in bytes (min: 64, max: 65536, default: 1024)");

/* --- debug: enable verbose kernel logging --- */
static bool enable_debug = false;
module_param_named(debug, enable_debug, bool, 0644);
MODULE_PARM_DESC(debug,
    "Enable debug-level logging (0=off, 1=on, default: 0)");

/* --- count: how many times read() was called (read-only via sysfs) --- */
static int read_count = 0;
module_param(read_count, int, 0444);
MODULE_PARM_DESC(read_count,
    "Number of times the device has been read (read-only)");

/* --- device_name: fixed-size string parameter --- */
static char device_name[32] = "hello_dev";
module_param_string(name, device_name, sizeof(device_name), 0644);
MODULE_PARM_DESC(name,
    "Device name identifier (up to 31 chars, default: hello_dev)");

/*
 * hello_init - Module initialization with parameter validation
 *
 * IMPORTANT: Validate ALL parameters here. If a parameter value
 * is invalid, return a negative error code. The kernel will
 * refuse to load the module and print the pr_err message.
 *
 * Convention: Use EINVAL for bad values, ENOMEM for impossible
 * allocations.
 */
static int __init hello_init(void)
{
    /* --- Validate buffer_size --- */
    if (buffer_size < 64) {
        pr_err("hello_module: buffer_size=%d is too small (minimum: 64)\n",
               buffer_size);
        return -EINVAL;
    }
    else if (buffer_size > 65536) {
        pr_err("hello_module: buffer_size=%d is too large (maximum: 65536)\n",
               buffer_size);
        return -EINVAL;
    }

    /* --- Validate device_name --- */
    if (strlen(device_name) == 0) {
        pr_err("hello_module: device name cannot be empty\n");
        return -EINVAL;
    }
    else if (strlen(device_name) > 32) {
        pr_err("hello_module: device name is too long (max. 32 chars)\n");
        return -EINVAL;
    }

    /*
     * print all parameters on load — this is standard kernel practice.
     * Every real driver logs its configuration on probe/init so we can
     * verify from dmesg that the correct parameters were applied.
     */
    pr_info("hello_module: loaded\n");
    pr_info("hello_module: buffer_size = %d\n", buffer_size);
    pr_info("hello_module: enable_debug = %s\n", enable_debug ? "on" : "off");
    pr_info("hello_module: device_name = %s\n", device_name);
    pr_info("hello_module: read_count  = %d\n", read_count);

    /* Print kernel version for reference */
    pr_info("hello_module: kernel %s\n", UTS_RELEASE);

    return 0;
}

static void __exit hello_exit(void)
{
    pr_info("hello_module: unloading (final read_count=%d, buffer_size=%d)\n",
            read_count, buffer_size);
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mohammed Abdelalim <mohammedabdelalim14@gmail.com>");
MODULE_DESCRIPTION("Kernel module with configurable parameters and sysfs interface");
MODULE_VERSION("1.1");


