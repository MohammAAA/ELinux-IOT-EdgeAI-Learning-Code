// SPDX-License-Identifier: GPL-2.0
/* The "// SPDX-License-Identifier: <identifier>" is not a comment for humans
*  it's a machine-readable license tag that the kernel's build system and compliance tools parse.
*  Key points:
*     - Required by the kernel community: Since Linux 4.17, the kernel source (python script) checker (scripts/spdxcheck.py) will warn if this is missing.
*     - Must match MODULE_LICENSE: If SPDX-License-Identifier says GPL-2.0 but MODULE_LICENSE("MIT"), we have a contradiction.
*     - Format: Exactly SPDX-License-Identifier: <identifier>. The valid identifiers are listed at spdx.org/licenses.
*  For our learning modules: Use GPL-2.0 since our MODULE_LICENSE is "GPL".
*/


/*
 * 1_Minimal_kernel_module.c - First kernel module for learning
 *
 * This is the simplest possible loadable kernel module.
 * It prints a message when loaded and another when unloaded.
 */

#include <linux/init.h>       /* __init, __exit macros           */
#include <linux/module.h>     /* MODULE_LICENSE, MODULE_AUTHOR   */
#include <linux/kernel.h>     /* pr_info(), pr_err()             */
#include <generated/utsrelease.h> /* UTS_RELEASE, needed for modern kernel versions (>2.4)*/


/*
 * hello_init - Module initialization function
 *
 * Called by the kernel when we run: insmod hello_module.ko
 * Returns 0 on success, negative error code on failure.
 *
 * The __init macro places this function in the .init.text ELF section.
 * Modern kernels (>2.4) free this memory immediately after init returns.
 */
static int __init hello_init(void)
{
    pr_info("hello_module: loaded into kernel\n");
    pr_info("hello_module: running on CPU %d in process context\n",
            smp_processor_id());
    pr_info("hello_module: kernel version: %s\n", UTS_RELEASE);

    return 0;  /* 0 = success, anything negative = failure */
}

/*
 * hello_exit - Module cleanup function
 *
 * Called by the kernel when we run: rmmod hello_module
 * This function returns void; it cannot fail.
 *
 * The __exit macro:
 *   - For loadable modules (obj -m): kept in core memory, freed on rmmod
 *   - For built-in (obj-y): discarded at link time (unreachable)
 */
static void __exit hello_exit(void)
{
    pr_info("hello_module: unloading from kernel\n");
}

/*
 * module_init - Tell the kernel which function is the entry point
 *
 * This macro defines a special ELF symbol that the kernel's module
 * loader looks for when you run insmod. The loader:
 *   1. Reads the .ko ELF file
 *   2. Scans for the "init" symbol in .gnu.linkonce.this_module
 *   3. Calls the function it points to (hello_init)
 *
 * We MUST have exactly one module_init() per module.
 */
module_init(hello_init);

/*
 * module_exit - Tell the kernel which function is the cleanup handler
 *
 * The module loader stores this function pointer in struct module->exit.
 * When rmmod is called, the kernel:
 *   1. Checks the module's reference count (i.e.: number of dependents on this module) (must be 0)
 *   2. Calls the exit function
 *   3. Unregisters from all subsystems
 *   4. Frees the module's memory
 *
 * We MUST have exactly one module_exit() per loadable module.
 */
module_exit(hello_exit);

/*
 * MODULE_LICENSE - Declares our module's license
 *
 * This is NOT just documentation; it has a real, enforced effect:
 *
 * "GPL" or "GPL v2":
 *   → Our module can access symbols exported with EXPORT_SYMBOL_GPL()
 *   → This includes most useful kernel APIs: kmalloc, printk,
 *     cdev_add, i2c_transfer, etc.
 *
 * "Dual BSD/GPL", "Dual MIT/GPL":
 *   → Same access as "GPL"
 *
 * "Proprietary":
 *   → We can ONLY access symbols exported with EXPORT_SYMBOL()
 *   → Many kernel subsystems are GPL-only — Our module won't build
 *
 * If we omit MODULE_LICENSE entirely:
 *   → Taints the kernel (visible in /proc/sys/kernel/tainted)
 *   → Warning printed at load time
 *   → GPL-only symbols are inaccessible
 *
 * The kernel community checks this. Companies like Nvidia use
 * "Dual BSD/GPL" or "NVIDIA" for their proprietary drivers, which
 * taints the kernel and prevents access to GPL-only internal APIs.
 */
MODULE_LICENSE("GPL");

/*
 * MODULE_AUTHOR - Who wrote this module
 *
 * Stored in the .modinfo ELF section. Readable with modinfo.
 * Format convention: "Name <email>"
 */
MODULE_AUTHOR("Mohammed Abdelalim <mohammedabdelalim14@gmail.com>");

/*
 * MODULE_DESCRIPTION - One-line summary of what this module does
 *
 * Also stored in .modinfo. Visible via modinfo.
 * Keep it under 80 characters — it's displayed in kernel logs.
 */
MODULE_DESCRIPTION("First loadable kernel module — prints hello on load/unload");

/*
 * MODULE_VERSION - Optional version string
 *
 * Useful when we're developing a driver and want to track which
 * version is loaded on a system. Visible via modinfo.
 */
MODULE_VERSION("1.0");