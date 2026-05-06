#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/ioctl.h>
#include <linux/math64.h>

#include <linux/lviconfig_parameters.h>
#include <linux/notifier.h>

#define PWM_MAGIC 'P'

#define PWM_IOCTL_SET_DUTY _IOW(PWM_MAGIC, 0, unsigned long)
#define PWM_IOCTL_SET_PERIOD _IOW(PWM_MAGIC, 1, unsigned long)
#define PWM_IOCTL_ENABLE _IO(PWM_MAGIC, 2)
#define PWM_IOCTL_DISABLE _IO(PWM_MAGIC, 3)
#define PWM_IOCTL_GET_DUTY _IOR(PWM_MAGIC, 4, unsigned long)

#define DEVICE_NAME "lvipwm"

struct lvipwm_drvdata {
	struct pwm_device *pwm;
	struct cdev cdev;
	struct pwm_state state; // Per-device state
	u64 max_duty_cycle;
	struct device *device;
	dev_t devno;
	struct notifier_block lviconfig_nb;
};

static int major;
static struct class *pwm_class;

static u64 lvipwm_scale_to_limited_duty(struct lvipwm_drvdata *drvdata,
						u64 requested_duty)
{
	if (!drvdata->state.period)
		return 0;

	if (requested_duty > drvdata->state.period)
		requested_duty = drvdata->state.period;

	return div64_u64(requested_duty * drvdata->max_duty_cycle,
			 drvdata->state.period);
}

/**
 * lvipwm_lviconfig_notifier - react to a confLighting.Intensity change.
 * Re-applies the PWM duty cycle whenever the Intensity parameter is
 * updated through lviconfig.
 */
static int lvipwm_lviconfig_notifier(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct lvipwm_drvdata *drvdata =
		container_of(nb, struct lvipwm_drvdata, lviconfig_nb);
	ConfigParam *param = (ConfigParam *)data;
	u64 duty;
	u64 limited_duty;
	int ret;

	if (param != &confLighting.Intensity)
		return NOTIFY_DONE;

	duty = confLighting.Intensity.data[0] |
	       ((u64)confLighting.Intensity.data[1] << 8);

	limited_duty = lvipwm_scale_to_limited_duty(drvdata, duty);
	drvdata->state.duty_cycle = limited_duty;
	ret = pwm_apply_state(drvdata->pwm, &drvdata->state);
	if (ret < 0)
		pr_err("[lvipwm] notifier: pwm_apply_state failed: %d\n", ret);
	else
		pr_info("[lvipwm] notifier: duty updated req=%llu ns applied=%llu ns\n",
			duty, limited_duty);

	return NOTIFY_OK;
}

static long lvipwm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lvipwm_drvdata *drvdata = file->private_data;
	struct pwm_device *pwm = drvdata->pwm;
	unsigned long value;
	u64 limited_duty;
	int ret;

	if (!pwm) {
		pr_err("[%s] PWM device is NULL!\n", __func__);
		return -ENODEV;
	}

	pr_info("[%s] IOCTL cmd=0x%x\n", __func__, cmd);

	switch (cmd) {
	case PWM_IOCTL_SET_DUTY:
		if (copy_from_user(&value, (unsigned long __user *)arg, sizeof(unsigned long))) {
			pr_err("[%s] copy_from_user failed for SET_DUTY\n", __func__);
			return -EFAULT;
		}

		// Validate duty cycle doesn't exceed logical period
		if (value > drvdata->state.period) {
			pr_err("[%s] Duty cycle %lu exceeds period %llu\n", __func__, value, drvdata->state.period);
			return -EINVAL;
		}

		limited_duty = lvipwm_scale_to_limited_duty(drvdata, value);

		pr_info("[%s] Setting duty cycle req=%lu ns applied=%llu ns (period=%llu max=%llu)\n",
			__func__, value, limited_duty, drvdata->state.period, drvdata->max_duty_cycle);

		drvdata->state.duty_cycle = limited_duty;
		ret = pwm_apply_state(pwm, &drvdata->state);
		if (ret < 0) {
			pr_err("[%s] pwm_apply_state failed: %d\n", __func__, ret);
			return ret;
		}
		break;

	case PWM_IOCTL_SET_PERIOD:
		if (copy_from_user(&value, (unsigned long __user *)arg, sizeof(unsigned long))) {
			pr_err("[%s] copy_from_user failed for SET_PERIOD\n", __func__);
			return -EFAULT;
		}

		pr_info("[%s] Setting period to %lu\n", __func__, value);
		drvdata->state.period = value;

		// Adjust duty cycle if it exceeds new period
		if (drvdata->state.duty_cycle > drvdata->state.period) {
			drvdata->state.duty_cycle = drvdata->state.period;
		}

		ret = pwm_apply_state(pwm, &drvdata->state);
		if (ret < 0) {
			pr_err("[%s] pwm_apply_state failed: %d\n", __func__, ret);
			return ret;
		}
		break;

	case PWM_IOCTL_GET_DUTY:
		value = drvdata->state.duty_cycle;
		if (copy_to_user((unsigned long __user *)arg, &value, sizeof(unsigned long))) {
			pr_err("[%s] copy_to_user failed for GET_DUTY\n", __func__);
			return -EFAULT;
		}
		pr_info("[%s] Getting duty cycle: %lu\n", __func__, value);
		return 0;

	case PWM_IOCTL_ENABLE:
		pr_info("[%s] Enabling PWM\n", __func__);
		drvdata->state.enabled = true;
		ret = pwm_apply_state(pwm, &drvdata->state);
		if (ret < 0) {
			pr_err("[%s] pwm_apply_state failed: %d\n", __func__, ret);
			return ret;
		}
		break;

	case PWM_IOCTL_DISABLE:
		pr_info("[%s] Disabling PWM\n", __func__);
		drvdata->state.enabled = false;
		ret = pwm_apply_state(pwm, &drvdata->state);
		if (ret < 0) {
			pr_err("[%s] pwm_apply_state failed: %d\n", __func__, ret);
			return ret;
		}
		break;

	default:
		pr_err("[%s] Invalid IOCTL cmd=0x%x\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
}

static int lvipwm_open(struct inode *inode, struct file *file)
{
	struct lvipwm_drvdata *drvdata;

	pr_info("[%s] called\n", __func__);
	drvdata = container_of(inode->i_cdev, struct lvipwm_drvdata, cdev);
	file->private_data = drvdata;

	return 0;
}

static int lvipwm_release(struct inode *inode, struct file *file)
{
	pr_info("[%s] called\n", __func__);
	return 0;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = lvipwm_open,
	.release = lvipwm_release,
	.unlocked_ioctl = lvipwm_ioctl,
};

static int lvipwm_probe(struct platform_device *pdev)
{
	struct lvipwm_drvdata *drvdata;
	struct device *dev = &pdev->dev;
	struct pwm_args pargs;
	u32 max_duty_percent;
	u64 requested_duty;
	int ret;

	pr_info("[%s] called\n", __func__);

	if (ConfigParam_IsInitialized() == 0) {
		pr_warn("[%s] ConfigParam not yet initialized, deferring probe\n", __func__);
		return -EPROBE_DEFER;
	} else {
		pr_info("[%s] ConfigParam is initialized, proceeding with probe\n", __func__);
	}

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		pr_err("[%s] Failed to allocate drvdata\n", __func__);
		return -ENOMEM;
	}

	drvdata->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(drvdata->pwm)) {
		ret = PTR_ERR(drvdata->pwm);
		pr_err("[%s] Unable to get PWM device: %d\n", __func__, ret);
		return ret;
	}

	pr_info("[%s] PWM device fetched from device tree\n", __func__);

	ret = alloc_chrdev_region(&drvdata->devno, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_err("[%s] alloc_chrdev_region failed: %d\n", __func__, ret);
		return ret;
	}

	major = MAJOR(drvdata->devno);

	cdev_init(&drvdata->cdev, &fops);
	drvdata->cdev.owner = THIS_MODULE;

	ret = cdev_add(&drvdata->cdev, drvdata->devno, 1);
	if (ret < 0) {
		pr_err("[%s] cdev_add failed: %d\n", __func__, ret);
		goto unregister_region;
	}

	pwm_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(pwm_class)) {
		ret = PTR_ERR(pwm_class);
		pr_err("[%s] class_create failed: %d\n", __func__, ret);
		goto del_cdev;
	}

	drvdata->device = device_create(pwm_class, NULL, drvdata->devno, NULL, DEVICE_NAME);
	if (IS_ERR(drvdata->device)) {
		ret = PTR_ERR(drvdata->device);
		pr_err("[%s] device_create failed: %d\n", __func__, ret);
		goto destroy_class;
	}

	pr_info("[%s] Device created: %s\n", __func__, DEVICE_NAME);

	platform_set_drvdata(pdev, drvdata);

	/* Initialize PWM state from device tree and EEPROM config */
	pwm_get_args(drvdata->pwm, &pargs);
	pwm_init_state(drvdata->pwm, &drvdata->state);

	drvdata->state.period = pargs.period;
	drvdata->state.polarity = pargs.polarity;
	drvdata->max_duty_cycle = drvdata->state.period;

	if (!of_property_read_u32(dev->of_node, "lvi,max-duty-percent", &max_duty_percent)) {
		if (!max_duty_percent || max_duty_percent > 100) {
			dev_warn(dev, "invalid lvi,max-duty-percent=%u, using 100\n",
				 max_duty_percent);
			max_duty_percent = 100;
		}

		drvdata->max_duty_cycle = div64_u64(drvdata->state.period * max_duty_percent,
							100);
	}

	// Read logical duty cycle from EEPROM config
	requested_duty = confLighting.Intensity.data[0] |
			 ((u64)confLighting.Intensity.data[1] << 8);
	drvdata->state.duty_cycle = lvipwm_scale_to_limited_duty(drvdata, requested_duty);

	if (drvdata->max_duty_cycle > drvdata->state.period)
		drvdata->max_duty_cycle = drvdata->state.period;

	drvdata->state.enabled = true;

	pr_info("[%s] Initializing PWM: period=%llu ns, requested=%llu ns, applied=%llu ns, max=%llu ns, polarity=%d\n",
		__func__, drvdata->state.period, requested_duty, drvdata->state.duty_cycle,
		drvdata->max_duty_cycle, drvdata->state.polarity);

	ret = pwm_apply_state(drvdata->pwm, &drvdata->state);
	if (ret < 0) {
		pr_err("[%s] pwm_apply_state failed: %d\n", __func__, ret);
		goto destroy_device;
	}

	pr_info("[%s] PWM state applied successfully\n", __func__);
	pr_info("[%s] lvipwm driver probed successfully\n", __func__);

	drvdata->lviconfig_nb.notifier_call = lvipwm_lviconfig_notifier;
	lviconfig_register_notifier(&drvdata->lviconfig_nb);

	return 0;

destroy_device:
	device_destroy(pwm_class, drvdata->devno);
destroy_class:
	class_destroy(pwm_class);
del_cdev:
	cdev_del(&drvdata->cdev);
unregister_region:
	unregister_chrdev_region(drvdata->devno, 1);
	return ret;
}

static int lvipwm_remove(struct platform_device *pdev)
{
	struct lvipwm_drvdata *drvdata = platform_get_drvdata(pdev);

	pr_info("[%s] called\n", __func__);

	if (drvdata) {
		lviconfig_unregister_notifier(&drvdata->lviconfig_nb);
		pwm_disable(drvdata->pwm);
		device_destroy(pwm_class, drvdata->devno);
		class_destroy(pwm_class);
		cdev_del(&drvdata->cdev);
		unregister_chrdev_region(drvdata->devno, 1);
	}

	pr_info("[%s] lvipwm driver removed\n", __func__);
	return 0;
}

static const struct of_device_id lvipwm_of_match[] = { { .compatible = "lvi,lvipwm" }, { /* sentinel */ } };
MODULE_DEVICE_TABLE(of, lvipwm_of_match);

static struct platform_driver lvipwm_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = lvipwm_of_match,
	},
	.probe = lvipwm_probe,
	.remove = lvipwm_remove,
};

module_platform_driver(lvipwm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("PWM driver using platform device model");