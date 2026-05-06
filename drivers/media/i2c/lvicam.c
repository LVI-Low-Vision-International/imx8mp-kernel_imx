// SPDX-License-Identifier: : GPL-2.0-only
/*
 * LVI camera bridge board
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-event.h>

#include <linux/gpio.h>
#include <linux/time.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/device.h>

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#include "tc358746_regs.h"
#include "lvicam.h"
#include <linux/lviconfig_parameters.h>

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "debug level (0-3)");

MODULE_DESCRIPTION("LVI Camera Driver");
MODULE_AUTHOR("tdb");
MODULE_LICENSE("GPL v2");

#define I2C_BUS_NUM 2
#define I2C_DEV_ADDR 0x10
#define I2C_MAX_XFER_SIZE	(512 + 2)

#define DEVICE_NAME "lvicam"
#define CLASS_NAME "lvicam"

struct lvicam_mode {
    u32 width;
    u32 height;
};

struct lvicam {
    struct v4l2_subdev sd;
    struct media_pad pad;

    struct v4l2_fract frame_interval;
    struct v4l2_mbus_framefmt fmt;
    struct mutex mutex;

    const struct lvicam_mode *curr_mode;

    struct gpio_desc *reset_gpio;
};

static const struct v4l2_mbus_framefmt tc358746_def_fmt = {
    .width			= 1920,
    .height			= 1080,
    .code			= MEDIA_BUS_FMT_UYVY8_2X8,
    .field			= V4L2_FIELD_NONE,
    .colorspace		= V4L2_COLORSPACE_DEFAULT,
    .ycbcr_enc		= V4L2_YCBCR_ENC_DEFAULT,
    .quantization	= V4L2_QUANTIZATION_DEFAULT,
    .xfer_func		= V4L2_XFER_FUNC_DEFAULT,
};

struct tc358746_mbus_fmt {
    u32 code;
    u8 bus_width;
    u8 bpp;		 		/* total bpp */
    u8 pdformat;		/* peripheral data format */
    u8 pdataf;	 		/* parallel data format option */
    u8 ppp;		 		/* pclk per pixel */
    bool csitx_only; 	/* format only in csi-tx mode supported */
};

static const struct lvicam_mode lvicam_modes[] = {
    {
        .width = 1920,
        .height = 1080,
    }
};

static const struct tc358746_mbus_fmt tc358746_formats[] = {
    {
        .code = MEDIA_BUS_FMT_UYVY8_2X8,
        .bus_width = 8,
        .bpp = 16,
        .pdformat = DATAFMT_PDFMT_YCBCRFMT_422_8_BIT,
        .pdataf = CONFCTL_PDATAF_MODE0,
        .ppp = 2,
    }
};

/* ############### LVICAM CONTROLLER STUFF ############### */

typedef struct {
    struct i2c_client *client;
    struct class *class;
    struct cdev cdev;
    struct i2c_adapter *adapter;
    unsigned int dev_num;

    uint16_t zoom;
    uint16_t zoom_min;
    uint16_t zoom_max;
    uint16_t zoom_speed;
} lvicam_controller;

lvicam_controller lvicam_ctrl;

/* Forward declarations */
static int lvicam_g_frame_interval(struct v4l2_subdev *sd,
            struct v4l2_subdev_frame_interval *ival);
static int lvicam_s_frame_interval(struct v4l2_subdev *sd,
            struct v4l2_subdev_frame_interval *ival);

/* Helpers */
static const struct tc358746_mbus_fmt *tc358746_get_format(u32 code)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(tc358746_formats); i++)
        if (tc358746_formats[i].code == code)
            return &tc358746_formats[i];

    return NULL;
}

static inline struct lvicam *to_lvicam(struct v4l2_subdev *_sd)
{
    return container_of(_sd, struct lvicam, sd);
}

/* --------------- i2c helper ------------ */

static void i2c_rd(struct v4l2_subdev *sd, u16 reg, u8 *values, u32 n)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    int err;
    u8 buf[2] = { reg >> 8, reg & 0xff };
    u8 data[I2C_MAX_XFER_SIZE];

    struct i2c_msg msgs[] = {
        {
            .addr = client->addr,
            .flags = 0,
            .len = 2,
            .buf = buf,
        },
        {
            .addr = client->addr,
            .flags = I2C_M_RD,
            .len = n,
            .buf = data,
        },
    };

    err = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
    if (err != ARRAY_SIZE(msgs)) {
        v4l2_err(sd, "%s: reading register 0x%x from 0x%x failed\n", __func__, reg, client->addr);
    }

    switch (n) {
    case 1:
        values[0] = data[0];
        break;
    case 2:
        values[0] = data[1];
        values[1] = data[0];
        break;
    case 4:
        values[0] = data[1];
        values[1] = data[0];
        values[2] = data[3];
        values[3] = data[2];
        break;
    default:
        v4l2_info(sd, "unsupported I2C read %d bytes from address 0x%04x\n", n, reg);
    }

    if (debug < 3)
        return;

    switch (n) {
    case 1:
        v4l2_info(sd, "I2C read 0x%04x = 0x%02x", reg, data[0]);
        break;
    case 2:
        v4l2_info(sd, "I2C read 0x%04x = 0x%02x%02x", reg, data[0], data[1]);
        break;
    case 4:
        v4l2_info(sd, "I2C read 0x%04x = 0x%02x%02x%02x%02x", reg, data[2], data[3], data[0], data[1]);
        break;
    default:
        v4l2_info(sd, "I2C unsupported read %d bytes from address 0x%04x\n", n, reg);
    }
}

static void i2c_wr(struct v4l2_subdev *sd, u16 reg, u8 *values, u32 n)
{
    struct i2c_client *client = v4l2_get_subdevdata(sd);
    int err;
    struct i2c_msg msg;
    u8 data[I2C_MAX_XFER_SIZE];

    if ((2 + n) > I2C_MAX_XFER_SIZE) {
        n = I2C_MAX_XFER_SIZE - 2;
        v4l2_warn(sd, "i2c wr reg=%04x: len=%d is too big!\n", reg, 2 + n);
    }

    msg.addr = client->addr;
    msg.buf = data;
    msg.len = 2 + n;
    msg.flags = 0;

    data[0] = reg >> 8;
    data[1] = reg & 0xff;

    switch (n) {
    case 1:
        data[2 + 0] = values[0];
        break;
    case 2:
        data[2 + 0] = values[1];
        data[2 + 1] = values[0];
        break;
    case 4:
        data[2 + 0] = values[1];
        data[2 + 1] = values[0];
        data[2 + 2] = values[3];
        data[2 + 3] = values[2];
        break;
    default:
        v4l2_info(sd, "unsupported I2C write %d bytes from address 0x%04x\n", n, reg);
    }

    err = i2c_transfer(client->adapter, &msg, 1);
    if (err != 1) {
        v4l2_err(sd, "%s: writing register 0x%x from 0x%x failed\n", __func__, reg, client->addr);
        return;
    }

    if (debug < 3)
        return;

    switch (n) {
    case 1:
        v4l2_info(sd, "I2C write 0x%04x = 0x%02x", reg, data[2 + 0]);
        break;
    case 2:
        v4l2_info(sd, "I2C write 0x%04x = 0x%02x%02x", reg, data[2 + 0],
              data[2 + 1]);
        break;
    case 4:
        v4l2_info(sd, "I2C write 0x%04x = 0x%02x%02x%02x%02x", reg,
              data[2 + 2], data[2 + 3], data[2 + 0], data[2 + 1]);
        break;
    default:
        v4l2_info(sd, "I2C unsupported write %d bytes from address 0x%04x\n",
              n, reg);
    }
}

static noinline u32 i2c_rdreg(struct v4l2_subdev *sd, u16 reg, u32 n)
{
    __le32 val = 0;

    i2c_rd(sd, reg, (u8 __force *)&val, n);

    return le32_to_cpu(val);
}

static noinline void i2c_wrreg(struct v4l2_subdev *sd, u16 reg, u32 val, u32 n)
{
    __le32 raw = cpu_to_le32(val);

    i2c_wr(sd, reg, (u8 __force *)&raw, n);
}

static u16 __maybe_unused i2c_rd8(struct v4l2_subdev *sd, u16 reg)
{
    return i2c_rdreg(sd, reg, 1);
}

static u16 __maybe_unused i2c_rd16(struct v4l2_subdev *sd, u16 reg)
{
    return i2c_rdreg(sd, reg, 2);
}

static u32 __maybe_unused i2c_rd32(struct v4l2_subdev *sd, u16 reg)
{
    return i2c_rdreg(sd, reg, 4);
}

static void __maybe_unused i2c_wr8(struct v4l2_subdev *sd, u16 reg, u16 val)
{
    i2c_wrreg(sd, reg, val, 1);
}

static void i2c_wr16(struct v4l2_subdev *sd, u16 reg, u16 val)
{
    i2c_wrreg(sd, reg, val, 2);
}

static void i2c_wr16_and_or(struct v4l2_subdev *sd, u16 reg, u32 mask, u16 val)
{
    u16 m = (u16)~mask;

    i2c_wrreg(sd, reg, (i2c_rd16(sd, reg) & m) | val, 2);
}

static void i2c_wr32(struct v4l2_subdev *sd, u16 reg, u32 val)
{
    i2c_wrreg(sd, reg, val, 4);
}

static void lvicam_setup(struct v4l2_subdev *sd)
{
    pr_info("[%s] call\n", __func__);

    /* Software Reset */
    i2c_wr16(sd, SYSCTL, 0x0001);
    usleep_range(10, 100);
    i2c_wr16(sd, SYSCTL, 0x0000);

    /* PLL, Clock Setting */
    i2c_wr16(sd, PLLCTL0, 0x1031);
    i2c_wr16(sd, PLLCTL1, 0x0203);
    usleep_range(1000, 2000);
    i2c_wr16(sd, PLLCTL1, 0x0213);

    /* DPI Input Control */
    i2c_wr16(sd, FIFOCTL, 0x0190);
    i2c_wr16(sd, DATAFMT, DATAFMT_PDFMT_SET(DATAFMT_PDFMT_YCBCRFMT_422_8_BIT));
    i2c_wr16(sd, WORDCNT, 0x0F00); /* 1920 * 2 = 3840 = 0x0F00 */

    /* D-PHY Setting - enable all lanes */
    i2c_wr16(sd, CLW_CNTRL, 0x0000);
    i2c_wr16(sd, 0x0142, 0x0000);
    i2c_wr16(sd, D0W_CNTRL, 0x0000);
    i2c_wr16(sd, 0x0146, 0x0000);
    i2c_wr16(sd, D1W_CNTRL, 0x0000);
    i2c_wr16(sd, 0x014A, 0x0000);
    i2c_wr16(sd, D2W_CNTRL, 0x0000);
    i2c_wr16(sd, 0x014E, 0x0000);
    i2c_wr16(sd, D3W_CNTRL, 0x0000);
    i2c_wr16(sd, 0x0152, 0x0000);

    /* CSI2-TX PPI Control */
    i2c_wr16(sd, LINEINITCNT, 0x1B58);
    i2c_wr16(sd, 0x0212, 0x0000);
    i2c_wr16(sd, LPTXTIMECNT, 0x0005);
    i2c_wr16(sd, 0x0216, 0x0000);
    i2c_wr16(sd, TCLK_HEADERCNT, 0x2304);
    i2c_wr16(sd, 0x021A, 0x0000);
    i2c_wr16(sd, TCLK_TRAILCNT, 0x0005);
	i2c_wr16(sd, 0x021E, 0x0000);
    i2c_wr16(sd, THS_HEADERCNT, 0x0705);
    i2c_wr16(sd, 0x0222, 0x0000);
    i2c_wr16(sd, TWAKEUP, 0x4E20);
    i2c_wr16(sd, 0x0226, 0x0000);
    i2c_wr16(sd, TCLK_POSTCNT, 0x000C);
	i2c_wr16(sd, 0x022A, 0x0000);
    i2c_wr16(sd, THS_TRAILCNT, 0x0005);
    i2c_wr16(sd, 0x022E, 0x0000);
    i2c_wr16(sd, HSTXVREGCNT, 0x0005);
    i2c_wr16(sd, 0x0232, 0x0000);
    i2c_wr16(sd, HSTXVREGEN, 0x001F);
    i2c_wr16(sd, 0x0236, 0x0000);
    i2c_wr16(sd, TXOPTIONCNTRL, 0x0001);
    i2c_wr16(sd, 0x023A, 0x0000);
    i2c_wr16(sd, STARTCNTRL, 0x0001);
    i2c_wr16(sd, 0x0206, 0x0000);
    i2c_wr16(sd, CSI_START, 0x0001);
    i2c_wr16(sd, 0x051A, 0x0000);

    /* Set to HS mode - 4 lanes, CSI2 mode */
    i2c_wr16(sd, CSI_CONFW, 0x8087);
    i2c_wr16(sd, 0x0502, 0xA300);
    i2c_wr16(sd, CONFCTL, 0x0143);
}

/* Ops */
static int lvicam_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg, struct v4l2_subdev_mbus_code_enum *code)
{
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] call\n", __func__);

    if (code->index >= ARRAY_SIZE(tc358746_formats))
        return -EINVAL;

    mutex_lock(&lvicam->mutex);
    code->code = tc358746_formats[code->index].code;
    mutex_unlock(&lvicam->mutex);

    return 0;
}

static int lvicam_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg, struct v4l2_subdev_format *fmt)
{
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] call\n", __func__);

    if (fmt->pad)
        return -EINVAL;

    fmt->format.code = lvicam->fmt.code;
    fmt->format.colorspace = lvicam->fmt.colorspace;
    fmt->format.field = V4L2_FIELD_NONE;

    fmt->format.width = lvicam->curr_mode->width;
    fmt->format.height = lvicam->curr_mode->height;

    return 0;
}

static int lvicam_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg, struct v4l2_subdev_format *fmt)
{
    struct lvicam *lvicam = to_lvicam(sd);
    struct v4l2_mbus_framefmt *framefmt;
    const struct lvicam_mode *mode;

    pr_info("[%s] call\n", __func__);

    mutex_lock(&lvicam->mutex);

    mode = v4l2_find_nearest_size(lvicam_modes, ARRAY_SIZE(lvicam_modes), width, height, fmt->format.width, fmt->format.height);

    fmt->format.code = lvicam->fmt.code;
    fmt->format.colorspace = lvicam->fmt.colorspace;
    fmt->format.field = V4L2_FIELD_NONE;

    fmt->format.width = mode->width;
    fmt->format.height = mode->height;

    if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
        framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
        *framefmt = fmt->format;
    } else {
        lvicam->curr_mode = mode;
    }

    lvicam->frame_interval.numerator = 1;
    lvicam->frame_interval.denominator = 60;

    mutex_unlock(&lvicam->mutex);

    return 0;
}

static int lvicam_set_stream(struct v4l2_subdev *sd, int enable)
{
    if (enable) {
        pr_info("[%s] enable:%d\n", __func__, enable);
        lvicam_setup(sd);
    } else {
        pr_info("[%s] disable:%d\n", __func__, enable);
    }

    return 0;
}

static int lvicam_enum_frame_size(struct v4l2_subdev *sd,
                  struct v4l2_subdev_pad_config *cfg,
                  struct v4l2_subdev_frame_size_enum *fse)
{
    pr_info("[%s] call\n", __func__);

    if (fse->index >= ARRAY_SIZE(lvicam_modes)) {
        pr_info("[%s] FAIL : fse-index = %d\n", __func__, fse->index);
        return -EINVAL;
    }

    fse->max_width = lvicam_modes[fse->index].width;
    fse->min_width = fse->max_width;

    fse->max_height = lvicam_modes[fse->index].height;
    fse->min_height = fse->max_height;

    pr_info("[%s] SUCCESS : width = %d, height = %d\n", __func__, fse->min_width, fse->min_height);

    return 0;
}

static int lvicam_enum_frame_interval(struct v4l2_subdev *sd,
                struct v4l2_subdev_pad_config *cfg,
                struct v4l2_subdev_frame_interval_enum *fie)
{
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] call\n", __func__);

    if (fie->index != 0)
        return -EINVAL;

    fie->code = lvicam->fmt.code;
    fie->width = lvicam_modes[0].width;
    fie->height = lvicam_modes[0].height;
    fie->interval.numerator = 1;
    fie->interval.denominator = 60;

    return 0;
}

static int lvicam_g_frame_interval(struct v4l2_subdev *sd,
            struct v4l2_subdev_frame_interval *ival)
{
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] call\n", __func__);

    ival->interval = lvicam->frame_interval;

    return 0;
}

static int lvicam_s_frame_interval(struct v4l2_subdev *sd,
                    struct v4l2_subdev_frame_interval *ival)
{
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] call\n", __func__);

    /* Enforce 60fps */
    lvicam->frame_interval.numerator = 1;
    lvicam->frame_interval.denominator = 60;
    ival->interval = lvicam->frame_interval;

    return 0;
}

static const struct v4l2_subdev_pad_ops lvicam_pad_ops = {
    .enum_mbus_code = lvicam_enum_mbus_code,
    .get_fmt = lvicam_get_fmt,
    .set_fmt = lvicam_set_fmt,
    .enum_frame_size = lvicam_enum_frame_size,
    .enum_frame_interval = lvicam_enum_frame_interval,
};

static const struct v4l2_subdev_video_ops tc358746_video_ops = {
    .g_frame_interval = lvicam_g_frame_interval,
    .s_frame_interval = lvicam_s_frame_interval,
    .s_stream = lvicam_set_stream,
};

static void lvicam_gpio_reset(struct lvicam *lvicam)
{
    pr_info("[%s] call\n", __func__);
    usleep_range(5000, 10000);
    gpiod_set_value(lvicam->reset_gpio, 1);
    usleep_range(1000, 2000);
    gpiod_set_value(lvicam->reset_gpio, 0);
    msleep(20);
}

static int lvicam_s_power(struct v4l2_subdev *sd, int on)
{
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] %d\n", __func__, on);

    lvicam_gpio_reset(lvicam);

    return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static void tc358746_print_register_map(struct v4l2_subdev *sd)
{
    v4l2_info(sd, "0x0000-0x0050: Global Register\n");
    v4l2_info(sd, "0x0056-0x0070: Rx Control Registers\n");
    v4l2_info(sd, "0x0080-0x00F8: Rx Status Registers\n");
    v4l2_info(sd, "0x0100-0x0150: Tx D-PHY Register\n");
    v4l2_info(sd, "0x0204-0x0238: Tx PPI Register\n");
    v4l2_info(sd, "0x040c-0x0518: Tx Control Register\n");
}

static int tc358746_get_reg_size(u16 address)
{
    if (address <= 0x00ff)
        return 2;
    else if ((address >= 0x0100) && (address <= 0x05FF))
        return 4;
    else
        return 1;
}

static int tc358746_g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
    if (reg->reg > 0xffff) {
        tc358746_print_register_map(sd);
        return -EINVAL;
    }

    reg->size = tc358746_get_reg_size(reg->reg);
    reg->val = i2c_rdreg(sd, reg->reg, reg->size);

    pr_info("[%s] 0x%llX=0x%llX, %d\n", __func__, reg->reg, reg->val, reg->size);

    return 0;
}

static int tc358746_s_register(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg)
{
    if (reg->reg > 0xffff) {
        tc358746_print_register_map(sd);
        return -EINVAL;
    }

    i2c_wrreg(sd, (u16)reg->reg, reg->val, tc358746_get_reg_size(reg->reg));

    return 0;
}
#endif

static int tc358746_log_status(struct v4l2_subdev *sd)
{
    struct lvicam *lvicam = to_lvicam(sd);
    uint16_t sysctl = i2c_rd16(sd, SYSCTL);

    v4l2_info(sd, "-----Chip status-----\n");
    v4l2_info(sd, "Chip ID: 0x%02lx\n", (i2c_rd16(sd, CHIPID) & CHIPID_CHIPID_MASK) >> 8);
    v4l2_info(sd, "Chip revision: 0x%02lx\n", i2c_rd16(sd, CHIPID) & CHIPID_REVID_MASK);
    v4l2_info(sd, "Sleep mode: %s\n", sysctl & SYSCTL_SLEEP_MASK ? "on" : "off");

    v4l2_info(sd, "-----CSI-TX status-----\n");
    v4l2_info(sd, "Waiting for particular sync signal: %s\n", (i2c_rd16(sd, CSI_STATUS) & CSI_STATUS_S_WSYNC_MASK) ? "yes" : "no");
    v4l2_info(sd, "Transmit mode: %s\n", (i2c_rd16(sd, CSI_STATUS) & CSI_STATUS_S_TXACT_MASK) ? "yes" : "no");
    v4l2_info(sd, "Stopped: %s\n", (i2c_rd16(sd, CSI_STATUS) & CSI_STATUS_S_HLT_MASK) ? "yes" : "no");
    v4l2_info(sd, "Color space: %s\n", lvicam->fmt.code == MEDIA_BUS_FMT_UYVY8_2X8 ? "YCbCr 422 8-bit" : "Unsupported");

    return 0;
}

static const struct v4l2_subdev_core_ops lvicam_core_ops = {
    .log_status = tc358746_log_status,
#ifdef CONFIG_VIDEO_ADV_DEBUG
    .g_register = tc358746_g_register,
    .s_register = tc358746_s_register,
#endif
    .s_power = lvicam_s_power,
    .subscribe_event = v4l2_ctrl_subdev_subscribe_event,
    .unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops lvicam_subdev_ops = {
    .core = &lvicam_core_ops,
    .video = &tc358746_video_ops,
    .pad = &lvicam_pad_ops,
};

static int lvicam_link_setup(struct media_entity *entity, const struct media_pad *local, const struct media_pad *remote, u32 flags)
{
    return 0;
}

static const struct media_entity_operations lvicam_entity_ops = {
    .link_setup = lvicam_link_setup,
    .link_validate = v4l2_subdev_link_validate,
};

/* IOCTL */

static long lvicam_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct lvicam_i2c_cmd i2c_cmd;
    int ret;

    if (copy_from_user(&i2c_cmd, (void __user *)arg, sizeof(i2c_cmd)))
        return -EFAULT;

    switch (cmd) {
    case LVICAM_CTRL_IOCTL_WRITE_DATA: {
        uint8_t buf[3] = { 0 };
        int buf_len;
        struct i2c_msg msg;

        if (i2c_cmd.size != 1 && i2c_cmd.size != 2)
            return -EINVAL;

        buf_len = 1 + i2c_cmd.size;
        buf[0] = i2c_cmd.subreg;

        if (i2c_cmd.size == 1) {
            buf[1] = (uint8_t)(i2c_cmd.data);
        } else if (i2c_cmd.size == 2) {
            buf[1] = (uint8_t)(i2c_cmd.data >> 8);
            buf[2] = (uint8_t)(i2c_cmd.data & 0xFF);
        }

        msg.addr = lvicam_ctrl.client->addr;
        msg.flags = 0;
        msg.len = buf_len;
        msg.buf = buf;

        ret = i2c_transfer(lvicam_ctrl.adapter, &msg, 1);

        pr_info("[%s] WRITE: subreg=0x%02X, size=%zu -> buf[1]=0x%02X buf[2]=0x%02X\n",
            __func__, i2c_cmd.subreg, i2c_cmd.size, buf[1], buf[2]);

        return (ret == 1) ? 0 : -EIO;
    }

    case LVICAM_CTRL_IOCTL_READ_DATA: {
        uint8_t subreg;
        uint8_t read_buf[2] = { 0 };
        struct i2c_msg msgs[2];

        if (i2c_cmd.size != 1 && i2c_cmd.size != 2)
            return -EINVAL;

        subreg = i2c_cmd.subreg;

        msgs[0].addr = lvicam_ctrl.client->addr;
        msgs[0].flags = 0;
        msgs[0].len = 1;
        msgs[0].buf = &subreg;

        msgs[1].addr = lvicam_ctrl.client->addr;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = i2c_cmd.size;
        msgs[1].buf = read_buf;

        ret = i2c_transfer(lvicam_ctrl.adapter, msgs, 2);
        if (ret != 2)
            return -EIO;

        if (i2c_cmd.size == 1) {
            i2c_cmd.data = read_buf[0];
        } else if (i2c_cmd.size == 2) {
            i2c_cmd.data = (read_buf[0] << 8) | read_buf[1];
        }

        pr_info("[%s] READ: subreg=0x%02X, size=%zu -> data=0x%02X%02X\n",
            __func__, subreg, i2c_cmd.size, read_buf[0], read_buf[1]);

        if (copy_to_user((void __user *)arg, &i2c_cmd, sizeof(i2c_cmd)))
            return -EFAULT;

        return 0;
    }

    default:
        return -ENOTTY;
    }
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = lvicam_ioctl,
};

static int lvicam_ctrl_device_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&lvicam_ctrl.dev_num, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&lvicam_ctrl.cdev, &fops);
    ret = cdev_add(&lvicam_ctrl.cdev, lvicam_ctrl.dev_num, 1);
    if (ret)
        goto unregister;

    lvicam_ctrl.class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(lvicam_ctrl.class)) {
        ret = PTR_ERR(lvicam_ctrl.class);
        goto del_cdev;
    }

    device_create(lvicam_ctrl.class, NULL, lvicam_ctrl.dev_num, NULL, DEVICE_NAME);

    lvicam_ctrl.adapter = i2c_get_adapter(I2C_BUS_NUM);
    if (!lvicam_ctrl.adapter) {
        ret = -ENODEV;
        goto destroy_device;
    }

    lvicam_ctrl.client = i2c_new_dummy_device(lvicam_ctrl.adapter, I2C_DEV_ADDR);
    if (IS_ERR(lvicam_ctrl.client)) {
        ret = PTR_ERR(lvicam_ctrl.client);
        goto put_adapter;
    }

    pr_info("[%s] Device initialized with I2C address 0x%02x\n",
        __func__, lvicam_ctrl.client->addr << 1);
    return 0;

put_adapter:
    i2c_put_adapter(lvicam_ctrl.adapter);
destroy_device:
    device_destroy(lvicam_ctrl.class, lvicam_ctrl.dev_num);
    class_destroy(lvicam_ctrl.class);
del_cdev:
    cdev_del(&lvicam_ctrl.cdev);
unregister:
    unregister_chrdev_region(lvicam_ctrl.dev_num, 1);
    return ret;
}

static void lvicam_ctrl_device_exit(void)
{
    if (lvicam_ctrl.client)
        i2c_unregister_device(lvicam_ctrl.client);
    if (lvicam_ctrl.adapter)
        i2c_put_adapter(lvicam_ctrl.adapter);
    device_destroy(lvicam_ctrl.class, lvicam_ctrl.dev_num);
    class_destroy(lvicam_ctrl.class);
    cdev_del(&lvicam_ctrl.cdev);
    unregister_chrdev_region(lvicam_ctrl.dev_num, 1);
}

/* Probe & Remove */

static int lvicam_probe(struct i2c_client *client)
{
    struct lvicam *lvicam;
    int err = 0;

    pr_info("[%s] call\n", __func__);

    lvicam = devm_kzalloc(&client->dev, sizeof(*lvicam), GFP_KERNEL);
    if (!lvicam) {
        pr_err("[%s] : ERROR 1\n", __func__);
        return -ENOMEM;
    }

    mutex_init(&lvicam->mutex);

    v4l2_i2c_subdev_init(&lvicam->sd, client, &lvicam_subdev_ops);

    v4l2_info(&lvicam->sd, "Fetching reset gpio\n");
    lvicam->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(lvicam->reset_gpio)) {
        pr_err("[%s] : ERROR 2\n", __func__);
        v4l2_err(&lvicam->sd, "Failed to get reset gpio\n");
        err = PTR_ERR(lvicam->reset_gpio);
        goto error_media_entity;
    }
    msleep(10);

    /* Check ID of the connected TC358746 */
    v4l2_info(&lvicam->sd, "Fetching device\n");
    if (((i2c_rd16(&lvicam->sd, CHIPID) & CHIPID_CHIPID_MASK) >> 8) != 0x44) {
        pr_err("[%s] : ERROR 3\n", __func__);
        v4l2_info(&lvicam->sd, "not a TC358746 on address 0x%x\n",
              client->addr << 1);
        err = -ENODEV;
        goto on_error;
    }

    lvicam->curr_mode = &lvicam_modes[0];

    /* Initialize subdev */
    lvicam->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
    lvicam->sd.entity.ops = &lvicam_entity_ops;
    lvicam->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

    /* Initialize pad */
    lvicam->pad.flags = MEDIA_PAD_FL_SOURCE;
    err = media_entity_pads_init(&lvicam->sd.entity, 1, &lvicam->pad);
    if (err) {
        pr_err("[%s] : ERROR 4\n", __func__);
        v4l2_err(&lvicam->sd, "failed to init entity pads: %d", err);
        goto on_error;
    }

    err = v4l2_async_register_subdev_sensor_common(&lvicam->sd);
    if (err < 0) {
        pr_err("[%s] : ERROR 5\n", __func__);
        v4l2_err(&lvicam->sd, "failed to register subdev: %d", err);
        goto error_media_entity;
    }

    if (lvicam->reset_gpio)
        lvicam_gpio_reset(lvicam);

    lvicam->fmt = tc358746_def_fmt;

    lvicam->frame_interval.numerator = 1;
    lvicam->frame_interval.denominator = 60;

    /* Initialize lvicam_ctrl chardev for FPGA control */
    lvicam_ctrl_device_init();

    return 0;

error_media_entity:
    pr_err("[%s] : ERROR : error_media_entity\n", __func__);
    media_entity_cleanup(&lvicam->sd.entity);

on_error:
    pr_err("[%s] : ERROR : on_error\n", __func__);
    mutex_destroy(&lvicam->mutex);
    return err;
}

static int lvicam_remove(struct i2c_client *client)
{
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    struct lvicam *lvicam = to_lvicam(sd);

    pr_info("[%s] call\n", __func__);

    lvicam_ctrl_device_exit();

    v4l2_async_unregister_subdev(sd);
    media_entity_cleanup(&sd->entity);

    mutex_destroy(&lvicam->mutex);

    return 0;
}

static const struct of_device_id __maybe_unused lvicam_of_match[] = {
    { .compatible = "lvi,lvicam" },
    {},
};
MODULE_DEVICE_TABLE(of, lvicam_of_match);

static struct i2c_driver lvicam_driver = {
    .driver = {
        .name = "lvicam",
        .of_match_table = of_match_ptr(lvicam_of_match),
    },
    .probe_new = lvicam_probe,
    .remove = lvicam_remove,
};

module_i2c_driver(lvicam_driver);