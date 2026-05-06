#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>

#include "ncs8801s.h"



static struct ncs8801s *ncs8801s = NULL;

static int ncs8801s_read(struct i2c_client *i2c, char addr, char reg, char *val)
{
	int ret = -1;
	s32 retries = 0;
	struct i2c_msg msgs[2];

	pr_info("[%s] called\n", __func__);

	msgs[0].flags = !I2C_M_RD;
	msgs[0].addr = addr;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr = addr;
	msgs[1].len = 1;
	msgs[1].buf = val;

	while (retries < 3) {
		ret = i2c_transfer(i2c->adapter, msgs, 2);
		if (ret == 2)
			break;
		retries++;
		pr_err("[%s] Retrying I2C read (retry %d)\n", __func__,
		       retries);
	}

	if (ret == 2)
		pr_info("[%s] I2C read successful: addr=0x%02x, reg=0x%02x, val=0x%02x\n",
			__func__, addr, reg, *val);
	else
		pr_err("[%s] I2C read failed: addr=0x%02x, reg=0x%02x\n",
		       __func__, addr, reg);

	return ret;
}

static int ncs8801s_write(struct i2c_client *i2c, char addr, char reg, char val)
{
	int ret = -1;
	struct i2c_msg msg;
	char tx_buf[2];

	pr_info("[%s] called\n", __func__);

	tx_buf[0] = reg;
	tx_buf[1] = val;

	msg.addr = addr;
	msg.buf = tx_buf;
	msg.len = 2;
	msg.flags = i2c->flags;

	ret = i2c_transfer(i2c->adapter, &msg, 1);

	if (ret < 0)
		pr_err("[%s] Failed to write: addr=0x%02x, reg=0x%02x, val=0x%02x, ret=%d\n",
		       __func__, addr, reg, val, ret);
	else
		pr_info("[%s] Write: addr=0x%02x, reg=0x%02x, val=0x%02x, ret=%d\n",
			__func__, addr, reg, val, ret);
	return ret;
}

static int ncs8801s_write_list(struct ncs8801s *ncs8801s, char addr,
			       const struct reg_data *list)
{
	int len = 0;
	pr_info("[%s] called\n", __func__);

	if (addr == NCS8801S_ID1_ADDR) {
		len = (ncs8801s->screen_w == 1920) ? ID1_1920_1080_REG_LEN :
						     ID1_1366_768_REG_LEN;
	} else if (addr == NCS8801S_ID2_ADDR) {
		len = (ncs8801s->screen_w == 1920) ? ID2_1920_1080_REG_LEN :
						     ID2_1366_768_REG_LEN;
	} else {
		len = ID3_1920_1080_REG_LEN;
	}

	while (len--) {
		ncs8801s_write(ncs8801s->i2c, addr, list->reg, list->val);
		list++;
	}

	pr_info("[%s] Write list completed for addr=0x%02x\n", __func__, addr);
	return 0;
}

static void ncs8801s_set_brightness(struct ncs8801s *ncs8801s, u32 brightness)
{
	u32 duty_cycle;
	pr_info("[%s] called\n", __func__);

	if (!ncs8801s->bl_pwm) {
		pr_err("[%s] PWM device not initialized for brightness control.\n",
		       __func__);
		return;
	}

	if (brightness > ncs8801s->max_brightness)
		brightness = ncs8801s->max_brightness;

	duty_cycle = (brightness * pwm_get_period(ncs8801s->bl_pwm)) /
		     ncs8801s->max_brightness;

	pwm_config(ncs8801s->bl_pwm, duty_cycle,
		   pwm_get_period(ncs8801s->bl_pwm));
	pwm_enable(ncs8801s->bl_pwm);

	pr_info("[%s] Brightness set to %u/%u (duty cycle: %u)\n", __func__,
		brightness, ncs8801s->max_brightness, duty_cycle);
}

static int ncs8801s_init(void)
{
	pr_info("[%s] called\n", __func__);
	pr_info("NCS8801S init...\n");
	if (ncs8801s != NULL) {
		int input_hactive = 0;
		char input_hactive_high = 0;
		char input_hactive_low = 0;
		// ncs8801s_read(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0xE4, &input_hactive_high);
		// ncs8801s_read(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0xE5, &input_hactive_low);
		input_hactive = input_hactive_low | (input_hactive_high << 8);

		pr_info("[%s] hactive 0x%x\n", __func__, input_hactive);
		// if (input_hactive == 0) {
		//power init

		pr_info("[%s] pwd_pin: %px, rst_pin: %px\n", __func__,
			ncs8801s->pwd_pin, ncs8801s->rst_pin);
		if (IS_ERR(ncs8801s->pwd_pin))
			pr_err("[%s] pwd_pin is error: %ld\n", __func__,
			       PTR_ERR(ncs8801s->pwd_pin));
		if (IS_ERR(ncs8801s->rst_pin))
			pr_err("[%s] rst_pin is error: %ld\n", __func__,
			       PTR_ERR(ncs8801s->rst_pin));

		//cansleep must be used when PCAL953x (PCA6416) is used as GPIO expander

		gpiod_set_value_cansleep(ncs8801s->pwd_pin, 0);
		gpiod_set_value_cansleep(ncs8801s->rst_pin, 0);
		usleep_range(3000, 6000);
		gpiod_set_value_cansleep(ncs8801s->pwd_pin, 1);
		gpiod_set_value_cansleep(ncs8801s->rst_pin, 1);
		usleep_range(3000, 6000);
		gpiod_set_value_cansleep(ncs8801s->pwd_pin, 0);
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(ncs8801s->rst_pin, 0);

		ncs8801s_write(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0x0f, 0x01);

		if (ncs8801s->screen_w == 1920 && ncs8801s->screen_h == 1080) {
			pr_info("[%s] Initializing in 1920x1080p resolution.\n",
				__func__);

			ncs8801s_write_list(ncs8801s, NCS8801S_ID1_ADDR,
					    id1_1920_1080_regs);
			ncs8801s_write_list(ncs8801s, NCS8801S_ID2_ADDR,
					    id2_1920_1080_regs);
			ncs8801s_write(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0x0f,
				       0x0);

			// ncs8801s_write(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0x00, 0x00);

			// ncs8801s_write_list(ncs8801s, NCS8801S_ID3_ADDR, id3_1920_1080_regs);
			//B156HTN need
			// ncs8801s_write(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0x71, 0x9);
		} else {
			pr_info("[%s] Initializing in 1366x768 resolution.\n",
				__func__);

			ncs8801s_write_list(ncs8801s, NCS8801S_ID1_ADDR,
					    id1_1366_768_regs);
			ncs8801s_write_list(ncs8801s, NCS8801S_ID2_ADDR,
					    id2_1366_768_regs);
			ncs8801s_write(ncs8801s->i2c, NCS8801S_ID1_ADDR, 0x0f,
				       0x0);
		}
		pr_info("[%s] Init with width: %d, and height: %d\n", __func__,
			ncs8801s->screen_w, ncs8801s->screen_h);
		pr_info("[%s] Initialization completed.\n", __func__);
		// }
	}
	return 0;
}

static void ncs8801s_parse_dt(struct ncs8801s *ncs8801s)
{
	struct device_node *np = ncs8801s->i2c->dev.of_node;
	pr_info("[%s] called\n", __func__);

	// GPIO requests
	ncs8801s->hpd_pin =
		devm_gpiod_get(&ncs8801s->i2c->dev, "hpd", GPIOD_IN);
	if (IS_ERR(ncs8801s->hpd_pin)) {
		pr_err("[%s] Failed to request HPD GPIO\n", __func__);
	}

	ncs8801s->rst_pin =
		devm_gpiod_get(&ncs8801s->i2c->dev, "rst", GPIOD_OUT_LOW);
	if (IS_ERR(ncs8801s->rst_pin)) {
		pr_err("[%s] Failed to request reset GPIO\n", __func__);
	}

	ncs8801s->pwd_pin =
		devm_gpiod_get(&ncs8801s->i2c->dev, "pwd", GPIOD_OUT_HIGH);
	if (IS_ERR(ncs8801s->pwd_pin)) {
		pr_err("[%s] Failed to request power GPIO\n", __func__);
	}

	ncs8801s->edp_bl_enable_pin = devm_gpiod_get(
		&ncs8801s->i2c->dev, "edp-bl-enable", GPIOD_OUT_HIGH);
	if (IS_ERR(ncs8801s->edp_bl_enable_pin)) {
		pr_err("[%s] Failed to request backlight enable GPIO\n",
		       __func__);
	}

	// PWM request
	ncs8801s->bl_pwm = devm_pwm_get(&ncs8801s->i2c->dev, "bl-pwm");
	if (IS_ERR(ncs8801s->bl_pwm)) {
		pr_err("[%s] Failed to get PWM for backlight\n", __func__);
		ncs8801s->bl_pwm = NULL;
	}

	// if (!IS_ERR(ncs8801s->hpd_pin))
	// 	gpiod_direction_input(ncs8801s->hpd_pin);
	// if (!IS_ERR(ncs8801s->pwd_pin)) {
	// 	gpiod_direction_output(ncs8801s->pwd_pin, 1);
	// 	gpiod_set_value(ncs8801s->pwd_pin, 0);
	// }
	// if (!IS_ERR(ncs8801s->rst_pin)) {
	// 	gpiod_direction_output(ncs8801s->rst_pin, 1);
	// 	gpiod_set_value(ncs8801s->rst_pin, 0);
	// }

	// Read device tree properties
	of_property_read_u32(np, "screen-w", &ncs8801s->screen_w);
	of_property_read_u32(np, "screen-h", &ncs8801s->screen_h);
	of_property_read_u32(np, "brightness-levels",
			     &ncs8801s->max_brightness);
	of_property_read_u32(np, "default-brightness-level",
			     &ncs8801s->default_brightness);
}

static int ncs8801s_i2c_probe(struct i2c_client *i2c,
			      const struct i2c_device_id *id)
{
	int ret;
	pr_info("[%s] called\n", __func__);

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		pr_err("[%s] I2C functionality check failed.\n", __func__);
		return -ENODEV;
	}

	ncs8801s = devm_kzalloc(&i2c->dev, sizeof(struct ncs8801s), GFP_KERNEL);
	if (!ncs8801s) {
		pr_err("[%s] Failed to allocate ncs8801s struct\n", __func__);
		return -ENOMEM;
	}

	ncs8801s->i2c = i2c;
	ncs8801s_parse_dt(ncs8801s);
	ncs8801s_init();

	/* Initialize brightness to default level */
	ncs8801s_set_brightness(ncs8801s, ncs8801s->default_brightness);

	pr_info("[%s] probe completed\n", __func__);
	return 0;
}

static int ncs8801s_i2c_remove(struct i2c_client *i2c)
{
	pr_info("[%s] called\n", __func__);
	if (ncs8801s->bl_pwm)
		pwm_disable(ncs8801s->bl_pwm);

	gpiod_put(ncs8801s->rst_pin);
	gpiod_put(ncs8801s->pwd_pin);
	gpiod_put(ncs8801s->edp_bl_enable_pin);

	return 0;
}

static const struct of_device_id ncs8801s_of_match[] = {
	{ .compatible = "newcosemi,ncs8801s" },
	{}
};
MODULE_DEVICE_TABLE(of, ncs8801s_of_match);

static const struct i2c_device_id ncs8801s_id[] = { { "ncs8801s", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, ncs8801s_id);

static struct i2c_driver ncs8801s_driver = {
    .driver = {
        .name = "ncs8801s",
        .of_match_table = of_match_ptr(ncs8801s_of_match),
    },
    .probe = ncs8801s_i2c_probe,
    .remove = ncs8801s_i2c_remove,
    .id_table = ncs8801s_id,
};
module_i2c_driver(ncs8801s_driver);

MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("NCS8801S TFT Driver with Brightness Control");
MODULE_LICENSE("GPL");
