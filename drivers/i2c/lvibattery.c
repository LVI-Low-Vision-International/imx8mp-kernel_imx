#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/of.h>
 
/* ── device names ─────────────────────────────────────────────────────────── */
#define BATTERY_DEVICE_NAME "lvibattery"
#define CHARGER_DEVICE_NAME "lvicharger"
 
/* ── IOCTL definitions ────────────────────────────────────────────────────── */
#define IOCTL_READ_DATA _IOR('b', 1, struct i2c_data)
#define IOCTL_WRITE_DATA _IOW('b', 2, struct i2c_data)
#define IOCTL_READ_STRING _IOR('b', 3, struct i2c_string_data)
 
#define IOCTL_CHARGER_READ_DATA _IOR('c', 1, struct i2c_data)
#define IOCTL_CHARGER_WRITE_DATA _IOW('c', 2, struct i2c_data)
 
/* ── shared structs ───────────────────────────────────────────────────────── */
struct i2c_data {
    unsigned char reg;
    unsigned short value;
};
 
#define MAX_STRING_SIZE 32
struct i2c_string_data {
    unsigned char reg;
    unsigned char length;
    char value[MAX_STRING_SIZE];
};
 
/* ── driver state ─────────────────────────────────────────────────────────── */
static int battery_major, charger_major;
static struct class *battery_class, *charger_class;
static struct i2c_client *battery_client, *charger_client;
 
/* ══════════════════════════════════════════════════════════════════════════
 * Shared helper — performs a raw I2C word read
 * ══════════════════════════════════════════════════════════════════════════ */
static int do_i2c_read(struct i2c_client *client, struct i2c_data *d)
{
    struct i2c_msg msgs[2];
    unsigned char reg = d->reg;
    unsigned char buf[2] = { 0 };
    int ret;
 
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
 
    msgs[1].addr = client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = 2;
    msgs[1].buf = buf;
 
    ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret < 0) {
        pr_err("[%s] i2c_transfer read failed for addr=0x%x reg=0x%x: %d\n", __func__, client->addr, reg, ret);
        return ret;
    }
 
    d->value = (buf[1] << 8) | buf[0];
    pr_info("[%s] addr=0x%x reg=0x%02x value=0x%04x\n", __func__, client->addr, reg, d->value);
    return 0;
}
 
/* ══════════════════════════════════════════════════════════════════════════
 * Shared helper — performs a raw I2C word write
 * ══════════════════════════════════════════════════════════════════════════ */
static int do_i2c_write(struct i2c_client *client, struct i2c_data *d)
{
    struct i2c_msg msgs[1];
    unsigned char buf[3];
    int ret;
 
    buf[0] = d->reg;
    buf[1] = d->value & 0xFF; /* LSB */
    buf[2] = (d->value >> 8) & 0xFF; /* MSB */
 
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 3;
    msgs[0].buf = buf;
 
    pr_info("[%s] addr=0x%x reg=0x%02x value=0x%04x\n", __func__, client->addr, d->reg, d->value);
 
    ret = i2c_transfer(client->adapter, msgs, 1);
    if (ret < 0) {
        pr_err("[%s] i2c_transfer write failed for addr=0x%x reg=0x%x: %d\n", __func__, client->addr, d->reg, ret);
        return ret;
    }
    return 0;
}
 
/* ══════════════════════════════════════════════════════════════════════════
 * Battery IOCTL  (/dev/lvibattery)
 * ══════════════════════════════════════════════════════════════════════════ */
static long battery_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct i2c_data reg_data;
    struct i2c_string_data string_data;
    struct i2c_msg msgs[2];
    unsigned char reg;
    int ret;
 
    pr_info("[%s] cmd=0x%x\n", __func__, cmd);
 
    switch (cmd) {
    case IOCTL_READ_DATA:
        if (copy_from_user(&reg_data, (void __user *)arg, sizeof(reg_data)))
            return -EFAULT;
        ret = do_i2c_read(battery_client, &reg_data);
        if (ret < 0)
            return ret;
        if (copy_to_user((void __user *)arg, &reg_data, sizeof(reg_data)))
            return -EFAULT;
        return 0;
 
    case IOCTL_WRITE_DATA:
        if (copy_from_user(&reg_data, (void __user *)arg, sizeof(reg_data)))
            return -EFAULT;
        return do_i2c_write(battery_client, &reg_data);
 
    case IOCTL_READ_STRING:
        if (copy_from_user(&string_data, (void __user *)arg, sizeof(string_data)))
            return -EFAULT;
        if (string_data.length > MAX_STRING_SIZE)
            return -EINVAL;
 
        reg = string_data.reg;
        msgs[0].addr = battery_client->addr;
        msgs[0].flags = 0;
        msgs[0].len = 1;
        msgs[0].buf = &reg;
        msgs[1].addr = battery_client->addr;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = string_data.length;
        msgs[1].buf = string_data.value;
 
        ret = i2c_transfer(battery_client->adapter, msgs, 2);
        if (ret < 0) {
            pr_err("[%s] string read failed: %d\n", __func__, ret);
            return ret;
        }
        if (copy_to_user((void __user *)arg, &string_data, sizeof(string_data)))
            return -EFAULT;
        return 0;
 
    default:
        pr_err("[%s] unknown cmd=0x%x\n", __func__, cmd);
        return -EINVAL;
    }
}
 
/* ══════════════════════════════════════════════════════════════════════════
 * Charger IOCTL  (/dev/lvicharger)
 * ══════════════════════════════════════════════════════════════════════════ */
static long charger_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct i2c_data reg_data;
    int ret;
 
    pr_info("[%s] cmd=0x%x\n", __func__, cmd);
 
    switch (cmd) {
    case IOCTL_CHARGER_READ_DATA:
        if (copy_from_user(&reg_data, (void __user *)arg, sizeof(reg_data)))
            return -EFAULT;
        ret = do_i2c_read(charger_client, &reg_data);
        if (ret < 0)
            return ret;
        if (copy_to_user((void __user *)arg, &reg_data, sizeof(reg_data)))
            return -EFAULT;
        return 0;
 
    case IOCTL_CHARGER_WRITE_DATA:
        if (copy_from_user(&reg_data, (void __user *)arg, sizeof(reg_data)))
            return -EFAULT;
        return do_i2c_write(charger_client, &reg_data);
 
    default:
        pr_err("[%s] unknown cmd=0x%x\n", __func__, cmd);
        return -EINVAL;
    }
}
 
/* ── file_operations ──────────────────────────────────────────────────────── */
static int generic_open(struct inode *inode, struct file *file)
{
    pr_info("[%s] %s opened\n", __func__, file->f_path.dentry->d_name.name);
    return 0;
}
static int generic_release(struct inode *inode, struct file *file)
{
    pr_info("[%s] %s released\n", __func__, file->f_path.dentry->d_name.name);
    return 0;
}
 
static struct file_operations battery_fops = {
    .owner = THIS_MODULE,
    .open = generic_open,
    .release = generic_release,
    .unlocked_ioctl = battery_ioctl,
};
 
static struct file_operations charger_fops = {
    .owner = THIS_MODULE,
    .open = generic_open,
    .release = generic_release,
    .unlocked_ioctl = charger_ioctl,
};
 
/* ══════════════════════════════════════════════════════════════════════════
 * Helper — register a char device + class + node, returns 0 or -errno
 * ══════════════════════════════════════════════════════════════════════════ */
static int register_chardev_node(const char *name, struct file_operations *fops, int *out_major, struct class **out_class)
{
    int major;
    struct class *cls;
 
    major = register_chrdev(0, name, fops);
    if (major < 0) {
        pr_err("[%s] register_chrdev failed for %s: %d\n", __func__, name, major);
        return major;
    }
 
    cls = class_create(THIS_MODULE, name);
    if (IS_ERR(cls)) {
        unregister_chrdev(major, name);
        return PTR_ERR(cls);
    }
 
    if (IS_ERR(device_create(cls, NULL, MKDEV(major, 0), NULL, name))) {
        class_destroy(cls);
        unregister_chrdev(major, name);
        return -ENOMEM;
    }
 
    *out_major = major;
    *out_class = cls;
    pr_info("[%s] /dev/%s created (major=%d)\n", __func__, name, major);
    return 0;
}
 
static void unregister_chardev_node(const char *name, int major, struct class *cls)
{
    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);
    unregister_chrdev(major, name);
    pr_info("[%s] /dev/%s removed\n", __func__, name);
}
 
/* ══════════════════════════════════════════════════════════════════════════
 * Probe / remove
 * ══════════════════════════════════════════════════════════════════════════ */
static int lvibattery_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    unsigned char test_byte;
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = I2C_M_RD,
        .len = 1,
        .buf = &test_byte,
    };
    int ret;
 
    pr_info("[%s] probing addr=0x%x\n", __func__, client->addr);
 
    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret < 0) {
        pr_err("[%s] device not responding at addr=0x%x: %d\n", __func__, client->addr, ret);
        return -ENODEV;
    }
 
    /* Distinguish battery (0x0b) from charger (0x09) by address */
    if (client->addr == 0x0b) {
        battery_client = client;
        ret = register_chardev_node(BATTERY_DEVICE_NAME, &battery_fops, &battery_major, &battery_class);
        if (ret < 0)
            return ret;
        pr_info("[%s] battery client ready\n", __func__);
    } else if (client->addr == 0x09) {
        charger_client = client;
        ret = register_chardev_node(CHARGER_DEVICE_NAME, &charger_fops, &charger_major, &charger_class);
        if (ret < 0)
            return ret;
        pr_info("[%s] charger client ready\n", __func__);
    } else {
        pr_err("[%s] unexpected addr=0x%x\n", __func__, client->addr);
        return -ENODEV;
    }
 
    return 0;
}
 
static int lvibattery_remove(struct i2c_client *client)
{
    pr_info("[%s] removing addr=0x%x\n", __func__, client->addr);
 
    if (client->addr == 0x0b && battery_class)
        unregister_chardev_node(BATTERY_DEVICE_NAME, battery_major, battery_class);
    else if (client->addr == 0x09 && charger_class)
        unregister_chardev_node(CHARGER_DEVICE_NAME, charger_major, charger_class);
    return 0;
}
 
/* ── module tables ────────────────────────────────────────────────────────── */
static const struct of_device_id lvibattery_of_match[] = {
    { .compatible = "lvi,lvibattery" },
    { .compatible = "lvi,lvicharger" },
    {},
};
MODULE_DEVICE_TABLE(of, lvibattery_of_match);
 
static struct i2c_device_id lvibattery_id[] = {
    { BATTERY_DEVICE_NAME, 0 },
    { CHARGER_DEVICE_NAME, 0 },
    {},
};
MODULE_DEVICE_TABLE(i2c, lvibattery_id);
 
static struct i2c_driver lvibattery_driver = {
    .driver = {
        .name          = BATTERY_DEVICE_NAME,
        .owner         = THIS_MODULE,
        .of_match_table = lvibattery_of_match,
    },
    .probe    = lvibattery_probe,
    .remove   = lvibattery_remove,
    .id_table = lvibattery_id,
};
module_i2c_driver(lvibattery_driver);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("I2C driver for smart battery (0x0b) and charger (0x09)");