#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/of.h>
#include <linux/cdev.h>
#include <linux/wait.h>      // For wait queues
#include <linux/poll.h>      // For poll
#include <linux/mutex.h>     // For mutex
#include <linux/slab.h>      // For kmalloc and kfree
#include "lvipanel_events.h"

#define DEVICE_NAME "lvipanel"
#define IOCTL_READ_DATA _IOR('i', 1, char)
#define IOCTL_WRITE_DATA _IOW('i', 2, char)

#define TIMER_DELAY 50

/* OF match table to match the device tree node */
static const struct of_device_id lvipanel_of_match[] = {
    { .compatible = "lvipanel", },
    {},
};
MODULE_DEVICE_TABLE(of, lvipanel_of_match);

/* Device-specific data structure */
struct lvipanel_data {
    struct i2c_client *client;
    struct cdev cdev;
    dev_t devt;
    struct class *class;
    wait_queue_head_t wait_queue;
    struct mutex lock;       // For synchronizing access to data_available and buffer
    bool data_available;
    char data_buf;
	struct work_struct work;
	struct timer_list timer;
};

long lvipanel_write_command(struct lvipanel_data *data, char data_buf) {
    pr_info("[%s] call\n", __func__);

    // char data_buf = 0x12; // SYSTEM_EVENT_LIGHT_OFF
    int ret;
    struct i2c_msg msgs[1];

    /* Check if file->private_data is valid */
    if (!data) {
        pr_err("[%s] - Invalid file private data\n", __func__);
        return -ENODEV;
    }

    struct i2c_client *client = data->client;

    /* Check if data->client is valid */
    if (!client) {
        pr_err("[%s] - Invalid I2C client\n", __func__);
        return -ENODEV;
    }

    /* Optional: Check if client->adapter is valid */
    if (!client->adapter) {
        pr_err("[%s] - Invalid I2C adapter\n", __func__);
        return -ENODEV;
    }

    msgs[0].addr  = client->addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &data_buf;

    ret = i2c_transfer(client->adapter, msgs, 1);
    if (ret < 0) {
        pr_err("[%s] - Failed to write to I2C device: %d\n", __func__, ret);
        return ret;
    }

    return 0;
}

static int lvipanel_light_green_flash(struct lvipanel_data *data){
    return lvipanel_write_command(data, SYSTEM_EVENT_LIGHT_GREEN_FLASH);
}

static int lvipanel_light_green_solid(struct lvipanel_data *data){
    return lvipanel_write_command(data, SYSTEM_EVENT_LIGHT_GREEN_SOLID);
}

static int lvipanel_light_yellow_flash(struct lvipanel_data *data){
    return lvipanel_write_command(data, SYSTEM_EVENT_LIGHT_YELLOW_FLASH);
}

static int lvipanel_light_yellow_solid(struct lvipanel_data *data){
    return lvipanel_write_command(data, SYSTEM_EVENT_LIGHT_YELLOW_SOLID);
}

static int lvipanel_light_off(struct lvipanel_data *data){
    return lvipanel_write_command(data, SYSTEM_EVENT_LIGHT_OFF);
}


static ssize_t lvipanel_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct lvipanel_data *data = file->private_data;
    int ret;

    if (!data) {
        pr_err("[%s] - Invalid file private data\n", __func__);
        return -ENODEV;
    }

    // Wait until data is available
    ret = wait_event_interruptible(data->wait_queue, data->data_available);
    if (ret) {
        pr_err("[%s] - wait_event_interruptible interrupted\n", __func__);
        return ret;
    }

    mutex_lock(&data->lock);

    // Copy data to user space
    if (copy_to_user(buf, &data->data_buf, sizeof(data->data_buf))) {
        mutex_unlock(&data->lock);
        pr_err("[%s] - Failed to copy data to user space\n", __func__);
        return -EFAULT;
    }

    data->data_available = false;

    mutex_unlock(&data->lock);

    return sizeof(data->data_buf);
}

static long lvipanel_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    pr_info("[%s] call\n", __func__);

    struct lvipanel_data *data;
    char data_buf;
    int ret;
    struct i2c_msg msgs[1];

    data = file->private_data;

    /* Check if file->private_data is valid */
    if (!data) {
        pr_err("[%s] - Invalid file private data\n", __func__);
        return -ENODEV;
    }

    struct i2c_client *client = data->client;

    /* Check if data->client is valid */
    if (!client) {
        pr_err("[%s] - Invalid I2C client\n", __func__);
        return -ENODEV;
    }

    /* Optional: Check if client->adapter is valid */
    if (!client->adapter) {
        pr_err("[%s] - Invalid I2C adapter\n", __func__);
        return -ENODEV;
    }

    pr_info("[%s] client addr: 0x%x, adapter name: %s\n", __func__, client->addr, client->adapter->name);

    switch (cmd) {
        case IOCTL_READ_DATA:
            // This IOCTL command is no longer needed since we're implementing read()
            pr_err("[%s] - IOCTL_READ_DATA is deprecated, use read() instead\n", __func__);
            return -EINVAL;

        case IOCTL_WRITE_DATA:
            if (copy_from_user(&data_buf, (char __user *)arg, sizeof(data_buf))) {
                pr_err("[%s] - Failed to copy data from user space\n", __func__);
                return -EFAULT;
            }

            msgs[0].addr  = client->addr;
            msgs[0].flags = 0;
            msgs[0].len   = 1;
            msgs[0].buf   = &data_buf;

            ret = i2c_transfer(client->adapter, msgs, 1);
            if (ret < 0) {
                pr_err("[%s] - Failed to write to I2C device: %d\n", __func__, ret);
                return ret;
            }

            return 0;

        default:
            pr_err("[%s] - Invalid IOCTL command\n", __func__);
            return -EINVAL;
    }
}

static unsigned int lvipanel_poll(struct file *file, poll_table *wait) {
    struct lvipanel_data *data = file->private_data;
    unsigned int mask = 0;

    if (!data) {
        pr_err("[%s] - Invalid file private data\n", __func__);
        return POLLERR;
    }

    // Add the wait queue to the poll table
    poll_wait(file, &data->wait_queue, wait);

    mutex_lock(&data->lock);
    // Check if data is available
    if (data->data_available) {
        mask |= POLLIN | POLLRDNORM; // Readable
    }
    mutex_unlock(&data->lock);

    return mask;
}

static int lvipanel_open(struct inode *inode, struct file *file) {
    struct lvipanel_data *data;

    data = container_of(inode->i_cdev, struct lvipanel_data, cdev);
    file->private_data = data;

    pr_info("[%s] call\n", __func__);

    return 0;
}

static int lvipanel_release(struct inode *inode, struct file *file) {
    struct lvipanel_data *data = file->private_data;

    pr_info("[%s] call\n", __func__);

    file->private_data = NULL;

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = lvipanel_open,
    .release = lvipanel_release,
    .read = lvipanel_read,
    .unlocked_ioctl = lvipanel_ioctl,
    .poll = lvipanel_poll,
};

static void lvipanel_work_function(struct work_struct *work) {
    struct lvipanel_data *data = container_of(work, struct lvipanel_data, work);
    struct i2c_client *client = data->client;
    int ret;
    char data_buf;
    struct i2c_msg msgs[1];

    // Read data from the I2C device
    msgs[0].addr  = client->addr;
    msgs[0].flags = I2C_M_RD;
    msgs[0].len   = 1;
    msgs[0].buf   = &data_buf;

    ret = i2c_transfer(client->adapter, msgs, 1);
    if (ret < 0) {
        pr_err("[%s] - Failed to read from I2C device: %d\n", __func__, ret);
        return;
    }

    if (data_buf == 0x0) {
        return;
    }

    mutex_lock(&data->lock);

    data->data_buf = data_buf;
    data->data_available = true;
    wake_up_interruptible(&data->wait_queue);

    mutex_unlock(&data->lock);
}

static void lvipanel_timer_callback(struct timer_list *t) {
    struct lvipanel_data *data = from_timer(data, t, timer);

    // Schedule work to read data from the I2C device
    schedule_work(&data->work);

    // Re-arm the timer
    mod_timer(&data->timer, jiffies + msecs_to_jiffies(TIMER_DELAY)); // Adjust the interval as needed
}

static int lvipanel_probe(struct i2c_client *client, const struct i2c_device_id *id) {
    int ret;
    struct lvipanel_data *data;
    struct device *dev_ret;

    printk("[%s] call\n", __func__);

    if (!client->dev.of_node) {
        pr_err("[%s] - Device tree node not found\n", __func__);
        return -EINVAL;
    }

    data = devm_kzalloc(&client->dev, sizeof(struct lvipanel_data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    dev_set_drvdata(&client->dev, data);

    mutex_init(&data->lock);
    init_waitqueue_head(&data->wait_queue);
    data->data_available = false;

    ret = alloc_chrdev_region(&data->devt, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("[%s] - Failed to allocate char device region\n", __func__);
        return ret;
    }

    cdev_init(&data->cdev, &fops);
    data->cdev.owner = THIS_MODULE;

    ret = cdev_add(&data->cdev, data->devt, 1);
    if (ret) {
        pr_err("[%s] - Failed to add cdev\n", __func__);
        unregister_chrdev_region(data->devt, 1);
        return ret;
    }

    data->class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(data->class)) {
        pr_err("[%s] - Failed to create class\n", __func__);
        cdev_del(&data->cdev);
        unregister_chrdev_region(data->devt, 1);
        return PTR_ERR(data->class);
    }

    dev_ret = device_create(data->class, NULL, data->devt, NULL, DEVICE_NAME);
    if (IS_ERR(dev_ret)) {
        pr_err("[%s] - Failed to create device\n", __func__);
        class_destroy(data->class);
        cdev_del(&data->cdev);
        unregister_chrdev_region(data->devt, 1);
        return PTR_ERR(dev_ret);
    }

    // Initialize work and timer for polling
    INIT_WORK(&data->work, lvipanel_work_function);
    timer_setup(&data->timer, lvipanel_timer_callback, 0);
    mod_timer(&data->timer, jiffies + msecs_to_jiffies(TIMER_DELAY)); // Adjust the interval as needed

    pr_info("[%s] - I2C client successfully initialized: addr=0x%x\n", __func__, client->addr);

    lvipanel_light_green_flash(data);

    return 0;
}

static int lvipanel_remove(struct i2c_client *client) {
    return 0;
}

static void lvipanel_shutdown(struct i2c_client *client) {
    struct lvipanel_data *data = dev_get_drvdata(&client->dev);

    pr_info("[%s] call\n", __func__);

    del_timer_sync(&data->timer);
    cancel_work_sync(&data->work);

    lvipanel_light_off(data);

    device_destroy(data->class, data->devt);
    class_destroy(data->class);
    cdev_del(&data->cdev);
    unregister_chrdev_region(data->devt, 1);
}

static struct i2c_device_id lvipanel_id[] = {
    { DEVICE_NAME, 0 },
    {}
};
MODULE_DEVICE_TABLE(i2c, lvipanel_id);

static struct i2c_driver lvipanel_driver = {
    .driver = {
        .name = DEVICE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = lvipanel_of_match,
    },
    .probe = lvipanel_probe,
    .remove = lvipanel_remove,
    .shutdown = lvipanel_shutdown,
    .id_table = lvipanel_id,
};

module_i2c_driver(lvipanel_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("I2C Example Kernel Module using i2c_msg named lvipanel with DT support");