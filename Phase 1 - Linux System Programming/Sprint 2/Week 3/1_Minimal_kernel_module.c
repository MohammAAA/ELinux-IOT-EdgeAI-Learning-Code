#include <linux/init.h>       // __init and __exit macros
#include <linux/module.h>     // MODULE_LICENSE, MODULE_AUTHOR, etc.
#include <linux/kernel.h>     // printk(), pr_info(), KERN_INFO

// This function runs when the module is loaded with insmod
static int __init hello_init(void)
{
    pr_info("hello_module: loaded\n");  // printk with KERN_INFO level
    return 0;  // 0 = success, negative = error code
}

// This function runs when the module is unloaded with rmmod
static void __exit hello_exit(void)
{
    pr_info("hello_module: unloaded\n");
}

// Register the init and exit functions
module_init(hello_init);
module_exit(hello_exit);

// MODULE_LICENSE is NOT optional for practical purposes:
// - "GPL" → your module can access GPL-exported kernel symbols
// - "Proprietary" → you can only use symbols exported to non-GPL modules
// Most useful kernel APIs are GPL-only, so use "GPL"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name <you@example.com>");
MODULE_DESCRIPTION("A minimal kernel module for learning");
MODULE_VERSION("1.0");