#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>

#define DEVICE_NAME "lvitest"
#define BUFFER_SIZE 256

// Define the IOCTL commands
#define IOCTL_MAGIC_NUMBER 'k'
#define IOCTL_WRITE_DATA _IOW(IOCTL_MAGIC_NUMBER, 1, char *)
#define IOCTL_READ_DATA  _IOR(IOCTL_MAGIC_NUMBER, 2, char *)

// --- Global variables ---
static int major_number;
static struct class*  ioctl_class  = NULL;
static struct device* ioctl_device = NULL;
static struct cdev ioctl_cdev;
static char kernel_buffer[BUFFER_SIZE] = {0};
static size_t kernel_buffer_len = 0;

// --- Function Prototypes ---
static int     __init ioctl_example_init(void);
static void    __exit ioctl_example_exit(void);
static long    ioctl_example_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

// --- File Operations Structure ---
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = ioctl_example_ioctl,
};

// --- IOCTL Handler ---
static long ioctl_example_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int len;
    int ret = 0;

    switch (cmd) {
        case IOCTL_WRITE_DATA:
            len = strnlen_user((const char __user *)arg, BUFFER_SIZE);
            if (len < 0 || len >= BUFFER_SIZE) {
                pr_err("%s: Invalid user string length\n", DEVICE_NAME);
                return -EINVAL;
            }
            memset(kernel_buffer, 0, BUFFER_SIZE);
            if (copy_from_user(kernel_buffer, (const char __user *)arg, len)) {
                pr_err("%s: Failed to copy data from user\n", DEVICE_NAME);
                return -EFAULT;
            }
            kernel_buffer_len = len;
            pr_info("%s: Received from user: %s\n", DEVICE_NAME, kernel_buffer);
            break;

        case IOCTL_READ_DATA:
            if (copy_to_user((char __user *)arg, kernel_buffer, kernel_buffer_len)) {
                pr_err("%s: Failed to copy data to user\n", DEVICE_NAME);
                return -EFAULT;
            }
            pr_info("%s: Sent to user: %s\n", DEVICE_NAME, kernel_buffer);
            break;

        default:
            pr_warn("%s: Unknown IOCTL command: 0x%x\n", DEVICE_NAME, cmd);
            ret = -EINVAL;
            break;
    }
    return ret;
}

// --- Module Init Function ---
static int __init ioctl_example_init(void) {
    // 1. Allocate a major number dynamically
    if (alloc_chrdev_region(&major_number, 0, 1, DEVICE_NAME) < 0) {
        pr_err("%s: Cannot allocate major number\n", DEVICE_NAME);
        return -1;
    }
    major_number = MAJOR(major_number);
    pr_info("%s: Major number allocated: %d\n", DEVICE_NAME, major_number);

    // 2. Create a device class
    ioctl_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(ioctl_class)) {
        pr_err("%s: Cannot create device class\n", DEVICE_NAME);
        goto r_class;
    }
    pr_info("%s: Device class created\n", DEVICE_NAME);

    // 3. Create the device file
    ioctl_device = device_create(ioctl_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(ioctl_device)) {
        pr_err("%s: Cannot create the device\n", DEVICE_NAME);
        goto r_device;
    }
    pr_info("%s: Device created successfully\n", DEVICE_NAME);

    // 4. Initialize and add the character device
    cdev_init(&ioctl_cdev, &fops);
    if (cdev_add(&ioctl_cdev, MKDEV(major_number, 0), 1) < 0) {
        pr_err("%s: Cannot add the device to the system\n", DEVICE_NAME);
        goto r_cdev;
    }

    pr_info("%s: Module loaded\n", DEVICE_NAME);
    return 0;

r_cdev:
    device_destroy(ioctl_class, MKDEV(major_number, 0));
r_device:
    class_destroy(ioctl_class);
r_class:
    unregister_chrdev_region(MKDEV(major_number, 0), 1);
    return -1;
}

// --- Module Exit Function ---
static void __exit ioctl_example_exit(void) {
    device_destroy(ioctl_class, MKDEV(major_number, 0));
    class_destroy(ioctl_class);
    cdev_del(&ioctl_cdev);
    unregister_chrdev_region(MKDEV(major_number, 0), 1);
    pr_info("%s: Module unloaded\n", DEVICE_NAME);
}

module_init(ioctl_example_init);
module_exit(ioctl_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub Copilot");
MODULE_DESCRIPTION("A minimal Linux driver for IOCTL functionality");