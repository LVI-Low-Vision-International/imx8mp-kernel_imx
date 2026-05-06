// SPDX-License-Identifier: GPL-2.0
/*
 * Industrial I/O driver for Microchip digital potentiometers
 * Copyright (c) 2018  Axentia Technologies AB
 * Author: Peter Rosin <peda@axentia.se>
 *
 * Datasheet: http://www.microchip.com/downloads/en/DeviceDoc/22147a.pdf
 *
 * DEVID	#Wipers	#Positions	Resistor Opts (kOhm)
 * mcp4017	1	128		5, 10, 50, 100
 * mcp4018	1	128		5, 10, 50, 100
 * mcp4019	1	128		5, 10, 50, 100
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>

#define MCP4018_WIPER_MAX 127

struct mcp4018_cfg {
	int kohms;
};

enum mcp4018_type {
	MCP4018_502,
	MCP4018_103,
	MCP4018_503,
	MCP4018_104,
};

static const struct mcp4018_cfg mcp4018_cfg[] = {
	[MCP4018_502] = { .kohms =   5, },
	[MCP4018_103] = { .kohms =  10, },
	[MCP4018_503] = { .kohms =  50, },
	[MCP4018_104] = { .kohms = 100, },
};

struct mcp4018_data {
	struct i2c_client *client;
	const struct mcp4018_cfg *cfg;
};

static const struct iio_chan_spec mcp4018_channel = {
	.type = IIO_RESISTANCE,
	.indexed = 1,
	.output = 1,
	.channel = 0,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
};

static int mcp4018_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct mcp4018_data *data = iio_priv(indio_dev);
	s32 ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = i2c_smbus_read_byte(data->client);
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 1000 * data->cfg->kohms;
		*val2 = MCP4018_WIPER_MAX;
		return IIO_VAL_FRACTIONAL;
	}

	return -EINVAL;
}

static int mcp4018_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct mcp4018_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val > MCP4018_WIPER_MAX || val < 0)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return i2c_smbus_write_byte(data->client, val);
}

static const struct iio_info mcp4018_info = {
	.read_raw = mcp4018_read_raw,
	.write_raw = mcp4018_write_raw,
};

static const struct i2c_device_id mcp4018_id[] = { { "mcp4017-502", MCP4018_502 },
						   { "mcp4017-103", MCP4018_103 },
						   { "mcp4017-503", MCP4018_503 },
						   { "mcp4017-104", MCP4018_104 },
						   { "mcp4018-502", MCP4018_502 },
						   { "mcp4018-103", MCP4018_103 },
						   { "mcp4018-503", MCP4018_503 },
						   { "mcp4018-104", MCP4018_104 },
						   { "mcp4019-502", MCP4018_502 },
						   { "mcp4019-103", MCP4018_103 },
						   { "mcp4019-503", MCP4018_503 },
						   { "mcp4019-104", MCP4018_104 },
						   {} };
MODULE_DEVICE_TABLE(i2c, mcp4018_id);

#define MCP4018_COMPATIBLE(of_compatible, cfg)                                                                                                                                                         \
	{                                                                                                                                                                                              \
		.compatible = of_compatible,                                                                                                                                                           \
		.data = &mcp4018_cfg[cfg],                                                                                                                                                             \
	}

static const struct of_device_id mcp4018_of_match[] = { MCP4018_COMPATIBLE("microchip,mcp4017-502", MCP4018_502),
							MCP4018_COMPATIBLE("microchip,mcp4017-103", MCP4018_103),
							MCP4018_COMPATIBLE("microchip,mcp4017-503", MCP4018_503),
							MCP4018_COMPATIBLE("microchip,mcp4017-104", MCP4018_104),
							MCP4018_COMPATIBLE("microchip,mcp4018-502", MCP4018_502),
							MCP4018_COMPATIBLE("microchip,mcp4018-103", MCP4018_103),
							MCP4018_COMPATIBLE("microchip,mcp4018-503", MCP4018_503),
							MCP4018_COMPATIBLE("microchip,mcp4018-104", MCP4018_104),
							MCP4018_COMPATIBLE("microchip,mcp4019-502", MCP4018_502),
							MCP4018_COMPATIBLE("microchip,mcp4019-103", MCP4018_103),
							MCP4018_COMPATIBLE("microchip,mcp4019-503", MCP4018_503),
							MCP4018_COMPATIBLE("microchip,mcp4019-104", MCP4018_104),
							{ /* sentinel */ } };
MODULE_DEVICE_TABLE(of, mcp4018_of_match);

static int mcp4018_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mcp4018_data *data;
	struct iio_dev *indio_dev;
	uint32_t initial_ohms;
	int wiper_val, ret;

	printk(KERN_INFO "[%s] probing device %s on adapter %s\n", __func__, client->name, dev_name(&client->adapter->dev));

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		printk(KERN_ERR "[%s] SMBUS Byte transfers not supported\n", __func__);
		return -EOPNOTSUPP;
	}

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev) {
		printk(KERN_ERR "[%s] devm_iio_device_alloc failed\n", __func__);
		return -ENOMEM;
	}

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;

	data->cfg = device_get_match_data(dev);
	if (!data->cfg)
		data->cfg = &mcp4018_cfg[i2c_match_id(mcp4018_id, client)->driver_data];

	printk(KERN_INFO "[%s] matched config: %d kOhm\n", __func__, data->cfg->kohms);

	/* ---- Fetch DT property and set wiper before registering ---- */
	if (!device_property_read_u32(dev, "microchip,initial-ohms", &initial_ohms)) {
		printk(KERN_INFO "[%s] requested initial resistance = %u ohms\n", __func__, initial_ohms);

		if (initial_ohms > 0 && initial_ohms <= data->cfg->kohms * 1000) {
			/* Scale ohms into raw wiper step (0..127) */
			wiper_val = DIV_ROUND_CLOSEST(initial_ohms * MCP4018_WIPER_MAX, data->cfg->kohms * 1000);

			uint8_t buf[2];
			struct i2c_client *c = data->client;

			buf[0] = 0x00; /* command register: write wiper */
			buf[1] = wiper_val; /* new wiper value */

			ret = i2c_master_send(c, buf, 2);
			if (ret < 0) {
				printk("[%s] failed to set initial resistance (%d)\n", __func__, ret);
			} else {
				printk("[%s] set initial resistance %u ohms (wiper=%d)\n", __func__, initial_ohms, wiper_val);
			}
		} else {
			printk(KERN_WARNING "[%s] Invalid initial-ohms=%u (max=%d)\n", __func__, initial_ohms, data->cfg->kohms * 1000);
		}
	} else {
		printk(KERN_INFO "[%s] no initial-ohms property found in DT\n", __func__);
	}

	/* ---- Register IIO device last ---- */
	indio_dev->info = &mcp4018_info;
	indio_dev->channels = &mcp4018_channel;
	indio_dev->num_channels = 1;
	indio_dev->name = client->name;

	printk(KERN_INFO "[%s] registering iio device '%s'\n", __func__, indio_dev->name);

	return devm_iio_device_register(dev, indio_dev);
}

static struct i2c_driver mcp4018_driver = {
	.driver = {
		.name	= "mcp4018",
		.of_match_table = mcp4018_of_match,
	},
	.probe_new	= mcp4018_probe,
	.id_table	= mcp4018_id,
};

module_i2c_driver(mcp4018_driver);

MODULE_AUTHOR("Peter Rosin <peda@axentia.se>");
MODULE_DESCRIPTION("MCP4018 digital potentiometer");
MODULE_LICENSE("GPL v2");