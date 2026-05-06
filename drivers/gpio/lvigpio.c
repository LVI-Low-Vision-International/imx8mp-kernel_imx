#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/of.h>

#define DEVICE_NAME "lvigpio"
#define CLASS_NAME "lvigpio_class"

#define LVIGPIO_IOC_MAGIC 'L'
#define LVIGPIO_IOC_READ _IOR(LVIGPIO_IOC_MAGIC, 1, int)

static dev_t dev_num;
static struct cdev lvigpio_cdev;
static struct class *lvigpio_class;
static struct device *lvigpio_device;

struct lvigpio_dev {
	struct device *dev;
	struct gpio_desc *gpiod;
};

static struct lvigpio_dev *lvigpio_data;

static int lvigpio_open(struct inode *inode, struct file *file)
{
	file->private_data = lvigpio_data;
	return 0;
}

static int lvigpio_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long lvigpio_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct lvigpio_dev *data = file->private_data;
	int value;

	if (_IOC_TYPE(cmd) != LVIGPIO_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case LVIGPIO_IOC_READ:
		value = gpiod_get_value(data->gpiod);
		if (copy_to_user((int __user *)arg, &value, sizeof(int)))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return 0;
}

static const struct file_operations lvigpio_fops = {
	.owner = THIS_MODULE,
	.open = lvigpio_open,
	.release = lvigpio_release,
	.unlocked_ioctl = lvigpio_ioctl,
};

static int lvigpio_probe(struct platform_device *pdev)
{
	int ret;

	lvigpio_data =
		devm_kzalloc(&pdev->dev, sizeof(*lvigpio_data), GFP_KERNEL);
	if (!lvigpio_data)
		return -ENOMEM;

	lvigpio_data->dev = &pdev->dev;

	// Get GPIO from device tree (label: "gpio" or "gpios")
	lvigpio_data->gpiod = devm_gpiod_get(&pdev->dev, "seesaw", GPIOD_IN);
	if (IS_ERR(lvigpio_data->gpiod)) {
		dev_err(&pdev->dev, "Failed to get GPIO\n");
		return PTR_ERR(lvigpio_data->gpiod);
	}

	// Allocate char device region
	ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&lvigpio_cdev, &lvigpio_fops);
	ret = cdev_add(&lvigpio_cdev, dev_num, 1);
	if (ret)
		goto err_unregister;

	lvigpio_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(lvigpio_class)) {
		ret = PTR_ERR(lvigpio_class);
		goto err_cdev;
	}

	lvigpio_device =
		device_create(lvigpio_class, NULL, dev_num, NULL, DEVICE_NAME);
	if (IS_ERR(lvigpio_device)) {
		ret = PTR_ERR(lvigpio_device);
		goto err_class;
	}

	platform_set_drvdata(pdev, lvigpio_data);

	dev_info(&pdev->dev, "lvigpio probed successfully\n");
	return 0;

err_class:
	class_destroy(lvigpio_class);
err_cdev:
	cdev_del(&lvigpio_cdev);
err_unregister:
	unregister_chrdev_region(dev_num, 1);
	return ret;
}

static int lvigpio_remove(struct platform_device *pdev)
{
	device_destroy(lvigpio_class, dev_num);
	class_destroy(lvigpio_class);
	cdev_del(&lvigpio_cdev);
	unregister_chrdev_region(dev_num, 1);
	return 0;
}

static const struct of_device_id lvigpio_dt_ids[] = {
	{
		.compatible = "lvi,lvigpio",
	},
	{}
};
MODULE_DEVICE_TABLE(of, lvigpio_dt_ids);

static struct platform_driver lvigpio_driver = {
    .driver = {
        .name = DEVICE_NAME,
        .of_match_table = lvigpio_dt_ids,
    },
    .probe = lvigpio_probe,
    .remove = lvigpio_remove,
};

module_platform_driver(lvigpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("Driver for handling GPIOs on LVIs platform");
