/*
*   05_init_and_cleanup_cdev_sequence_boilerplate.c
* 
* This represents the typical startup sequence for a character device driver:
*    (assign major and minor numbers, assign the file operations of this cdev, add the cdev to the kernel)
* Cleanup is always the reverse of init, and must handle partial failures.
*
* Note: This file is not meant to be compiled, I created it for illustration purposes.
*
*/

static dev_t dev_num;       /* Device number */
static struct cdev my_cdev; /* Character device structure */

static int __init hello_init(void)
{
    int ret;

    /* Step 1: Get a device number (major, minor) dynamically,
       try to avoid the static dev number allocation (register_chrdev_region())
    */
    ret = alloc_chrdev_region(&dev_num, 0, 1, "hello_cdev");
    if (ret < 0) {
        pr_err("hello_cdev: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    /* Step 2: Initialize cdev and bind to file_operations */
    cdev_init(&my_cdev, &my_fops);
    my_cdev.owner = THIS_MODULE;

    /* Step 3: Add cdev to the kernel — NOW the device is live */
    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("hello_cdev: cdev_add failed: %d\n", ret);
        unregister_chrdev_region(dev_num, 1);  /* Clean up step 1 */
        return ret;
    }

    pr_info("hello_cdev: registered major=%d minor=%d\n",
            MAJOR(dev_num), MINOR(dev_num));
    return 0;
}

static void __exit hello_exit(void)
{
    /* Step 3 undo: remove cdev from kernel */
    cdev_del(&my_cdev);

    /* Step 1 undo: release the device number */
    unregister_chrdev_region(dev_num, 1);

    pr_info("hello_cdev: unregistered\n");
}