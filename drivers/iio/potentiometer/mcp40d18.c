// SPDX-License-Identifier: GPL-2.0
/*
 * Industrial I/O driver for Microchip MCP40D18 digital potentiometer
 * Copyright (c) 2024
 *
 * Datasheet: https://ww1.microchip.com/downloads/en/DeviceDoc/22152b.pdf
 *
 * DEVID	#Wipers	#Positions	Resistor Opts (kOhm)
 * mcp40d17	1	128		5, 10, 50, 100
 * mcp40d18	1	128		5, 10, 50, 100
 * mcp40d19	1	128		5, 10, 50, 100
 *
 * Key difference from MCP4018: MCP40D18 uses I2C with command codes
 * (SMBus 2.0 protocol), while MCP4018 uses simple single-byte I2C
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>

#define MCP40D18_WIPER_MAX 127

/* Command codes for MCP40D18 */
#define MCP40D18_CMD_WRITE	0x00
#define MCP40D18_CMD_READ	0x0C

struct mcp40d18_cfg {
	int kohms;
};

enum mcp40d18_type {
	MCP40D18_502,
	MCP40D18_103,
	MCP40D18_503,
	MCP40D18_104,
};

static const struct mcp40d18_cfg mcp40d18_cfg[] = {
	[MCP40D18_502] = { .kohms =   5, },
	[MCP40D18_103] = { .kohms =  10, },
	[MCP40D18_503] = { .kohms =  50, },
	[MCP40D18_104] = { .kohms = 100, },
};

struct mcp40d18_data {
	struct i2c_client *client;
	const struct mcp40d18_cfg *cfg;
};

static const struct iio_chan_spec mcp40d18_channel = {
	.type = IIO_RESISTANCE,
	.indexed = 1,
	.output = 1,
	.channel = 0,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
};

static int mcp40d18_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct mcp40d18_data *data = iio_priv(indio_dev);
	s32 ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* MCP40D18 uses SMBus read byte protocol with command code */
		ret = i2c_smbus_read_byte_data(data->client, MCP40D18_CMD_READ);
		if (ret < 0)
			return ret;
		*val = ret & 0x7F;  /* 7-bit value */
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 1000 * data->cfg->kohms;
		*val2 = MCP40D18_WIPER_MAX;
		return IIO_VAL_FRACTIONAL;
	}

	return -EINVAL;
}

static int mcp40d18_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct mcp40d18_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val > MCP40D18_WIPER_MAX || val < 0)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	/* MCP40D18 uses SMBus write byte protocol with command code */
	return i2c_smbus_write_byte_data(data->client, MCP40D18_CMD_WRITE, val);
}

static const struct iio_info mcp40d18_info = {
	.read_raw = mcp40d18_read_raw,
	.write_raw = mcp40d18_write_raw,
};

static const struct i2c_device_id mcp40d18_id[] = { { "mcp40d17-502", MCP40D18_502 },
						    { "mcp40d17-103", MCP40D18_103 },
						    { "mcp40d17-503", MCP40D18_503 },
						    { "mcp40d17-104", MCP40D18_104 },
						    { "mcp40d18-502", MCP40D18_502 },
						    { "mcp40d18-103", MCP40D18_103 },
						    { "mcp40d18-503", MCP40D18_503 },
						    { "mcp40d18-104", MCP40D18_104 },
						    { "mcp40d19-502", MCP40D18_502 },
						    { "mcp40d19-103", MCP40D18_103 },
						    { "mcp40d19-503", MCP40D18_503 },
						    { "mcp40d19-104", MCP40D18_104 },
						    {} };
MODULE_DEVICE_TABLE(i2c, mcp40d18_id);

#define MCP40D18_COMPATIBLE(of_compatible, cfg)                                                                                                                                                         \
	{                                                                                                                                                                                              \
		.compatible = of_compatible,                                                                                                                                                           \
		.data = &mcp40d18_cfg[cfg],                                                                                                                                                             \
	}

static const struct of_device_id mcp40d18_of_match[] = { MCP40D18_COMPATIBLE("microchip,mcp40d17-502", MCP40D18_502),
							 MCP40D18_COMPATIBLE("microchip,mcp40d17-103", MCP40D18_103),
							 MCP40D18_COMPATIBLE("microchip,mcp40d17-503", MCP40D18_503),
							 MCP40D18_COMPATIBLE("microchip,mcp40d17-104", MCP40D18_104),
							 MCP40D18_COMPATIBLE("microchip,mcp40d18-502", MCP40D18_502),
							 MCP40D18_COMPATIBLE("microchip,mcp40d18-103", MCP40D18_103),
							 MCP40D18_COMPATIBLE("microchip,mcp40d18-503", MCP40D18_503),
							 MCP40D18_COMPATIBLE("microchip,mcp40d18-104", MCP40D18_104),
							 MCP40D18_COMPATIBLE("microchip,mcp40d19-502", MCP40D18_502),
							 MCP40D18_COMPATIBLE("microchip,mcp40d19-103", MCP40D18_103),
							 MCP40D18_COMPATIBLE("microchip,mcp40d19-503", MCP40D18_503),
							 MCP40D18_COMPATIBLE("microchip,mcp40d19-104", MCP40D18_104),
							 { /* sentinel */ } };
MODULE_DEVICE_TABLE(of, mcp40d18_of_match);

static int mcp40d18_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mcp40d18_data *data;
	struct iio_dev *indio_dev;
	uint32_t initial_ohms;
	int wiper_val, ret;

	printk(KERN_INFO "[%s] probing device %s on adapter %s\n", __func__, client->name, dev_name(&client->adapter->dev));

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		printk(KERN_ERR "[%s] SMBUS Byte Data transfers not supported\n", __func__);
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
		data->cfg = &mcp40d18_cfg[i2c_match_id(mcp40d18_id, client)->driver_data];

	printk(KERN_INFO "[%s] matched config: %d kOhm\n", __func__, data->cfg->kohms);

	/* ---- Fetch DT property and set wiper before registering ---- */
	if (!device_property_read_u32(dev, "microchip,initial-ohms", &initial_ohms)) {
		printk(KERN_INFO "[%s] requested initial resistance = %u ohms\n", __func__, initial_ohms);

		if (initial_ohms > 0 && initial_ohms <= data->cfg->kohms * 1000) {
			/* Scale ohms into raw wiper step (0..127) */
			wiper_val = DIV_ROUND_CLOSEST(initial_ohms * MCP40D18_WIPER_MAX, data->cfg->kohms * 1000);

			/* MCP40D18 uses SMBus write byte with command code */
			ret = i2c_smbus_write_byte_data(data->client, MCP40D18_CMD_WRITE, wiper_val);
			
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
	indio_dev->info = &mcp40d18_info;
	indio_dev->channels = &mcp40d18_channel;
	indio_dev->num_channels = 1;
	indio_dev->name = client->name;

	printk(KERN_INFO "[%s] registering iio device '%s'\n", __func__, indio_dev->name);

	return devm_iio_device_register(dev, indio_dev);
}

static struct i2c_driver mcp40d18_driver = {
	.driver = {
		.name	= "mcp40d18",
		.of_match_table = mcp40d18_of_match,
	},
	.probe_new	= mcp40d18_probe,
	.id_table	= mcp40d18_id,
};

module_i2c_driver(mcp40d18_driver);

MODULE_AUTHOR("Based on MCP4018 driver by Peter Rosin <peda@axentia.se>");
MODULE_DESCRIPTION("MCP40D18 digital potentiometer with I2C command codes");
MODULE_LICENSE("GPL v2");