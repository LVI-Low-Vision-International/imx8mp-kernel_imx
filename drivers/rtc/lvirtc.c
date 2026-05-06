// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/rtc.h>
#include <linux/bcd.h>

#define PCF85063_REG_CTRL1        0x00
#define PCF85063_REG_TIME         0x04  // Starting address for time registers

// Define register positions
#define PCF85063_REG_SECONDS      0x04
#define PCF85063_REG_MINUTES      0x05
#define PCF85063_REG_HOURS        0x06
#define PCF85063_REG_DAYS         0x07
#define PCF85063_REG_WEEKDAYS     0x08
#define PCF85063_REG_MONTHS       0x09
#define PCF85063_REG_YEARS        0x0A

struct lvirtc_data {
    struct i2c_client *client;
    struct rtc_device *rtc;
};

static int lvirtc_read_time(struct device *dev, struct rtc_time *tm)
{
    struct i2c_client *client = to_i2c_client(dev);
    u8 data[7];
    int ret;

    // Read the time registers from the RTC
    ret = i2c_smbus_read_i2c_block_data(client, PCF85063_REG_TIME, sizeof(data), data);
    if (ret < 0) {
        dev_err(dev, "Failed to read time registers\n");
        return ret;
    }

    // Convert BCD values to binary and populate rtc_time structure
    tm->tm_sec  = bcd2bin(data[PCF85063_REG_SECONDS - PCF85063_REG_TIME] & 0x7F);
    tm->tm_min  = bcd2bin(data[PCF85063_REG_MINUTES - PCF85063_REG_TIME] & 0x7F);
    tm->tm_hour = bcd2bin(data[PCF85063_REG_HOURS - PCF85063_REG_TIME] & 0x3F);
    tm->tm_mday = bcd2bin(data[PCF85063_REG_DAYS - PCF85063_REG_TIME] & 0x3F);
    tm->tm_mon  = bcd2bin(data[PCF85063_REG_MONTHS - PCF85063_REG_TIME] & 0x1F) - 1;
    tm->tm_year = bcd2bin(data[PCF85063_REG_YEARS - PCF85063_REG_TIME]) + 100;
    tm->tm_wday = bcd2bin(data[PCF85063_REG_WEEKDAYS - PCF85063_REG_TIME] & 0x07);

    dev_info(dev, "RTC time read: %04d-%02d-%02d %02d:%02d:%02d\n",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    return 0;
}

static int lvirtc_set_time(struct device *dev, struct rtc_time *tm)
{
    struct lvirtc_data *data = dev_get_drvdata(dev);
    struct i2c_client *client = data->client;
    u8 buf[7];
    int ret;

    dev_info(dev, "Setting time: %04d-%02d-%02d %02d:%02d:%02d\n",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    buf[PCF85063_REG_SECONDS - PCF85063_REG_TIME] = bin2bcd(tm->tm_sec);
    buf[PCF85063_REG_MINUTES - PCF85063_REG_TIME] = bin2bcd(tm->tm_min);
    buf[PCF85063_REG_HOURS - PCF85063_REG_TIME] = bin2bcd(tm->tm_hour);
    buf[PCF85063_REG_DAYS - PCF85063_REG_TIME] = bin2bcd(tm->tm_mday);
    buf[PCF85063_REG_WEEKDAYS - PCF85063_REG_TIME] = tm->tm_wday;
    buf[PCF85063_REG_MONTHS - PCF85063_REG_TIME] = bin2bcd(tm->tm_mon + 1);
    buf[PCF85063_REG_YEARS - PCF85063_REG_TIME] = bin2bcd(tm->tm_year - 100);

    ret = i2c_smbus_write_i2c_block_data(client, PCF85063_REG_SECONDS, sizeof(buf), buf);
    if (ret < 0) {
        dev_err(dev, "Failed to write time registers\n");
        return ret;
    }

    return 0;
}


static const struct rtc_class_ops lvirtc_rtc_ops = {
    .read_time = lvirtc_read_time,
    .set_time = lvirtc_set_time,
};

static int lvirtc_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct lvirtc_data *data;
    int ret;

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);

    // Register RTC device
    data->rtc = devm_rtc_device_register(&client->dev, "lvirtc",
                                         &lvirtc_rtc_ops, THIS_MODULE);
    if (IS_ERR(data->rtc)) {
        dev_err(&client->dev, "Failed to register RTC device\n");
        return PTR_ERR(data->rtc);
    }

    dev_info(&client->dev, "lvirtc RTC driver probed\n");

    return 0;
}

static const struct of_device_id lvirtc_of_match[] = {
    { .compatible = "lvi,lvirtc" },
    { }
};
MODULE_DEVICE_TABLE(of, lvirtc_of_match);

static const struct i2c_device_id lvirtc_id[] = {
    { "lvirtc", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, lvirtc_id);

static struct i2c_driver lvirtc_driver = {
    .driver = {
        .name = "lvirtc",
        .of_match_table = lvirtc_of_match,
    },
    .probe = lvirtc_probe,
    .id_table = lvirtc_id,
};

module_i2c_driver(lvirtc_driver);

MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("Custom driver for PCF85063 RTC");
MODULE_LICENSE("GPL");
